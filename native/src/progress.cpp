#include "progress.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <vector>

#include "MinHook.h"
#include "log.h"

namespace coop::progress {

namespace {

// Either way: u8 list index, then the list's values (f64 each). The receiver adds the ones it lacks: donations are
// never taken back, so the union is right, and two donations within a second both stay (each side used to replace
// the whole list with the other's, and one of them was lost - with its items already taken).
constexpr uint8_t kProgressList = 90;
constexpr unsigned kEverySteps = 60;
constexpr size_t kMaxValues = 4096;

struct SharedList {
    const char* name;
    int slot;  // a global holding a ds_list id (decompiled/code/var_slots.tsv)
};
constexpr SharedList kLists[] = {
    {"museum", 0x68A},  // global.museumBundles: what was donated
};
constexpr size_t kListCount = sizeof(kLists) / sizeof(kLists[0]);

std::vector<double> g_last[kListCount];  // sorted: what both sides have, as far as this side knows
bool g_seen[kListCount] = {};
unsigned g_steps = 0;

// The museum takes two of an item instead of one (QUESTIONS.md) - the donation is shared, so the price doubles.
// In the socket menu (objSocketMenu, owner = the museum) an item with fewer than two copies cannot be put in
// (the selection is dropped for that step); with two or more, RemoveItem takes two.
constexpr uint32_t kSocketMenuStep = 0x00E787F0;   // objSocketMenu Step_0: putting the selected item in
constexpr uint32_t kScriptRemoveItem = yyc::script::RemoveItem;
constexpr int kObjMuseum = 171;                     // decompiled/data/objects.tsv
constexpr int kObjMuseumBundle = 172;
constexpr int kSlotOwner = yyc::slot::owner;
constexpr int kSlotSelected = 0x7C7;
constexpr int kSlotInventory = 0x5A4;
constexpr int kMuseumCopies = 2;
yyc::EventFn g_origSocketStep = nullptr;
yyc::ScriptFn g_origRemoveItem = nullptr;
yyc::CInstance* g_player = nullptr;  // the local player, from Tick
int g_doubleRemove = 0;

double ListId(size_t i) {
    const yyc::RValue* id = yyc::GlobalVar(kLists[i].slot);
    return id != nullptr && (id->kind & yyc::kKindMask) == yyc::kReal ? id->real : -1;
}

bool Exists(yyc::CInstance* self, double id) {
    if (id < 0) return false;
    yyc::RValue args[2] = {yyc::Real(id), yyc::Real(2)};  // ds_type_list
    yyc::RValue result{};
    yyc::CallBuiltin("ds_exists", self, 2, args, &result);
    const bool exists = yyc::ToReal(result) != 0;
    yyc::Free(&result);
    return exists;
}

double ListCall(yyc::CInstance* self, const char* builtin, std::initializer_list<double> values) {
    yyc::RValue args[3] = {};
    int argc = 0;
    for (const double v : values) args[argc++] = yyc::Real(v);
    yyc::RValue result{};
    yyc::CallBuiltin(builtin, self, argc, args, &result);
    const double value = yyc::IsNumber(result) ? yyc::ToReal(result) : NAN;
    yyc::Free(&result);
    return value;
}

// The list's values; false if one is not a number (then it is not shared: a union of numbers would not do).
bool Read(yyc::CInstance* self, double id, std::vector<double>* values) {
    const int size = static_cast<int>(ListCall(self, "ds_list_size", {id}));
    values->clear();
    for (int i = 0; i < size && values->size() < kMaxValues; ++i) {
        const double v = ListCall(self, "ds_list_find_value", {id, double(i)});
        if (!std::isfinite(v)) return false;
        values->push_back(v);
    }
    return true;
}

// Lists are compared as sets of entries: the union adds the peer's entries at the end, in another order.
std::vector<double> Sorted(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return values;
}

bool Has(const std::vector<double>& values, double v) {
    return std::find(values.begin(), values.end(), v) != values.end();
}

// Testing aid: COOP_DEBUG_MUSEUM=<item> adds that item to the museum's list ~5 s in, as a donation would.
void DebugDonate(yyc::CInstance* self) {
    static const int item = [] {
        char value[16] = {};
        return GetEnvironmentVariableA("COOP_DEBUG_MUSEUM", value, sizeof(value)) > 0 ? atoi(value) : 0;
    }();
    if (item <= 0 || g_steps != 300) return;
    const double id = ListId(0);
    if (!Exists(self, id)) return;
    yyc::RValue args[2] = {yyc::Real(id), yyc::Real(item)};
    yyc::RValue result{};
    yyc::CallBuiltin("ds_list_add", self, 2, args, &result);
    yyc::Free(&result);
    Log("progress: debug donated item %d to the museum", item);
}

double ReadReal(int id, int slot, bool* ok) {
    yyc::RValue value{};
    *ok = yyc::GetVar(id, slot, &value) && (value.kind & yyc::kKindMask) == yyc::kReal;
    const double real = *ok ? value.real : 0;
    yyc::Free(&value);
    return real;
}

// How many of the selected inventory entry the player has, or -1 if the inventory is not laid out as expected
// (then the museum is left alone).
double SelectedCount(int playerId, double slot) {
    yyc::RValue inventory{};
    if (!yyc::GetVar(playerId, kSlotInventory, &inventory)) return -1;
    double count = -1;
    if ((inventory.kind & yyc::kKindMask) == yyc::kArray) {
        yyc::RValue entry = yyc::ArrayElement(&inventory, static_cast<int>(slot));
        if ((entry.kind & yyc::kKindMask) == yyc::kArray) {
            const yyc::RValue item = yyc::ArrayElement(&entry, 0);
            const yyc::RValue amount = yyc::ArrayElement(&entry, 1);
            if ((amount.kind & yyc::kKindMask) == yyc::kReal) count = amount.real;
            static bool reported = false;
            if (!reported) {
                reported = true;
                Log("progress: museum socket: slot %.0f holds item %.0f x%.0f", slot, yyc::ToReal(item), count);
            }
        }
    }
    yyc::Free(&inventory);
    return count;
}

void __cdecl SocketStepHook(yyc::CInstance* self, yyc::CInstance* other) {
    const int menu = static_cast<int>(yyc::GetBuiltinReal(self, yyc::va::YYVar_id));
    bool ok = false;
    const double owner = ReadReal(menu, kSlotOwner, &ok);
    yyc::CInstance* target = ok ? yyc::FindInstance(static_cast<int>(owner)) : nullptr;
    const int object = target != nullptr ? static_cast<int>(yyc::GetBuiltinReal(target, yyc::va::YYVar_object_index)) : -1;
    if (!net::IsConnected() || g_player == nullptr || (object != kObjMuseum && object != kObjMuseumBundle)) {
        g_origSocketStep(self, other);
        return;
    }
    const double slot = ReadReal(menu, kSlotSelected, &ok);
    const double count = ok && slot >= 0 ? SelectedCount(static_cast<int>(yyc::GetBuiltinReal(g_player, yyc::va::YYVar_id)), slot) : -1;
    if (count >= 0 && count < kMuseumCopies) {
        yyc::RValue none = yyc::Real(-1);
        yyc::SetVar(menu, kSlotSelected, &none);
        g_origSocketStep(self, other);
        yyc::RValue back = yyc::Real(slot);
        yyc::SetVar(menu, kSlotSelected, &back);
        return;
    }
    if (count >= kMuseumCopies) ++g_doubleRemove;
    g_origSocketStep(self, other);
    if (count >= kMuseumCopies) --g_doubleRemove;
}

yyc::RValue* __cdecl RemoveItemHook(yyc::CInstance* self, yyc::CInstance* other, yyc::RValue* result, int argc,
                                    yyc::RValue** args) {
    yyc::RValue* r = g_origRemoveItem(self, other, result, argc, args);
    if (g_doubleRemove > 0) {
        yyc::Free(r);
        r = g_origRemoveItem(self, other, result, argc, args);
        Log("progress: museum took a second %.0f", argc > 0 ? yyc::ToReal(*args[0]) : -1.0);
    }
    return r;
}

}  // namespace

bool InstallHooks() {
    const bool ok = MH_CreateHook(yyc::At<void*>(kSocketMenuStep), reinterpret_cast<void*>(&SocketStepHook),
                                  reinterpret_cast<void**>(&g_origSocketStep)) == MH_OK &&
                    MH_CreateHook(yyc::At<void*>(kScriptRemoveItem), reinterpret_cast<void*>(&RemoveItemHook),
                                  reinterpret_cast<void**>(&g_origRemoveItem)) == MH_OK;
    if (!ok) Log("progress: hooks failed");
    return ok;
}

void Tick(yyc::CInstance* localPlayer) {
    g_player = localPlayer;
    ++g_steps;
    DebugDonate(localPlayer);
    if (g_steps % kEverySteps != 0) return;
    for (size_t i = 0; i < kListCount; ++i) {
        const double id = ListId(i);
        if (!Exists(localPlayer, id)) continue;
        std::vector<double> values;
        if (!Read(localPlayer, id, &values)) {
            static bool reported = false;
            if (!reported) Log("progress: %s holds something other than numbers - not shared", kLists[i].name);
            reported = true;
            continue;
        }
        if (!g_seen[i]) {  // the first look: both sides start from the same save
            g_last[i] = Sorted(values);
            g_seen[i] = true;
            continue;
        }
        if (Sorted(values) == g_last[i]) continue;
        std::vector<uint8_t> payload(1 + values.size() * sizeof(double));
        payload[0] = static_cast<uint8_t>(i);
        if (!values.empty()) memcpy(payload.data() + 1, values.data(), values.size() * sizeof(double));
        if (net::Send(kProgressList, payload.data(), payload.size())) {
            Log("progress: %s changed here (%zu entries), sent", kLists[i].name, values.size());
            g_last[i] = Sorted(values);
        }
    }
}

bool Handle(const net::Message& message, yyc::CInstance* localPlayer) {
    if (message.type != kProgressList) return false;
    const auto& p = message.payload;
    if (p.empty() || p[0] >= kListCount || (p.size() - 1) % sizeof(double) != 0) return true;
    const size_t i = p[0];
    const size_t count = (p.size() - 1) / sizeof(double);
    const double id = ListId(i);
    std::vector<double> here;
    if (count > kMaxValues || !Exists(localPlayer, id) || !Read(localPlayer, id, &here)) return true;
    size_t added = 0;
    std::vector<double> theirs;
    for (size_t k = 0; k < count; ++k) {
        double v = 0;
        memcpy(&v, p.data() + 1 + k * sizeof(double), sizeof(v));
        if (!std::isfinite(v)) continue;
        theirs.push_back(v);
        if (Has(here, v)) continue;
        ListCall(localPlayer, "ds_list_add", {id, v});
        here.push_back(v);
        ++added;
    }
    // What both have is the peer's list: anything only we have (a donation of ours it has not heard of yet) differs
    // from it, so our next look sends it.
    g_last[i] = Sorted(theirs);
    g_seen[i] = true;
    Log("progress: %s from the peer: %zu new of %zu", kLists[i].name, added, count);
    return true;
}

void Reset() {
    for (size_t i = 0; i < kListCount; ++i) {
        g_last[i].clear();
        g_seen[i] = false;
    }
}

}  // namespace coop::progress
