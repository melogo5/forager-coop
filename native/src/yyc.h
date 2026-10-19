// GameMaker 2.2.5.378 YYC runtime as compiled into Forager.exe 2.2.2.302 (x86).
// Addresses are VAs for the preferred image base; the exe has ASLR, use yyc::At().
// How each item was identified: decompiled/RUNTIME.md.
#pragma once

#include <cstdint>
#include <string>

namespace yyc {

constexpr uint32_t kImageBase = 0x400000;
constexpr uint32_t kExpectedTimestamp = 0x60148428;  // PE header of the supported build

enum RValueKind : uint32_t {
    kReal = 0,
    kString = 1,
    kArray = 2,
    kPtr = 3,
    kUndefined = 5,
    kObject = 6,
    kInt32 = 7,
    kInt64 = 10,
    kBool = 13,
    kIterator = 14,
};
constexpr uint32_t kKindMask = 0xFFFFFF;
constexpr int kNoArrayIndex = static_cast<int>(0x80000000);

#pragma pack(push, 4)
struct RValue {
    union {
        double real;
        int64_t i64;
        int32_t i32;
        void* ptr;
    };
    uint32_t flags;
    uint32_t kind;
};
#pragma pack(pop)
static_assert(sizeof(RValue) == 16, "RValue must match the runtime layout");

struct CInstance;  // +0x04 RValue* yyvars, +0x74 flags (bit0/1 = destroyed/deactivated), +0x7C object_index

// YYVAR { const char* name; int id; } — id is filled by the runtime at startup.
struct YYVar {
    const char* name;
    int id;
};

// Builtin function table entry (0x4C bytes).
struct RFunction {
    char name[64];
    void* fn;
    int argc;
    int usage;
};
static_assert(sizeof(RFunction) == 0x4C, "RFunction must match the runtime layout");

namespace va {
constexpr uint32_t YYGML_CallBuiltin = 0x016AB490;
constexpr uint32_t Variable_GetValue = 0x016CAD50;
constexpr uint32_t Variable_SetValue = 0x016CAF50;
constexpr uint32_t Variable_GetBuiltin = 0x016CA140;
constexpr uint32_t Variable_SetBuiltinDirect = 0x016CACD0;  // (CInstance*, varId, arrayIndex, RValue*)
constexpr uint32_t FindInstanceById = 0x016A52A0;           // CInstance* (int id), NULL if none
constexpr uint32_t YYSetString = 0x016ABC10;                // (RValue*, const char*) — new ref-counted string
constexpr uint32_t PerformEvent = 0x017DA800;               // (self, other, eventType, eventNumber)
constexpr uint32_t ArrayIndex = 0x004A4670;                 // thiscall RValue* (RValue* array, int index)
constexpr uint32_t FREE_RValue = 0x004012B0;
constexpr uint32_t BuiltinTable = 0x01B8C014;   // RFunction*
constexpr uint32_t GlobalObject = 0x01B7CA98;   // CInstance*
constexpr uint32_t YYVar_x = 0x01AD8738;
constexpr uint32_t YYVar_y = 0x01AD8740;
constexpr uint32_t YYVar_id = 0x01AD8770;
constexpr uint32_t YYVar_object_index = 0x01AD8780;
constexpr uint32_t YYVar_depth = 0x01AD8788;
constexpr uint32_t YYVar_sprite_index = 0x01AD8798;
constexpr uint32_t YYVar_image_index = 0x01AD87A8;
constexpr uint32_t YYVar_image_speed = 0x01AD87C0;
constexpr uint32_t YYVar_room = 0x01AD87B8;
constexpr uint32_t YYVar_image_xscale = 0x01AD8DA8;
constexpr uint32_t YYVar_image_yscale = 0x01AD8DB0;
constexpr uint32_t YYVar_keyboard_string = 0x01AD8830;
constexpr uint32_t YYVar_mouse_x = 0x01AD8B20;  // room coordinates
constexpr uint32_t YYVar_mouse_y = 0x01AD8B28;
constexpr uint32_t YYVar_bbox_left = 0x01AD8878;
constexpr uint32_t YYVar_bbox_right = 0x01AD8860;
constexpr uint32_t YYVar_bbox_top = 0x01AD8868;
constexpr uint32_t YYVar_bbox_bottom = 0x01AD8870;
constexpr uint32_t StackTraceTop = 0x01B77490;  // top GML frame {prev, name, line}

constexpr uint32_t Script_PlayerFind = 0x00736ED0;
constexpr uint32_t Script_SaveGame = 0x004FAE80;  // SaveGame() — async, writes %LOCALAPPDATA%\Forager\<global.saveFile>
constexpr uint32_t Object_objPlayer_Step_0 = 0x009A0430;
}  // namespace va

namespace slot {
constexpr int players = 0x6FE;  // decompiled/code/var_slots.tsv
constexpr int saveFile = 0x76F;  // current save slot number (real)
}

// GML script:  RValue* gml_Script_X(self, other, result, argc, args)
using ScriptFn = RValue*(__cdecl*)(CInstance* self, CInstance* other, RValue* result, int argc, RValue** args);
// Object event: void gml_Object_X_Event(self, other)
using EventFn = void(__cdecl*)(CInstance* self, CInstance* other);

extern uintptr_t g_imageDelta;

// Translates a preferred-base VA into the loaded image.
template <typename T>
T At(uint32_t address) {
    return reinterpret_cast<T>(address + g_imageDelta);
}

void InitImageDelta();
bool IsSupportedBuild();

RValue Real(double value);
double ToReal(const RValue& value);
// A real, int32 or int64: the game keeps some numbers (e.g. read from a save) as integers.
bool IsNumber(const RValue& value);
void Free(RValue* value);
// A new GML string (free it with Free once the runtime has copied it, like any other RValue).
RValue String(const char* text);
// Text of a string RValue; empty for other kinds.
std::string ToString(const RValue& value);

// Element `index` of a GML array (a plain copy: fine for numbers, not for strings/arrays).
// Reads an element. Keep `index` below the array's length: the runtime accessor grows the array otherwise,
// which broke the game (objPlayer "index out of bounds") when a station's 2-element `crafting` was read at [2].
RValue ArrayElement(RValue* array, int index);

// event_user(n) is PerformEvent(self, other, kEvOther, kEvUser0 + n).
constexpr int kEvOther = 7;
constexpr int kEvUser0 = 10;
void PerformEvent(CInstance* self, CInstance* other, int type, int number);

// Calls a builtin GML function by name; false if the name is unknown.
bool CallBuiltin(const char* name, CInstance* self, int argc, RValue* args, RValue* result);
// Native implementation of a builtin (to hook it); nullptr if unknown. The table is filled at game start.
using BuiltinFn = void(__cdecl*)(RValue* result, CInstance* self, CInstance* other, int argc, RValue* args);
BuiltinFn BuiltinAddress(const char* name);
int BuiltinCount();

CInstance* Global();
// The pointer is only valid until the next GML call (the slot array may be reallocated); fetch it again each use.
RValue* GlobalVar(int slotId);
bool GetBuiltinVar(CInstance* self, uint32_t yyvarAddress, RValue* out);
void SetBuiltinVar(CInstance* self, uint32_t yyvarAddress, RValue* value);

// Instance (non-builtin) variables by slot id; `instanceId` must exist (the runtime errors otherwise).
bool GetVar(int instanceId, int slotId, RValue* out);
void SetVar(int instanceId, int slotId, RValue* value);
// Element of an array variable: arrayIndex = i for var[i], i * 32000 + j for var[i, j].
bool GetVarAt(int instanceId, int slotId, int arrayIndex, RValue* out);
void SetVarAt(int instanceId, int slotId, int arrayIndex, RValue* value);
constexpr int kArray2dStride = 32000;
double GetBuiltinReal(CInstance* self, uint32_t yyvarAddress);
void SetBuiltinReal(CInstance* self, uint32_t yyvarAddress, double value);

// Live (not destroyed) instance with this id, or nullptr.
CInstance* FindInstance(int id);

namespace object {
constexpr int objEmpty = 0;  // no events: drawn by the default sprite draw
}

}  // namespace yyc
