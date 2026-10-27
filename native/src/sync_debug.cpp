// Testing aids of sync.cpp, each switched on by a COOP_DEBUG_* variable (tools/gametest scripts use them). They act
// through sync_internal.h, as a player's actions would be reported.
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "log.h"
#include "puppet.h"
#include "sync.h"
#include "sync_internal.h"

namespace coop::sync::internal {

namespace {

using yyc::CallReal;

// Testing aid: COOP_DEBUG_AUTOHIT=1 makes the guest hit the nearest enemy every 2 s, exactly as a tool
// hit would be reported.
// COOP_DEBUG_AUTOHIT=<object index> hits the nearest instance of that object instead (e.g. 84 = rocks).
void DebugAutoHit(yyc::CInstance* localPlayer, unsigned steps) {
    static int target = [] {
        char value[16] = {};
        if (GetEnvironmentVariableA("COOP_DEBUG_AUTOHIT", value, sizeof(value)) == 0) return 0;
        const int object = atoi(value);
        return object > 1 ? object : 23;  // "1": enemies (parEnemy)
    }();
    constexpr unsigned kEverySteps = 120;
    // Not at step 0: the first mobs update has not removed the enemies only the guest's copy of a save has yet.
    if (target == 0 || !IsGuest() || steps == 0 || steps % kEverySteps != 0) return;
    const double x = yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_x);
    const double y = yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_y);
    const int id = static_cast<int>(CallReal(localPlayer, "instance_nearest", {x, y, double(target)}));
    yyc::CInstance* enemy = id == kNoOne ? nullptr : yyc::FindInstance(id);
    if (enemy == nullptr) return;
    Hit(localPlayer, enemy);
    Log("sync: debug auto-hit on object %d", static_cast<int>(yyc::GetBuiltinReal(enemy, yyc::va::YYVar_object_index)));
}

// Testing aid: COOP_DEBUG_BUILD=<object index> makes the guest build that object next to itself ~5 s
// after entering the world, reported like a real build.
void DebugBuild(yyc::CInstance* localPlayer, unsigned steps) {
    constexpr unsigned kAtStep = 300;
    char value[16] = {};
    if (!IsGuest() || steps != kAtStep ||
        GetEnvironmentVariableA("COOP_DEBUG_BUILD", value, sizeof(value)) == 0) {
        return;
    }
    const double x = yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_x) + 32;
    const double y = yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_y);
    const int id = static_cast<int>(CallReal(localPlayer, "instance_create_depth", {x, y, -y, double(atoi(value))}));
    if (yyc::CInstance* built = yyc::FindInstance(id)) {
        const InstanceInfo info = Describe(built);
        ReportBuild(info);
        Log("sync: debug build of object %d as %d", info.object, id);
    }
}

// Testing aid: COOP_DEBUG_CULLTEST=1 (on both sides) checks destroys of objects the guest has culled, on rocks;
// COOP_DEBUG_CULLTEST=<object index> on another object (85 = trees). Both sides list those objects at the
// start; the guest deactivates the ones away from itself, as CullObject does (usually the game already has);
// the host destroys the ones away from both players; both sides list them again - the lists must match.
// `localPlayer` and every puppet.
std::vector<yyc::CInstance*> WithPuppets(yyc::CInstance* localPlayer) {
    std::vector<yyc::CInstance*> players = puppet::Instances();
    players.insert(players.begin(), localPlayer);
    return players;
}

std::vector<int> FarInstances(yyc::CInstance* self, int object, const std::vector<yyc::CInstance*>& players) {
    constexpr double kNear = 150;
    std::vector<int> distant;
    const int count = static_cast<int>(CallReal(self, "instance_number", {double(object)}));
    for (int i = 0; i < count; ++i) {
        const int id = static_cast<int>(CallReal(self, "instance_find", {double(object), double(i)}));
        yyc::CInstance* instance = yyc::FindInstance(id);
        if (instance == nullptr) continue;
        const InstanceInfo info = Describe(instance);
        const bool nearby = std::any_of(players.begin(), players.end(), [&](yyc::CInstance* player) {
            return player != nullptr && std::hypot(info.x - yyc::GetBuiltinReal(player, yyc::va::YYVar_x),
                                                   info.y - yyc::GetBuiltinReal(player, yyc::va::YYVar_y)) <= kNear;
        });
        if (!nearby) distant.push_back(id);
    }
    return distant;
}

// Listings and the host's destroys run right before the game culls (BeforeCull): objects woken up a step
// earlier are active only there.
const char* g_cullTestListing = nullptr;
bool g_cullTestDestroy = false;

int CullTestObject() {
    static const int object = [] {
        char value[16] = {};
        if (GetEnvironmentVariableA("COOP_DEBUG_CULLTEST", value, sizeof(value)) == 0) return 0;
        const int index = atoi(value);
        return index > 1 ? index : 84;  // "1": rocks
    }();
    return object;
}

void LogCullTestObjects(yyc::CInstance* self, const char* when) {
    const int object = CullTestObject();
    const int count = static_cast<int>(CallReal(self, "instance_number", {double(object)}));
    Log("sync: cull test: %d objects at the %s", count, when);
    for (int i = 0; i < count; ++i) {
        const int id = static_cast<int>(CallReal(self, "instance_find", {double(object), double(i)}));
        if (yyc::CInstance* instance = yyc::FindInstance(id)) {
            const InstanceInfo info = Describe(instance);
            Log("sync: cull test: object at (%.0f, %.0f), host id %d", info.x, info.y, HostIdOf(info.id));
        }
    }
}

void DebugCullTest(yyc::CInstance* localPlayer) {
    constexpr unsigned kStartAt = 200, kCullAt = 300, kDestroyAt = 900, kEndAt = 1500;  // steps since connecting
    const int object = CullTestObject();
    static unsigned steps = 0;
    if (object == 0 || !Connected()) return;
    ++steps;
    if (steps == kStartAt || steps == kEndAt) {
        CallReal(localPlayer, "instance_activate_object", {double(object)});
        g_cullTestListing = steps == kStartAt ? "start" : "end";
    } else if (steps == kDestroyAt && IsHost()) {
        CallReal(localPlayer, "instance_activate_object", {double(object)});
        g_cullTestDestroy = true;
    } else if (steps == kCullAt && IsGuest()) {
        const std::vector<int> distant = FarInstances(localPlayer, object, {localPlayer});
        for (const int id : distant) CallReal(localPlayer, "instance_deactivate_object", {double(id)});
        Log("sync: cull test: deactivated %zu distant objects", distant.size());
    }
}

// Testing aid: COOP_DEBUG_XP=<amount> gives the host that much XP every ~10 s (level-ups on both sides).
void DebugXp(yyc::CInstance* localPlayer, unsigned steps) {
    static const double amount = [] {
        char text[16] = {};
        return GetEnvironmentVariableA("COOP_DEBUG_XP", text, sizeof(text)) > 0 ? atof(text) : 0.0;
    }();
    if (amount <= 0 || !IsHost() || !Connected() || steps == 0 || steps % 600 != 0) return;
    Log("sync: debug gives the host %.0f XP", amount);
    GiveXp(localPlayer, amount);
}

// Testing aid: COOP_DEBUG_CRAFTDROP=<item> on the guest makes a drop of that item next to it ~5 s in, the way
// a finished craft does (inside the crafting flag), to check that it lands in the inventory.
void DebugCraftDrop(yyc::CInstance* localPlayer) {
    static int item = [] {
        char value[16] = {};
        return GetEnvironmentVariableA("COOP_DEBUG_CRAFTDROP", value, sizeof(value)) > 0 ? atoi(value) : 0;
    }();
    static unsigned steps = 0;
    if (item <= 0 || !IsGuest() || ++steps != 300) return;
    const double x = yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_x) + 24;
    const double y = yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_y);
    int id = 0;
    {
        const CraftingScope crafting;
        id = static_cast<int>(CallReal(localPlayer, "instance_create_depth", {x, y, -y, double(kParResource)}));
    }
    yyc::RValue value = yyc::Real(item);
    yyc::SetVar(id, kSlotItem, &value);
    Log("sync: debug crafted drop of item %d as %d", item, id);
}

// Testing aid: COOP_DEBUG_CENSUS=1 on the host makes both sides write their world objects (rocks, trees,
// structures, bushes; not drops) to coop_census_<pid>_<n>.txt every 30 s, so the lists can be diffed.
// Only objects near the host's player count: off-screen ones are deactivated, differently on each side.
struct CensusMsg {
    int32_t number;
    float x, y;  // the host player's position: the centre of the area both sides list
};
constexpr double kCensusRadius = 90;  // inside both cameras (half a view is ~120 px high)

bool CensusEnabled() {
    static const bool enabled = GetEnvironmentVariableA("COOP_DEBUG_CENSUS", nullptr, 0) > 0;
    return enabled;
}

void WriteCensus(yyc::CInstance* self, const CensusMsg& census) {
    const int number = census.number;
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (slash == nullptr) return;
    snprintf(slash + 1, MAX_PATH - (slash + 1 - path), "coop_census_%lu_%d.txt", GetCurrentProcessId(), number);
    FILE* file = fopen(path, "w");
    if (file == nullptr) return;
    size_t written = 0;
    for (const int parent : {80, 81}) {  // parObject, parHerb
        const int count = static_cast<int>(CallReal(self, "instance_number", {double(parent)}));
        for (int i = 0; i < count; ++i) {
            const int id = static_cast<int>(CallReal(self, "instance_find", {double(parent), double(i)}));
            yyc::CInstance* instance = yyc::FindInstance(id);
            if (instance == nullptr) continue;
            const InstanceInfo info = Describe(instance);
            if (std::hypot(info.x - census.x, info.y - census.y) > kCensusRadius) continue;
            fprintf(file, "%d %.0f %.0f %d\n", info.object, info.x, info.y, HostIdOf(info.id));
            ++written;
        }
    }
    fclose(file);
    Log("sync: census #%d: %zu objects", number, written);
}

void DebugCensus(yyc::CInstance* localPlayer, unsigned steps) {
    constexpr unsigned kEverySteps = 1800;
    static int number = 0;
    if (!IsHost() || !CensusEnabled() || !Connected() || steps % kEverySteps != 0) return;
    ++number;
    const CensusMsg census{number, static_cast<float>(yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_x)),
                           static_cast<float>(yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_y))};
    WriteCensus(localPlayer, census);
    net::Send(kCensusType, &census, sizeof(census));
}

}  // namespace

void DebugTick(yyc::CInstance* localPlayer, unsigned steps) {
    DebugAutoHit(localPlayer, steps);
    DebugBuild(localPlayer, steps);
    DebugCensus(localPlayer, steps);
    DebugCullTest(localPlayer);
    DebugCraftDrop(localPlayer);
    DebugXp(localPlayer, steps);
}

void DebugBeforeCull(yyc::CInstance* self, yyc::CInstance* localPlayer) {
    if (g_cullTestListing != nullptr) {
        LogCullTestObjects(self, g_cullTestListing);
        g_cullTestListing = nullptr;
    }
    if (g_cullTestDestroy && localPlayer != nullptr) {
        g_cullTestDestroy = false;
        const std::vector<int> distant = FarInstances(self, CullTestObject(), WithPuppets(localPlayer));
        for (const int id : distant) CallReal(self, "instance_destroy", {double(id)});
        Log("sync: cull test: destroyed %zu distant objects", distant.size());
    }
}

bool DebugHandle(const net::Message& message, yyc::CInstance* localPlayer) {
    if (message.type != kCensusType) return false;
    if (message.payload.size() == sizeof(CensusMsg) && IsGuest()) {
        CensusMsg census{};
        memcpy(&census, message.payload.data(), sizeof(census));
        WriteCensus(localPlayer, census);
    }
    return true;
}

}  // namespace coop::sync::internal
