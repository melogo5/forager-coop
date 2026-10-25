#include "yyc.h"

#include <windows.h>

#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace yyc {

uintptr_t g_imageDelta = 0;

namespace {

// The table ends with an empty name (about 2780 entries in this build; entry 2502 has a space in its name, which is
// where the count in decompiled/RUNTIME.md stopped). kMaxBuiltins only bounds the walk.
constexpr int kMaxBuiltins = 4096;
constexpr size_t kNameSize = sizeof(RFunction::name);
constexpr int kYYVarsOffset = 0x04;

using CallBuiltinFn = void(__cdecl*)(CInstance* self, CInstance* other, RValue* result, int argc, int fnIndex,
                                     RValue** args);
using GetBuiltinVarFn = int(__cdecl*)(CInstance* self, int varId, int arrayIndex, RValue* out);
using FreeFn = void(__cdecl*)(RValue* value);
using GetYYVarRefFn = RValue*(__thiscall*)(CInstance* self, int slotId);
using SetBuiltinVarFn = char(__cdecl*)(CInstance* self, int varId, int arrayIndex, RValue* value);
using FindInstanceFn = CInstance*(__cdecl*)(int id);
using VariableAccessFn = int(__cdecl*)(int instanceId, int slotId, int arrayIndex, RValue* value);
using SetStringFn =void(__cdecl*)(RValue* out, const char* text);
using PerformEventFn = void(__cdecl*)(CInstance* self, CInstance* other, int type, int number);

constexpr int kFlagsOffset = 0x74;
constexpr uint8_t kDestroyedFlag = 0x01;
constexpr uint8_t kDeactivatedFlag = 0x02;

bool IsIdentifier(const char* name) {
    if (name[0] == '\0') return false;
    for (size_t i = 0; i < kNameSize && name[i] != '\0'; ++i) {
        const char c = name[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                        c == '$';
        if (!ok) return false;
    }
    return true;
}

RFunction* Table() { return *At<RFunction**>(va::BuiltinTable); }

int ScanBuiltin(const char* name) {
    RFunction* table = Table();
    if (table == nullptr) return -1;
    // The whole table: names that are not identifiers (internal ones like "@@NewGMLArray@@") sit in the middle of
    // it, and the layer_*/tilemap_* functions come after them.
    for (int i = 0; i < kMaxBuiltins && table[i].name[0] != 0; ++i) {
        if (IsIdentifier(table[i].name) && std::strncmp(table[i].name, name, kNameSize) == 0) return i;
    }
    return -1;
}

// Game thread only. Builtins are called thousands of times a frame (the menu, every world sweep), so lookups are
// cached - without building a std::string per call: the keys are views of the names in the game's own table (or,
// for names it lacks, kept in `missing`, so a miss is not a scan of the whole table every time).
int FindBuiltin(const char* name) {
    static std::unordered_map<std::string_view, int> cache;
    static std::unordered_set<std::string> missing;
    const std::string_view key(name, strnlen(name, kNameSize));
    const auto found = cache.find(key);
    if (found != cache.end()) return found->second;
    const int index = ScanBuiltin(name);
    if (index >= 0) {
        const char* own = Table()[index].name;
        cache.emplace(std::string_view(own, strnlen(own, kNameSize)), index);
    } else {
        cache.emplace(std::string_view(*missing.insert(std::string(key)).first), -1);
    }
    return index;
}

}  // namespace

void InitImageDelta() {
    g_imageDelta = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) - kImageBase;
}

bool IsSupportedBuild() {
    const auto* base = reinterpret_cast<const BYTE*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    return nt->FileHeader.Machine == IMAGE_FILE_MACHINE_I386 && nt->FileHeader.TimeDateStamp == kExpectedTimestamp;
}

RValue Real(double value) {
    RValue v{};
    v.real = value;
    v.kind = kReal;
    return v;
}

double ToReal(const RValue& value) {
    switch (value.kind & kKindMask) {
        case kReal: return value.real;
        case kInt32: return value.i32;
        case kInt64: return static_cast<double>(value.i64);
        case kBool: return value.real;
        default: return 0.0;
    }
}

bool IsNumber(const RValue& value) {
    const uint32_t kind = value.kind & kKindMask;
    return kind == kReal || kind == kInt32 || kind == kInt64;
}

void Free(RValue* value) {
    // Same test the runtime inlines (kinds 1..4 on the unmasked value); FREE_RValue itself frees only 1..3.
    if (((value->kind - 1) & 0xFFFFFC) == 0) At<FreeFn>(va::FREE_RValue)(value);
    value->kind = kUndefined;
    value->flags = 0;
    value->i64 = 0;
}

RValue String(const char* text) {
    RValue v{};
    At<SetStringFn>(va::YYSetString)(&v, text);
    return v;
}

std::string ToString(const RValue& value) {
    if ((value.kind & kKindMask) != kString || value.ptr == nullptr) return {};
    // String RValues point at a ref-counted holder whose first field is the UTF-8 text.
    const char* text = *static_cast<const char* const*>(value.ptr);
    return text != nullptr ? std::string(text) : std::string();
}

RValue ArrayElement(RValue* array, int index) {
    using ArrayIndexFn = RValue*(__thiscall*)(RValue* array, int index);
    if ((array->kind & kKindMask) != kArray) return Real(-1);
    const RValue* element = At<ArrayIndexFn>(va::ArrayIndex)(array, index);
    return element != nullptr ? *element : Real(-1);
}

void PerformEvent(CInstance* self, CInstance* other, int type, int number) {
    At<PerformEventFn>(va::PerformEvent)(self, other, type, number);
}

int BuiltinCount() {
    RFunction* table = Table();
    if (table == nullptr) return 0;
    int count = 0;
    while (count < kMaxBuiltins && IsIdentifier(table[count].name)) ++count;
    return count;
}

std::string BuiltinsMatching(const char* part) {
    RFunction* table = Table();
    std::string out;
    int odd = 0;
    for (int i = 0; table != nullptr && i < kMaxBuiltins && table[i].name[0] != 0; ++i) {
        const std::string name(table[i].name, strnlen(table[i].name, kNameSize));
        if (!IsIdentifier(table[i].name)) ++odd;
        if (name.find(part) != std::string::npos) out += std::to_string(i) + ":" + name + " ";
    }
    return out + "(" + std::to_string(odd) + " odd names)";
}

std::string BuiltinNameAt(int index) {
    RFunction* table = Table();
    if (table == nullptr || index < 0) return "?";
    return std::string(table[index].name, strnlen(table[index].name, kNameSize));
}

bool CallBuiltin(const char* name, CInstance* self, int argc, RValue* args, RValue* result) {
    const int index = FindBuiltin(name);
    if (index < 0) return false;
    RValue* argv[16] = {};
    if (argc > 16) return false;
    for (int i = 0; i < argc; ++i) argv[i] = &args[i];
    result->kind = kUndefined;
    result->flags = 0;
    result->i64 = 0;
    At<CallBuiltinFn>(va::YYGML_CallBuiltin)(self, self, result, argc, index, argv);
    return true;
}

double CallReal(CInstance* self, const char* name, std::initializer_list<double> args) {
    RValue values[16] = {};
    int argc = 0;
    for (const double v : args) {
        if (argc == 16) break;
        values[argc++] = Real(v);
    }
    RValue result{};
    CallBuiltin(name, self, argc, values, &result);
    const double value = ToReal(result);
    Free(&result);
    return value;
}

BuiltinFn BuiltinAddress(const char* name) {
    const int index = FindBuiltin(name);
    return index < 0 ? nullptr : reinterpret_cast<BuiltinFn>(Table()[index].fn);
}

CInstance* Global() { return *At<CInstance**>(va::GlobalObject); }

RValue* GlobalVar(int slotId) {
    CInstance* global = Global();
    if (global == nullptr) return nullptr;
    auto* yyvars = *reinterpret_cast<RValue**>(reinterpret_cast<BYTE*>(global) + kYYVarsOffset);
    if (yyvars != nullptr) return &yyvars[slotId];
    // No flat slot array yet: the runtime goes through CInstance's virtual InternalGetYYVarRef (vtable[1]).
    auto vtable = *reinterpret_cast<void***>(global);
    return reinterpret_cast<GetYYVarRefFn>(vtable[1])(global, slotId);
}

bool GetBuiltinVar(CInstance* self, uint32_t yyvarAddress, RValue* out) {
    const int varId = At<YYVar*>(yyvarAddress)->id;
    out->kind = kUndefined;
    At<GetBuiltinVarFn>(va::Variable_GetBuiltin)(self, varId, kNoArrayIndex, out);
    return (out->kind & kKindMask) != kUndefined;
}

double GetBuiltinReal(CInstance* self, uint32_t yyvarAddress) {
    RValue value{};
    GetBuiltinVar(self, yyvarAddress, &value);
    const double result = ToReal(value);
    Free(&value);
    return result;
}

void SetBuiltinVar(CInstance* self, uint32_t yyvarAddress, RValue* value) {
    At<SetBuiltinVarFn>(va::Variable_SetBuiltinDirect)(self, At<YYVar*>(yyvarAddress)->id, kNoArrayIndex, value);
}

void SetBuiltinReal(CInstance* self, uint32_t yyvarAddress, double value) {
    RValue v = Real(value);
    SetBuiltinVar(self, yyvarAddress, &v);
}

bool GetVar(int instanceId, int slotId, RValue* out) { return GetVarAt(instanceId, slotId, kNoArrayIndex, out); }

void SetVar(int instanceId, int slotId, RValue* value) { SetVarAt(instanceId, slotId, kNoArrayIndex, value); }

bool GetVarAt(int instanceId, int slotId, int arrayIndex, RValue* out) {
    out->kind = kUndefined;
    return At<VariableAccessFn>(va::Variable_GetValue)(instanceId, slotId, arrayIndex, out) != 0;
}

void SetVarAt(int instanceId, int slotId, int arrayIndex, RValue* value) {
    At<VariableAccessFn>(va::Variable_SetValue)(instanceId, slotId, arrayIndex, value);
}

CInstance* FindInstance(int id) {
    CInstance* instance = At<FindInstanceFn>(va::FindInstanceById)(id);
    if (instance == nullptr) return nullptr;
    const uint8_t flags = *reinterpret_cast<const uint8_t*>(reinterpret_cast<const BYTE*>(instance) + kFlagsOffset);
    return (flags & kDestroyedFlag) != 0 ? nullptr : instance;
}

bool IsDeactivated(const CInstance* instance) {
    const uint8_t flags = *reinterpret_cast<const uint8_t*>(reinterpret_cast<const BYTE*>(instance) + kFlagsOffset);
    return (flags & kDeactivatedFlag) != 0;
}

}  // namespace yyc
