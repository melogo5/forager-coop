#include "stations.h"

#include <windows.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <set>
#include <tuple>

#include "MinHook.h"
#include "log.h"

namespace coop::stations {

namespace {

constexpr uint8_t kStation = 92;  // either way: StationMsg
constexpr unsigned kEverySteps = 30;
constexpr int kParStructure = 108;  // decompiled/data/objects.tsv
constexpr int kUiCrafting = 215;    // the crafting menu
constexpr int kSlotCrafting = 0x146;  // a station's `crafting`: [item, count], item -4 when idle (var_slots.tsv).
                                      // Read [0] only: reading past the end grows the array and breaks the game.
constexpr int kNoOne = -4;
constexpr uint32_t kScriptCreateCraftingMenu = 0x00828AB0;  // a station's Other_11 opens its menu with it

struct StationMsg {
    int32_t object;
    int32_t x, y;
    int32_t inUse;
};

using Key = std::tuple<int, int, int>;  // object, x, y: the same station on both sides
std::set<Key> g_mine;  // in use on this side, as last told to the peer
std::set<Key> g_peer;  // in use on the peer's side
unsigned g_steps = 0;
yyc::ScriptFn g_origCreateMenu = nullptr;
int g_opened = kNoOne;  // the station whose menu this side opened last (open while a crafting menu exists)

double CallReal(yyc::CInstance* self, const char* builtin, std::initializer_list<double> values) {
    yyc::RValue args[4] = {};
    int argc = 0;
    for (const double v : values) args[argc++] = yyc::Real(v);
    yyc::RValue result{};
    yyc::CallBuiltin(builtin, self, argc, args, &result);
    const double value = yyc::ToReal(result);
    yyc::Free(&result);
    return value;
}

Key KeyOf(yyc::CInstance* station) {
    return {static_cast<int>(yyc::GetBuiltinReal(station, yyc::va::YYVar_object_index)),
            static_cast<int>(std::lround(yyc::GetBuiltinReal(station, yyc::va::YYVar_x))),
            static_cast<int>(std::lround(yyc::GetBuiltinReal(station, yyc::va::YYVar_y)))};
}

bool Crafting(int station) {
    yyc::RValue crafting{};
    if (!yyc::GetVar(station, kSlotCrafting, &crafting)) return false;
    bool busy = false;
    if ((crafting.kind & yyc::kKindMask) == yyc::kArray) {
        const yyc::RValue item = yyc::ArrayElement(&crafting, 0);
        const uint32_t kind = item.kind & yyc::kKindMask;  // the item is set as an integer while crafting
        busy = (kind == yyc::kReal || kind == yyc::kInt32 || kind == yyc::kInt64) && yyc::ToReal(item) >= 0;
    }
    yyc::Free(&crafting);
    return busy;
}

// The station whose crafting menu is open here, or kNoOne.
int OpenMenuStation(yyc::CInstance* self) {
    if (g_opened == kNoOne || CallReal(self, "instance_number", {kUiCrafting}) < 1) return kNoOne;
    return g_opened;
}

void Send(const Key& key, bool inUse) {
    const StationMsg msg{std::get<0>(key), std::get<1>(key), std::get<2>(key), inUse ? 1 : 0};
    net::Send(kStation, &msg, sizeof(msg));
}

yyc::RValue* __cdecl CreateMenuHook(yyc::CInstance* self, yyc::CInstance* other, yyc::RValue* result, int argc,
                                    yyc::RValue** args) {
    if (self != nullptr && net::IsConnected() && g_peer.count(KeyOf(self)) > 0) {
        const Key key = KeyOf(self);
        Log("stations: %d at (%d, %d) is in use by the other player", std::get<0>(key), std::get<1>(key),
            std::get<2>(key));
        return result;
    }
    if (self != nullptr) g_opened = static_cast<int>(yyc::GetBuiltinReal(self, yyc::va::YYVar_id));
    return g_origCreateMenu(self, other, result, argc, args);
}

// Testing aid: COOP_DEBUG_STATION=<seconds> opens the nearest station's menu that late, as interacting does
// (its Other_11 with the player as `other`).
void DebugOpen(yyc::CInstance* localPlayer) {
    static const unsigned atStep = [] {
        char value[16] = {};
        return GetEnvironmentVariableA("COOP_DEBUG_STATION", value, sizeof(value)) > 0
                   ? static_cast<unsigned>(atoi(value)) * 60
                   : 0u;
    }();
    if (atStep == 0 || g_steps != atStep) return;
    const double x = yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_x);
    const double y = yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_y);
    const int id = static_cast<int>(CallReal(localPlayer, "instance_nearest", {x, y, kParStructure}));
    yyc::CInstance* station = id == kNoOne ? nullptr : yyc::FindInstance(id);
    if (station == nullptr) {
        Log("stations: debug: no station nearby");
        return;
    }
    const Key key = KeyOf(station);
    Log("stations: debug opens station %d at (%d, %d)", std::get<0>(key), std::get<1>(key), std::get<2>(key));
    yyc::PerformEvent(station, localPlayer, yyc::kEvOther, yyc::kEvUser0 + 1);
    Log("stations: debug: %s", g_opened == id ? "menu asked for" : "refused");
}

}  // namespace

bool InstallHooks() {
    const bool ok = MH_CreateHook(yyc::At<void*>(kScriptCreateCraftingMenu), reinterpret_cast<void*>(&CreateMenuHook),
                                  reinterpret_cast<void**>(&g_origCreateMenu)) == MH_OK;
    if (!ok) Log("stations: hook failed");
    return ok;
}

void Tick(yyc::CInstance* localPlayer) {
    ++g_steps;
    DebugOpen(localPlayer);
    if (g_steps % kEverySteps != 0) return;
    std::set<Key> now;
    const int open = OpenMenuStation(localPlayer);
    const int count = static_cast<int>(CallReal(localPlayer, "instance_number", {kParStructure}));
    for (int i = 0; i < count; ++i) {
        const int id = static_cast<int>(CallReal(localPlayer, "instance_find", {kParStructure, double(i)}));
        yyc::CInstance* station = id == kNoOne ? nullptr : yyc::FindInstance(id);
        if (station == nullptr) continue;
        if (id == open || Crafting(id)) now.insert(KeyOf(station));
    }
    for (const Key& key : now) {
        if (g_mine.count(key) == 0) Send(key, true);
    }
    for (const Key& key : g_mine) {
        if (now.count(key) == 0) Send(key, false);
    }
    g_mine.swap(now);
}

bool Handle(const net::Message& message) {
    if (message.type != kStation) return false;
    if (message.payload.size() != sizeof(StationMsg)) return true;
    StationMsg msg{};
    memcpy(&msg, message.payload.data(), sizeof(msg));
    const Key key{msg.object, msg.x, msg.y};
    if (msg.inUse != 0) {
        g_peer.insert(key);
    } else {
        g_peer.erase(key);
    }
    Log("stations: the other player %s station %d at (%d, %d)", msg.inUse != 0 ? "uses" : "left", msg.object, msg.x,
        msg.y);
    return true;
}

void Reset() {
    g_opened = kNoOne;
    g_mine.clear();
    g_peer.clear();
}

}  // namespace coop::stations
