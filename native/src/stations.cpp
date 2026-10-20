#include "stations.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <map>
#include <set>
#include <tuple>
#include <vector>

#include "MinHook.h"
#include "log.h"

namespace coop::stations {

namespace {

constexpr uint8_t kStation = 92;       // either way: StationMsg
constexpr uint8_t kStationState = 94;  // either way: a craft running at the sender's station (see SendState)
constexpr uint8_t kStationDenied = 95;  // host -> guest: someone else took that station first; leave it
constexpr unsigned kStateEverySteps = 10;
// What a station's craft and its progress bar are drawn from (var_slots.tsv): craftingOriginal, craftingTime,
// craftingTimer, craftingTimerTarget, craftingTimerVisual.
constexpr int kStateSlots[] = {0x149, 0x14C, 0x14D, 0x14E, 0x14F};
constexpr int kStateSlotCount = sizeof(kStateSlots) / sizeof(kStateSlots[0]);
constexpr int kSlotCraftingTimer = 0x14D;
constexpr int kSlotCraftingTimerVisual = 0x14F;
constexpr int kMaxCells = 8;
constexpr int kIdle = -4;
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
std::set<Key> g_peer;  // in use by another player
std::map<Key, net::PeerId> g_peerOwners;  // host: the guest using each of those
std::map<Key, int> g_crafting;  // stations crafting here (local ids), as of the last scan
std::map<Key, int> g_mirrors;   // stations showing the peer's craft here (local ids): they make nothing
std::set<Key> g_own;  // crafts started on this side (menu open when they began); see IsOwnCraft
unsigned g_steps = 0;
yyc::CInstance* g_localPlayer = nullptr;
constexpr uint32_t kScriptDialogue = 0x0072E5A0;  // Dialogue(text): a speech bubble over self
constexpr DWORD kSayEveryMs = 3000;
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
        busy = yyc::IsNumber(item) && yyc::ToReal(item) >= 0;  // the item is set as an integer while crafting
    }
    yyc::Free(&crafting);
    return busy;
}

// Length of a GML array value (0 if it is not one).
int ArrayLength(yyc::CInstance* self, yyc::RValue* array) {
    if ((array->kind & yyc::kKindMask) != yyc::kArray) return 0;
    yyc::RValue length{};
    yyc::CallBuiltin("array_length_1d", self, 1, array, &length);
    const int n = static_cast<int>(yyc::ToReal(length));
    yyc::Free(&length);
    return n;
}

void PutDouble(std::vector<uint8_t>& out, double value) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
    out.insert(out.end(), bytes, bytes + sizeof(value));
}

// Payload: object, x, y (i32), u8 cells, f64 crafting[cells], then per state slot u8 isNumber + f64.
void SendState(yyc::CInstance* self, const Key& key, int station) {
    std::vector<uint8_t> out(12);
    const int32_t head[3] = {std::get<0>(key), std::get<1>(key), std::get<2>(key)};
    memcpy(out.data(), head, sizeof(head));
    yyc::RValue crafting{};
    yyc::GetVar(station, kSlotCrafting, &crafting);
    const int cells = (std::min)(ArrayLength(self, &crafting), kMaxCells);  // only in-bounds reads
    out.push_back(static_cast<uint8_t>(cells));
    for (int i = 0; i < cells; ++i) PutDouble(out, yyc::ToReal(yyc::ArrayElement(&crafting, i)));
    yyc::Free(&crafting);
    for (const int slot : kStateSlots) {
        yyc::RValue value{};
        const bool number = yyc::GetVar(station, slot, &value) && yyc::IsNumber(value);
        out.push_back(number ? 1 : 0);
        PutDouble(out, number ? yyc::ToReal(value) : 0);
        yyc::Free(&value);
    }
    net::Send(kStationState, out.data(), out.size());
}

// The local station at `key`, or kNoOne.
int FindStation(yyc::CInstance* self, const Key& key) {
    const int count = static_cast<int>(CallReal(self, "instance_number", {kParStructure}));
    for (int i = 0; i < count; ++i) {
        const int id = static_cast<int>(CallReal(self, "instance_find", {kParStructure, double(i)}));
        yyc::CInstance* station = id == kNoOne ? nullptr : yyc::FindInstance(id);
        if (station != nullptr && KeyOf(station) == key) return id;
    }
    return kNoOne;
}

void ApplyState(yyc::CInstance* self, const std::vector<uint8_t>& p) {
    if (p.size() < 13) return;
    int32_t head[3] = {};
    memcpy(head, p.data(), sizeof(head));
    const Key key{head[0], head[1], head[2]};
    const size_t cells = p[12];
    if (cells > kMaxCells || p.size() != 13 + cells * 8 + kStateSlotCount * 9) return;
    const int station = FindStation(self, key);
    if (station == kNoOne) return;  // asleep here (off-screen): nothing to show anyway
    if (g_mirrors.count(key) == 0) {
        Log("stations: showing the other player's craft at station %d at (%d, %d)", head[0], head[1], head[2]);
    }
    g_mirrors[key] = station;
    size_t at = 13;
    for (size_t i = 0; i < cells; ++i, at += 8) {
        double v = 0;
        memcpy(&v, p.data() + at, 8);
        yyc::RValue cell = yyc::Real(v);
        yyc::SetVarAt(station, kSlotCrafting, static_cast<int>(i), &cell);
    }
    for (const int slot : kStateSlots) {
        const bool number = p[at] != 0;
        double v = 0;
        memcpy(&v, p.data() + at + 1, 8);
        at += 9;
        if (!number) continue;
        yyc::RValue value = yyc::Real(v);
        yyc::SetVar(station, slot, &value);
    }
}

// The peer's craft is over: the copy here goes back to idle.
void EndMirror(const Key& key) {
    const auto mirror = g_mirrors.find(key);
    if (mirror == g_mirrors.end()) return;
    const int station = mirror->second;
    g_mirrors.erase(mirror);
    if (yyc::FindInstance(station) == nullptr) return;
    yyc::RValue idle = yyc::Real(kIdle);
    yyc::SetVarAt(station, kSlotCrafting, 0, &idle);
    for (const int slot : {kSlotCraftingTimer, kSlotCraftingTimerVisual}) {
        yyc::RValue zero = yyc::Real(0);
        yyc::SetVar(station, slot, &zero);
    }
    Log("stations: the other player's craft at station %d at (%d, %d) is over", std::get<0>(key), std::get<1>(key),
        std::get<2>(key));
}

// Host: the guest whose craft a station showed has left; the craft goes on here from where it was (its own
// HandleCrafting runs again), so what the guest paid for is made and drops at the station.
void AdoptMirror(const Key& key) {
    const auto mirror = g_mirrors.find(key);
    if (mirror == g_mirrors.end()) return;
    const int station = mirror->second;
    g_mirrors.erase(mirror);
    if (yyc::FindInstance(station) == nullptr || !Crafting(station)) return;
    Log("stations: the craft at station %d at (%d, %d) is finished here", std::get<0>(key), std::get<1>(key),
        std::get<2>(key));
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

// Testing aid: COOP_DEBUG_IGNORE_BUSY=1 (see storage.cpp).
bool IgnoreBusy() {
    static const bool ignore = GetEnvironmentVariableA("COOP_DEBUG_IGNORE_BUSY", nullptr, 0) > 0;
    return ignore;
}

yyc::RValue* __cdecl CreateMenuHook(yyc::CInstance* self, yyc::CInstance* other, yyc::RValue* result, int argc,
                                    yyc::RValue** args) {
    if (self != nullptr && net::IsConnected() && g_peer.count(KeyOf(self)) > 0 && !IgnoreBusy()) {
        const Key key = KeyOf(self);
        Log("stations: %d at (%d, %d) is in use by the other player", std::get<0>(key), std::get<1>(key),
            std::get<2>(key));
        SayBusy(g_localPlayer);
        return result;
    }
    if (self != nullptr) g_opened = static_cast<int>(yyc::GetBuiltinReal(self, yyc::va::YYVar_id));
    return g_origCreateMenu(self, other, result, argc, args);
}

// Testing aid: COOP_DEBUG_STATION=<seconds> opens the nearest station's menu that late, as interacting does
// (its Other_11 with the player as `other`); COOP_DEBUG_STATION_OBJECT=<object> picks the nearest of that kind.
void DebugOpen(yyc::CInstance* localPlayer) {
    static const unsigned atStep = [] {
        char value[16] = {};
        return GetEnvironmentVariableA("COOP_DEBUG_STATION", value, sizeof(value)) > 0
                   ? static_cast<unsigned>(atoi(value)) * 60
                   : 0u;
    }();
    static const int object = [] {
        char value[16] = {};
        return GetEnvironmentVariableA("COOP_DEBUG_STATION_OBJECT", value, sizeof(value)) > 0 ? atoi(value)
                                                                                              : kParStructure;
    }();
    if (atStep == 0 || g_steps != atStep) return;
    const double x = yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_x);
    const double y = yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_y);
    const int id = static_cast<int>(CallReal(localPlayer, "instance_nearest", {x, y, double(object)}));
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

// Testing aid: COOP_DEBUG_CRAFT=<item> starts a craft of 3 of that item at the nearest station ~12 s in (as
// the crafting menu leaves it: crafting = [item, count]), then logs both sides' progress every 2 s.
void DebugCraft(yyc::CInstance* localPlayer) {
    static const int item = [] {
        char value[16] = {};
        return GetEnvironmentVariableA("COOP_DEBUG_CRAFT", value, sizeof(value)) > 0 ? atoi(value) : -1;
    }();
    static const bool watch = GetEnvironmentVariableA("COOP_DEBUG_CRAFTWATCH", nullptr, 0) > 0;
    if (item >= 0 && g_steps == 720) {
        const double x = yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_x);
        const double y = yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_y);
        const int id = static_cast<int>(CallReal(localPlayer, "instance_nearest", {x, y, kParStructure}));
        if (yyc::FindInstance(id) == nullptr) return;
        yyc::RValue what = yyc::Real(item), count = yyc::Real(3);
        yyc::SetVarAt(id, kSlotCrafting, 0, &what);
        yyc::SetVarAt(id, kSlotCrafting, 1, &count);
        Log("stations: debug started crafting item %d at %d", item, id);
    }
    if ((item < 0 && !watch) || g_steps % 120 != 0) return;
    const double x = yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_x);
    const double y = yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_y);
    const int id = static_cast<int>(CallReal(localPlayer, "instance_nearest", {x, y, kParStructure}));
    if (yyc::FindInstance(id) == nullptr) return;
    yyc::RValue crafting{};
    yyc::GetVar(id, kSlotCrafting, &crafting);
    const int cells = ArrayLength(localPlayer, &crafting);
    const double first = cells > 0 ? yyc::ToReal(yyc::ArrayElement(&crafting, 0)) : -99;
    const double second = cells > 1 ? yyc::ToReal(yyc::ArrayElement(&crafting, 1)) : -99;
    yyc::Free(&crafting);
    double v[kStateSlotCount] = {};
    for (int i = 0; i < kStateSlotCount; ++i) {
        yyc::RValue value{};
        yyc::GetVar(id, kStateSlots[i], &value);
        v[i] = yyc::ToReal(value);
        yyc::Free(&value);
    }
    Log("stations: debug: station %d crafting [%g, %g] (%d cells) original %g time %g timer %g target %g visual %g", id,
        first, second, cells, v[0], v[1], v[2], v[3], v[4]);
}

}  // namespace

void SayBusy(yyc::CInstance* player) {
    static DWORD last = 0;
    const DWORD now = GetTickCount();
    if (player == nullptr || (last != 0 && now - last < kSayEveryMs)) return;
    last = now;
    Say(player, "The other player is using this");
}

void Say(yyc::CInstance* player, const char* text) {
    if (player == nullptr) return;
    yyc::RValue words = yyc::String(text);
    yyc::RValue* args[1] = {&words};
    yyc::RValue result{};
    yyc::At<yyc::ScriptFn>(kScriptDialogue)(player, player, &result, 1, args);
    yyc::Free(&result);
    yyc::Free(&words);
}

bool InstallHooks() {
    const bool ok = MH_CreateHook(yyc::At<void*>(kScriptCreateCraftingMenu), reinterpret_cast<void*>(&CreateMenuHook),
                                  reinterpret_cast<void**>(&g_origCreateMenu)) == MH_OK;
    if (!ok) Log("stations: hook failed");
    return ok;
}

void Tick(yyc::CInstance* localPlayer, bool guest) {
    g_localPlayer = localPlayer;
    ++g_steps;
    DebugOpen(localPlayer);
    DebugCraft(localPlayer);
    const int menu = OpenMenuStation(localPlayer);
    if (yyc::CInstance* station = menu == kNoOne ? nullptr : yyc::FindInstance(menu)) {
        if (Crafting(menu)) g_own.insert(KeyOf(station));
    }
    if (g_steps % kStateEverySteps == 0) {
        for (const auto& [key, id] : g_crafting) {
            if (yyc::FindInstance(id) != nullptr && Crafting(id)) SendState(localPlayer, key, id);
        }
    }
    if (g_steps % kEverySteps != 0) return;
    std::set<Key> now;
    g_crafting.clear();
    const int open = OpenMenuStation(localPlayer);
    const int count = static_cast<int>(CallReal(localPlayer, "instance_number", {kParStructure}));
    for (int i = 0; i < count; ++i) {
        const int id = static_cast<int>(CallReal(localPlayer, "instance_find", {kParStructure, double(i)}));
        yyc::CInstance* station = id == kNoOne ? nullptr : yyc::FindInstance(id);
        if (station == nullptr) continue;
        const Key key = KeyOf(station);
        if (g_mirrors.count(key) > 0) continue;  // the peer's craft shown here, not ours
        const bool crafting = Crafting(id);
        if (!crafting) g_own.erase(key);
        if (crafting && guest && g_own.count(key) == 0) {
            // Guest: a craft that came with the host's world (PLAYTEST3.md #5) goes on at the host, which shows it
            // here; claiming it would make the host stop its own and hand the output to us.
            g_mirrors[key] = id;
            Log("stations: the craft at station %d at (%d, %d) came with the host's world - the host's",
                std::get<0>(key), std::get<1>(key), std::get<2>(key));
            continue;
        }
        if (crafting) g_crafting[key] = id;
        if (id == open || crafting) now.insert(key);
    }
    for (const Key& key : now) {
        if (g_mine.count(key) == 0) Send(key, true);
    }
    for (const Key& key : g_mine) {
        if (now.count(key) == 0) Send(key, false);
    }
    g_mine.swap(now);
}

bool IsOwnCraft(yyc::CInstance* station) {
    if (station == nullptr) return false;
    const int id = static_cast<int>(yyc::GetBuiltinReal(station, yyc::va::YYVar_id));
    return g_own.count(KeyOf(station)) > 0 || id == OpenMenuStation(station);
}

bool IsMirror(yyc::CInstance* station) {
    if (g_mirrors.empty() || station == nullptr) return false;
    const int id = static_cast<int>(yyc::GetBuiltinReal(station, yyc::va::YYVar_id));
    for (const auto& [key, mirror] : g_mirrors) {
        if (mirror == id) return true;
    }
    return false;
}

// Host: is the station at `key` this side's (menu open or crafting here)?
bool UsedHere(yyc::CInstance* self, const Key& key) {
    if (g_mine.count(key) > 0) return true;
    const int open = OpenMenuStation(self);
    yyc::CInstance* station = open == kNoOne ? nullptr : yyc::FindInstance(open);
    return station != nullptr && KeyOf(station) == key;
}

// Host: a guest's word about a station. Taking one the host or another guest has is refused (two players can
// click it at the same moment); leaving one only counts from the guest that has it. What stands is passed on to
// the other guests.
bool Arbitrate(yyc::CInstance* self, const net::Message& message, const StationMsg& msg, const Key& key) {
    const auto owner = g_peerOwners.find(key);
    const bool otherGuest = owner != g_peerOwners.end() && owner->second != message.from;
    if (msg.inUse != 0 && (otherGuest || UsedHere(self, key))) {
        net::SendTo(message.from, kStationDenied, &msg, sizeof(msg));
        Log("stations: guest %u took station %d at (%d, %d), which %s has - told to leave it", message.from,
            msg.object, msg.x, msg.y, otherGuest ? "another guest" : "the host");
        return false;
    }
    const bool ownsIt = owner != g_peerOwners.end() && owner->second == message.from;
    if (msg.inUse == 0 && !ownsIt) return false;  // not that guest's to free (it may have been refused it)
    net::SendExcept(message.from, kStation, &msg, sizeof(msg));
    return true;
}

// Guest: the host refused the station whose menu we just opened.
void LeaveDenied(yyc::CInstance* self, const Key& key) {
    const int open = OpenMenuStation(self);
    yyc::CInstance* station = open == kNoOne ? nullptr : yyc::FindInstance(open);
    if (station == nullptr || KeyOf(station) != key) return;
    CallReal(self, "instance_destroy", {kUiCrafting});
    g_opened = kNoOne;
    g_peer.insert(key);
    SayBusy(g_localPlayer);
    Log("stations: station %d at (%d, %d) was taken by someone else first; menu closed", std::get<0>(key),
        std::get<1>(key), std::get<2>(key));
}

bool Handle(const net::Message& message, yyc::CInstance* localPlayer) {
    if (message.type == kStationState) {
        ApplyState(localPlayer, message.payload);
        return true;
    }
    if (message.type != kStation && message.type != kStationDenied) return false;
    if (message.payload.size() != sizeof(StationMsg)) return true;
    StationMsg msg{};
    memcpy(&msg, message.payload.data(), sizeof(msg));
    const Key key{msg.object, msg.x, msg.y};
    if (message.type == kStationDenied) {
        LeaveDenied(localPlayer, key);
        return true;
    }
    if (message.from != net::kHostPeer && !Arbitrate(localPlayer, message, msg, key)) return true;
    if (msg.inUse != 0) {
        g_peer.insert(key);
        g_peerOwners[key] = message.from;
    } else {
        g_peer.erase(key);
        g_peerOwners.erase(key);
        EndMirror(key);
    }
    Log("stations: the other player %s station %d at (%d, %d)", msg.inUse != 0 ? "uses" : "left", msg.object, msg.x,
        msg.y);
    return true;
}

void OnPeerJoined(net::PeerId peer) {
    std::set<Key> inUse = g_mine;
    inUse.insert(g_peer.begin(), g_peer.end());
    for (const Key& key : inUse) {
        const StationMsg msg{std::get<0>(key), std::get<1>(key), std::get<2>(key), 1};
        net::SendTo(peer, kStation, &msg, sizeof(msg));
    }
    if (!inUse.empty()) Log("stations: told guest %u about %zu stations in use", peer, inUse.size());
}

void OnPeerLeft(net::PeerId peer) {
    std::vector<Key> freed;
    for (const auto& [key, owner] : g_peerOwners) {
        if (owner == peer) freed.push_back(key);
    }
    for (const Key& key : freed) {
        g_peer.erase(key);
        g_peerOwners.erase(key);
        AdoptMirror(key);
        Send(key, false);  // the other guests heard it was in use (if it crafts on, the host's Tick says so)
        Log("stations: station %d at (%d, %d) was used by guest %u, who left; free again", std::get<0>(key),
            std::get<1>(key), std::get<2>(key), peer);
    }
}

void Reset() {
    g_localPlayer = nullptr;
    g_peerOwners.clear();
    g_opened = kNoOne;
    g_mine.clear();
    g_peer.clear();
    g_crafting.clear();
    g_mirrors.clear();
    g_own.clear();
}

}  // namespace coop::stations
