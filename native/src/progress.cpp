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

// Either way: u8 grid index, u16 width, u16 height, then the cells column by column (f64 each). The receiver keeps
// the larger value of each cell: donations only ever add, so two donations within a second both stay (each side used
// to replace the whole thing with the other's, and one of them was lost - with its items already taken).
constexpr uint8_t kProgressGrid = 90;
constexpr unsigned kEverySteps = 60;
constexpr int kMaxCells = 4096;
constexpr double kDsTypeGrid = 5;

struct SharedGrid {
    const char* name;
    int slot;  // a global holding a ds_grid id (decompiled/code/var_slots.tsv)
};
// global.museumBundles: a column per bundle (objMuseumBundle `number`): row i how many of its item i are in, row 10
// set once it is complete (the save reads it with ds_grid_read). It was taken for a ds_list until playtest 6, so the
// museum was never shared: ds_list calls on its id reached whatever list had the same number.
constexpr SharedGrid kGrids[] = {
    {"museum", 0x68A},
};
constexpr size_t kGridCount = sizeof(kGrids) / sizeof(kGrids[0]);

struct Cells {
    int width = 0, height = 0;
    std::vector<double> values;  // column by column
    bool operator==(const Cells& other) const {
        return width == other.width && height == other.height && values == other.values;
    }
};

Cells g_last[kGridCount];  // what both sides have, as far as this side knows
bool g_seen[kGridCount] = {};
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

double GridId(size_t i) {
    const yyc::RValue* id = yyc::GlobalVar(kGrids[i].slot);
    return id != nullptr && yyc::IsNumber(*id) ? yyc::ToReal(*id) : -1;
}

bool Exists(yyc::CInstance* self, double id) {
    if (id < 0) return false;
    yyc::RValue args[2] = {yyc::Real(id), yyc::Real(kDsTypeGrid)};
    yyc::RValue result{};
    yyc::CallBuiltin("ds_exists", self, 2, args, &result);
    const bool exists = yyc::ToReal(result) != 0;
    yyc::Free(&result);
    return exists;
}

double GridCall(yyc::CInstance* self, const char* builtin, std::initializer_list<double> values) {
    yyc::RValue args[4] = {};
    int argc = 0;
    for (const double v : values) args[argc++] = yyc::Real(v);
    yyc::RValue result{};
    yyc::CallBuiltin(builtin, self, argc, args, &result);
    const double value = yyc::IsNumber(result) ? yyc::ToReal(result) : NAN;
    yyc::Free(&result);
    return value;
}

// The grid's cells; false if one is not a number (then it is not shared: a maximum of numbers would not do).
bool Read(yyc::CInstance* self, double id, Cells* cells) {
    cells->width = static_cast<int>(GridCall(self, "ds_grid_width", {id}));
    cells->height = static_cast<int>(GridCall(self, "ds_grid_height", {id}));
    cells->values.clear();
    if (cells->width <= 0 || cells->height <= 0 || cells->width * cells->height > kMaxCells) return false;
    for (int x = 0; x < cells->width; ++x) {
        for (int y = 0; y < cells->height; ++y) {
            const double v = GridCall(self, "ds_grid_get", {id, double(x), double(y)});
            if (!std::isfinite(v)) return false;
            cells->values.push_back(v);
        }
    }
    return true;
}

// Testing aid: COOP_DEBUG_MUSEUM=<bundle> puts one more of that bundle's first item in ~5 s in, as a donation would.
void DebugDonate(yyc::CInstance* self) {
    static const int bundle = [] {
        char value[16] = {};
        return GetEnvironmentVariableA("COOP_DEBUG_MUSEUM", value, sizeof(value)) > 0 ? atoi(value) : -1;
    }();
    if (bundle < 0 || g_steps != 300) return;
    const double id = GridId(0);
    if (!Exists(self, id)) return;
    const double count = GridCall(self, "ds_grid_get", {id, double(bundle), 0});
    GridCall(self, "ds_grid_set", {id, double(bundle), 0, (std::isfinite(count) ? count : 0) + 1});
    Log("progress: debug donated to museum bundle %d", bundle);
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
    for (size_t i = 0; i < kGridCount; ++i) {
        const double id = GridId(i);
        if (!Exists(localPlayer, id)) continue;
        Cells cells;
        if (!Read(localPlayer, id, &cells)) {
            static bool reported = false;
            if (!reported) Log("progress: %s holds something other than numbers - not shared", kGrids[i].name);
            reported = true;
            continue;
        }
        if (!g_seen[i]) {  // the first look: both sides start from the same save
            g_last[i] = cells;
            g_seen[i] = true;
            continue;
        }
        if (cells == g_last[i]) continue;
        std::vector<uint8_t> payload(5 + cells.values.size() * sizeof(double));
        payload[0] = static_cast<uint8_t>(i);
        const uint16_t size[2] = {static_cast<uint16_t>(cells.width), static_cast<uint16_t>(cells.height)};
        memcpy(payload.data() + 1, size, sizeof(size));
        memcpy(payload.data() + 5, cells.values.data(), cells.values.size() * sizeof(double));
        if (net::Send(kProgressGrid, payload.data(), payload.size())) {
            Log("progress: %s changed here (%dx%d), sent", kGrids[i].name, cells.width, cells.height);
            g_last[i] = cells;
        }
    }
}

bool Handle(const net::Message& message, yyc::CInstance* localPlayer) {
    if (message.type != kProgressGrid) return false;
    const auto& p = message.payload;
    if (p.size() < 5 || p[0] >= kGridCount) return true;
    const size_t i = p[0];
    uint16_t size[2] = {};
    memcpy(size, p.data() + 1, sizeof(size));
    Cells theirs{size[0], size[1], std::vector<double>(static_cast<size_t>(size[0]) * size[1])};
    if (p.size() != 5 + theirs.values.size() * sizeof(double)) return true;
    memcpy(theirs.values.data(), p.data() + 5, theirs.values.size() * sizeof(double));
    const double id = GridId(i);
    Cells here;
    if (!Exists(localPlayer, id) || !Read(localPlayer, id, &here)) return true;
    if (here.width != theirs.width || here.height != theirs.height) {
        Log("progress: %s from the peer is %dx%d, here %dx%d - not taken", kGrids[i].name, theirs.width,
            theirs.height, here.width, here.height);
        return true;
    }
    int raised = 0;
    for (int x = 0; x < here.width; ++x) {
        for (int y = 0; y < here.height; ++y) {
            const size_t k = static_cast<size_t>(x) * here.height + y;
            if (!std::isfinite(theirs.values[k]) || theirs.values[k] <= here.values[k]) continue;
            GridCall(localPlayer, "ds_grid_set", {id, double(x), double(y), theirs.values[k]});
            ++raised;
        }
    }
    // What both have is the peer's grid: a cell only we raised (a donation of ours it has not heard of yet) differs
    // from it, so our next look sends it.
    g_last[i] = theirs;
    g_seen[i] = true;
    Log("progress: %s from the peer: %d cells raised here", kGrids[i].name, raised);
    return true;
}

void Reset() {
    for (size_t i = 0; i < kGridCount; ++i) {
        g_last[i] = Cells{};
        g_seen[i] = false;
    }
}

}  // namespace coop::progress
