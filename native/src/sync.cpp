#include "sync.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "MinHook.h"
#include "log.h"
#include "puppet.h"
#include "skilltab.h"
#include "stations.h"

namespace coop::sync {

namespace {

enum MessageType : uint8_t {
    kSpawn = 10,           // host -> guest
    kDestroy = 11,         // host -> guest
    kHit = 12,             // guest -> host
    kXp = 13,              // guest -> host: XP the guest earned goes to the shared progress (the host's)
    kGuestDestroyed = 14,  // guest -> host
    kGuestSpawn = 15,      // guest -> host: something the guest built/placed (id = the guest's instance)
    kGuestSpawnAck = 16,   // host -> guest: id = the host's instance, item = the guest's instance
    kObjectHp = 17,        // host -> guest: health of a world object after a hit (HpMsg)
    kProgress = 19,        // host -> guest: the shared level/XP/skill points (ProgressMsg)
    kCensus = 18,          // host -> guest (testing aid): write down your world objects now, as I just did
};
constexpr int kSlotHp = 0x573;
constexpr int kSlotHpAlpha = 0x574;
constexpr int kSlotHpLast = 0x575;
constexpr int kSlotHpWhite = 0x577;
constexpr int kSlotDmg = 0x197;  // objPlayer.dmg: what a hit takes off an object (parObject Other_10)

#pragma pack(push, 1)
struct ObjectMsg {
    int32_t id;  // the host's instance id; -1 when the sender does not know it
    int32_t object;
    float x, y;
    float depth;    // kSpawn: the depth; kHit: the guest's `dmg` (its tool's damage)
    int32_t item;   // kSpawn of a drop: its `item`; otherwise kNoItem
    int32_t owner;  // kSpawn of a drop: who may collect it (kOwnerAnyone/Host/Guest - the receiving guest)
    float sprite;   // kSpawn: the look it rolled after creation (ore vs plain rock...); -1 = keep
    float image;
};
struct HpMsg {
    int32_t id;  // the host's instance id
    int32_t object;
    float x, y;
    float hp, alpha, last, white;  // what the health bar is drawn from
};
constexpr int32_t kOwnerAnyone = 0;
constexpr int32_t kOwnerHost = 1;
constexpr int32_t kOwnerGuest = 2;
struct XpMsg {
    float amount;
};

struct ProgressMsg {
    float level, xp, xpCap, xpToGain, skillPoints;
};
#pragma pack(pop)

// Parents of the objects that make up the world (decompiled/data/objects.tsv).
constexpr int kSyncedParents[] = {
    80,   // parObject: rocks, trees, structures, special objects
    81,   // parHerb: bushes, flowers
    281,  // parResource: drops on the ground
    23,   // parEnemy
    115,  // objBridge: no parent (PLAYTEST.md #10)
    64,   // parCritter: fireflies, butterflies, cows... (streamed by mobs like enemies; PLAYTEST.md #15)
};
constexpr int kParResource = 281;  // drops; `item` says what they give
constexpr int kSlotItem = 0x5B8;   // decompiled/code/var_slots.tsv
constexpr int kSlotKilledByPlayer = 0x5DF;
constexpr uint32_t kYYVarRoomSpeed = 0x01AD8750;
constexpr int32_t kNoItem = -1;
constexpr uint32_t kResourceCollect = 0x00FF5580;  // parResource Other_10: collected by `other`
constexpr int kMaxObjects = 512;
constexpr double kMatchDistance = 4;  // pixels; objects loaded from the same save sit on the same spot
constexpr int kNoOne = -4;

constexpr uint32_t kScriptResourceSpawn = 0x00749E80;
constexpr uint32_t kScriptResourceSpawnSingle = 0x00750710;
constexpr uint32_t kScriptDirtProcess = 0x004BEB20;  // dirt left by broken objects grows herbs (wheat, cotton...)
constexpr uint32_t kScriptGainXp = 0x00730600;
constexpr uint32_t kScriptUnlockAchievement = 0x004A6DA0;  // UnlockAchievement(name): Steam achievement
constexpr uint32_t kScriptHandleCrafting = 0x004AB790;  // a structure works on its craft; DropItem when done
constexpr uint32_t kEventUser = 0x016F2D00;  // event_user(n): (self, other, n)

using EventUserFn = void(__cdecl*)(yyc::CInstance* self, yyc::CInstance* other, int n);

bool (*g_isHost)() = nullptr;
bool (*g_isGuest)() = nullptr;
yyc::CInstance* g_localPlayer = nullptr;
bool g_builtinsHooked = false;
int g_applyingRemote = 0;  // >0 while carrying out a peer's change: do not report it back
bool g_dryHit = false;     // guest: inside a hit shown locally only (DryHit)

int8_t g_syncedCache[kMaxObjects] = {};  // 0 unknown, 1 synced, -1 not
std::unordered_map<int, int> g_hostToLocal;  // guest: host instance id -> replica id
std::unordered_map<int, int> g_localToHost;
std::unordered_map<int, net::PeerId> g_hitByGuest;  // host: instances a guest hit last, and which guest
std::unordered_set<int> g_destroyedOnHost;  // guest: host ids reported destroyed (ids are never reused)

yyc::ScriptFn g_origResourceSpawn = nullptr;
yyc::ScriptFn g_origResourceSpawnSingle = nullptr;
yyc::ScriptFn g_origDirtProcess = nullptr;
yyc::ScriptFn g_origGainXp = nullptr;
yyc::ScriptFn g_origHandleCrafting = nullptr;
yyc::ScriptFn g_origUnlockAchievement = nullptr;
EventUserFn g_origEventUser = nullptr;
yyc::BuiltinFn g_origCreate = nullptr;
using DestroyFn = void(__cdecl*)(yyc::CInstance* self, yyc::CInstance* other, int id, int executeEvent);
constexpr uint32_t kInstanceDestroy = 0x016E6BA0;
DestroyFn g_origDestroy = nullptr;

// The YYC call stack: one frame per running GML function.
struct StackFrame {
    StackFrame* prev;
    const char* name;
    int line;
};

bool IsHost() { return g_isHost != nullptr && g_isHost(); }
bool IsGuest() { return g_isGuest != nullptr && g_isGuest(); }
bool Connected() { return net::IsConnected(); }

double Builtin(yyc::CInstance* self, const char* name, std::initializer_list<double> args) {
    std::vector<yyc::RValue> values;
    for (const double a : args) values.push_back(yyc::Real(a));
    yyc::RValue result{};
    yyc::CallBuiltin(name, self, static_cast<int>(values.size()), values.data(), &result);
    const double value = yyc::ToReal(result);
    yyc::Free(&result);
    return value;
}

bool IsSynced(yyc::CInstance* self, int object) {
    if (object < 0 || object >= kMaxObjects) return false;
    int8_t& cached = g_syncedCache[object];
    if (cached == 0) {
        cached = -1;
        for (const int parent : kSyncedParents) {
            if (object == parent || Builtin(self, "object_is_ancestor", {double(object), double(parent)}) != 0) {
                cached = 1;
                break;
            }
        }
    }
    return cached == 1;
}

struct InstanceInfo {
    int id;
    int object;
    double x, y, depth;
};

InstanceInfo Describe(yyc::CInstance* instance) {
    return {static_cast<int>(yyc::GetBuiltinReal(instance, yyc::va::YYVar_id)),
            static_cast<int>(yyc::GetBuiltinReal(instance, yyc::va::YYVar_object_index)),
            yyc::GetBuiltinReal(instance, yyc::va::YYVar_x), yyc::GetBuiltinReal(instance, yyc::va::YYVar_y),
            yyc::GetBuiltinReal(instance, yyc::va::YYVar_depth)};
}

unsigned g_sent[16] = {};
unsigned g_applied[16] = {};
unsigned g_steps = 0;
constexpr unsigned kStatsEverySteps = 600;

// To everyone, or only to guest `to` when it is set.
void Send(MessageType type, const InstanceInfo& info, int id, int32_t item = kNoItem,
          int32_t owner = kOwnerAnyone, int to = -1) {
    ObjectMsg msg{id, info.object, static_cast<float>(info.x), static_cast<float>(info.y),
                  static_cast<float>(info.depth), item, owner, -1, 0};
    if (type == kSpawn) {
        if (yyc::CInstance* instance = yyc::FindInstance(id)) {
            msg.sprite = static_cast<float>(yyc::GetBuiltinReal(instance, yyc::va::YYVar_sprite_index));
            msg.image = static_cast<float>(yyc::GetBuiltinReal(instance, yyc::va::YYVar_image_index));
        }
    }
    const bool sent = to < 0 ? net::Send(type, &msg, sizeof(msg))
                             : net::SendTo(static_cast<net::PeerId>(to), type, &msg, sizeof(msg));
    if (sent) ++g_sent[type & 15];
}

bool IsDrop(yyc::CInstance* self, int object) {
    return object == kParResource ||
           (object >= 0 && Builtin(self, "object_is_ancestor", {double(object), double(kParResource)}) != 0);
}

yyc::EventFn g_origCollect = nullptr;  // parResource Other_10: `other` collects the drop

int g_suppressing = 0;          // guest: inside BeginSuppressSpawns/EndSuppressSpawns
std::vector<int> g_suppressed;  // guest: world objects created meanwhile

// The game's own pickup, run right away for `player` (no flying-to-the-player phase on the ground).
void CollectNow(yyc::CInstance* drop, yyc::CInstance* player) {
    ++g_applyingRemote;  // its destroy is ours, not news for the peer
    g_origCollect(drop, player);
    --g_applyingRemote;
}

// Drops belong to whoever broke/killed what they came from: only that player may collect them.
std::unordered_map<int, net::PeerId> g_guestDrops;  // host: drops created while a guest's puppet was acting
std::unordered_set<int> g_hostOwnedDrops;   // guest: replicas of drops the host earned

// Host: objects created this step. Sent at the end of the step, once their creator has set them up
// (DropItem sets a drop's `item` right after instance_create_depth returns).
std::vector<int> g_pendingSpawns;

// Crafting: what a structure makes goes straight into the inventory of whoever crafted it (QUESTIONS.md), not
// onto the ground. The host gets all its own drops at once anyway (FlushSpawns); the guest crafts on its own
// copy of the structure, and its crafted drops are collected here at its next step (DropItem sets `item`
// after the drop is created).
int g_crafting = 0;               // inside HandleCrafting
std::vector<int> g_craftedDrops;  // guest
std::unordered_set<int> g_localDrops;  // guest: crafted here, the host never knew them - their end is no news

void DestroySilently(yyc::CInstance* self, int id);

void FlushSpawns(yyc::CInstance* self) {
    for (const int id : g_pendingSpawns) {
        yyc::CInstance* instance = yyc::FindInstance(id);
        if (instance == nullptr) continue;
        const InstanceInfo info = Describe(instance);
        int32_t item = kNoItem;
        int32_t owner = kOwnerAnyone;
        const auto guestDrop = g_guestDrops.find(id);
        if (IsDrop(self, info.object)) {
            yyc::RValue value{};
            yyc::GetVar(id, kSlotItem, &value);
            item = static_cast<int32_t>(yyc::ToReal(value));
            yyc::Free(&value);
            owner = guestDrop != g_guestDrops.end() ? kOwnerGuest : kOwnerHost;
        }
        if (owner == kOwnerHost) {
            CollectNow(instance, self);  // the host's loot goes straight into its inventory
            continue;
        }
        if (owner != kOwnerGuest) {
            Send(kSpawn, info, id, item, owner);
            continue;
        }
        // Only its guest hears of it: that guest collects its copy on arrival, ours goes - for the others it
        // never existed.
        const net::PeerId guest = guestDrop->second;
        Send(kSpawn, info, id, item, owner, guest);
        Log("sync: drop of item %d goes to guest %u", item, guest);
        g_guestDrops.erase(guestDrop);
        DestroySilently(self, id);
    }
    g_pendingSpawns.clear();
}

// The active instance of `object` standing at (x, y), or nullptr. Off-screen objects are deactivated
// (CullObject) and not found here.
yyc::CInstance* FindAt(yyc::CInstance* self, int object, double x, double y) {
    const int id = static_cast<int>(Builtin(self, "instance_nearest", {x, y, double(object)}));
    if (id == kNoOne) return nullptr;
    yyc::CInstance* instance = yyc::FindInstance(id);
    if (instance == nullptr) return nullptr;
    const double dx = yyc::GetBuiltinReal(instance, yyc::va::YYVar_x) - x;
    const double dy = yyc::GetBuiltinReal(instance, yyc::va::YYVar_y) - y;
    return std::sqrt(dx * dx + dy * dy) <= kMatchDistance ? instance : nullptr;
}

void DestroySilently(yyc::CInstance* self, int id) {
    ++g_applyingRemote;
    Builtin(self, "instance_destroy", {double(id), 0});  // execute_event = false: no drops, no XP
    --g_applyingRemote;
}

// ---- builtin hooks ----

// Guest: what the player builds or places (build menu, placeable items, seeds) must exist on the host too.
bool CreatedByGuestAction() {
    const StackFrame* top = *yyc::At<StackFrame**>(yyc::va::StackTraceTop);
    if (top == nullptr || top->name == nullptr) return false;
    // A seed is planted by the placement cursor UseSeed creates (objNewSeed), not by UseSeed itself.
    return strcmp(top->name, "gml_Object_objNewBuilding_Step_0") == 0 ||
           strcmp(top->name, "gml_Script_UsePlaceableItem") == 0 || strcmp(top->name, "gml_Script_UseSeed") == 0 ||
           strcmp(top->name, "gml_Object_objNewSeed_Step_0") == 0;
}

void __cdecl CreateHook(yyc::RValue* result, yyc::CInstance* self, yyc::CInstance* other, int argc,
                        yyc::RValue* args) {
    g_origCreate(result, self, other, argc, args);
    if (g_applyingRemote > 0 || !Connected()) return;
    const int id = static_cast<int>(yyc::ToReal(*result));
    yyc::CInstance* created = yyc::FindInstance(id);
    if (created == nullptr) return;
    const int object = static_cast<int>(yyc::GetBuiltinReal(created, yyc::va::YYVar_object_index));
    if (!IsSynced(self, object)) return;
    if (IsGuest() && g_crafting > 0 && IsDrop(self, object)) {
        g_craftedDrops.push_back(id);  // ours: collected at the next step, never reported
        g_localDrops.insert(id);
        return;
    }
    if (IsHost()) {
        g_pendingSpawns.push_back(id);
        if (puppet::IsActing()) g_guestDrops[id] = puppet::ActingPlayer();
    } else if (IsGuest() && g_suppressing > 0) {
        g_suppressed.push_back(id);
    } else if (IsGuest() && CreatedByGuestAction()) {
        const InstanceInfo info = Describe(created);
        Send(kGuestSpawn, info, info.id);
        Log("sync: built object %d at (%.0f, %.0f), sent to the host", info.object, info.x, info.y);
    }
}

// Every instance_destroy ends up here (YYC calls it directly, not through the builtin table).
// id == -1: self; otherwise an instance id (or an object index, which is not tracked).
void __cdecl DestroyHook(yyc::CInstance* self, yyc::CInstance* other, int id, int executeEvent) {
    yyc::CInstance* target = id == -1 ? self : id >= 100000 ? yyc::FindInstance(id) : nullptr;
    const bool report = target != nullptr && g_applyingRemote == 0 && !g_dryHit && Connected();
    InstanceInfo info{};
    bool synced = false;
    if (report) {
        info = Describe(target);
        synced = IsSynced(self, info.object);
    }
    // XP and rewards from destroying something a guest hit go to that guest (PlayerFind -> its puppet).
    const auto hitter = synced && IsHost() ? g_hitByGuest.find(info.id) : g_hitByGuest.end();
    const bool forGuest = hitter != g_hitByGuest.end();
    if (forGuest) {
        puppet::BeginActing(hitter->second);
        g_hitByGuest.erase(hitter);
    }
    g_origDestroy(self, other, id, executeEvent);
    if (forGuest) puppet::EndActing();
    if (!synced) return;

    if (IsHost()) {
        // Created and gone within one step: the guest never heard of it.
        const auto pending = std::find(g_pendingSpawns.begin(), g_pendingSpawns.end(), info.id);
        if (pending != g_pendingSpawns.end()) {
            g_pendingSpawns.erase(pending);
        } else {
            Send(kDestroy, info, info.id);
        }
    } else if (IsGuest()) {
        if (g_localDrops.erase(info.id) > 0) return;
        const auto host = g_localToHost.find(info.id);
        Send(kGuestDestroyed, info, host != g_localToHost.end() ? host->second : -1);
        if (host != g_localToHost.end()) {
            g_hostToLocal.erase(host->second);
            g_localToHost.erase(host);
        }
    }
}

// Dungeons are not part of coop (yet): rooms other than the world are not synced, so while connected nobody
// enters one (QUESTIONS.md). The entrance's transition just plays and the player stays in the world.
constexpr int kFirstDungeonRoom = 14;  // rmAncientTomb ... (decompiled/data/rooms.tsv)
constexpr int kLastDungeonRoom = 21;   // ... rmVolcanoSpaceTower; 22-24 are interiors (wizard tower, druid tree, museum)
yyc::BuiltinFn g_origRoomGoto = nullptr;

void __cdecl RoomGotoHook(yyc::RValue* result, yyc::CInstance* self, yyc::CInstance* other, int argc,
                          yyc::RValue* args) {
    const int room = argc >= 1 ? static_cast<int>(yyc::ToReal(args[0])) : -1;
    if (skilltab::InterceptRoomGoto(self, room)) return;  // the skill tree opens/closes in the world room
    if (Connected() && room >= kFirstDungeonRoom && room <= kLastDungeonRoom) {
        Log("sync: not entering room %d - no dungeons in coop", room);
        return;
    }
    g_origRoomGoto(result, self, other, argc, args);
}

void HookBuiltins() {
    g_builtinsHooked = true;
    if (const yyc::BuiltinFn roomGoto = yyc::BuiltinAddress("room_goto")) {
        const bool hooked = MH_CreateHook(reinterpret_cast<void*>(roomGoto), reinterpret_cast<void*>(&RoomGotoHook),
                                          reinterpret_cast<void**>(&g_origRoomGoto)) == MH_OK &&
                            MH_EnableHook(reinterpret_cast<void*>(roomGoto)) == MH_OK;
        if (!hooked) Log("sync: room_goto hook FAILED");
    }
    const yyc::BuiltinFn create = yyc::BuiltinAddress("instance_create_depth");
    if (create == nullptr) {
        Log("sync: instance_create_depth not found");
        return;
    }
    const bool ok = MH_CreateHook(reinterpret_cast<void*>(create), reinterpret_cast<void*>(&CreateHook),
                                  reinterpret_cast<void**>(&g_origCreate)) == MH_OK &&
                    MH_EnableHook(reinterpret_cast<void*>(create)) == MH_OK;
    Log("sync: builtin hooks %s", ok ? "enabled" : "FAILED");
}

// ---- script hooks ----

// The guest's world must not grow on its own: everything new comes from the host. Callers use the
// result (`if (res != noone) res.object_index ...`), so the skipped spawn must say "noone", not undefined.
yyc::RValue* SpawnedNothing(yyc::RValue* result) {
    yyc::Free(result);
    *result = yyc::Real(kNoOne);
    return result;
}

yyc::RValue* __cdecl ResourceSpawnHook(yyc::CInstance* self, yyc::CInstance* other, yyc::RValue* result, int argc,
                                       yyc::RValue** args) {
    if (IsGuest()) return SpawnedNothing(result);
    return g_origResourceSpawn(self, other, result, argc, args);
}

yyc::RValue* __cdecl ResourceSpawnSingleHook(yyc::CInstance* self, yyc::CInstance* other, yyc::RValue* result,
                                             int argc, yyc::RValue** args) {
    if (IsGuest()) return SpawnedNothing(result);
    return g_origResourceSpawnSingle(self, other, result, argc, args);
}

// Guest: herbs growing out of dirt are the host's to spawn (PLAYTEST.md #5): ours would exist here only. The
// dirt itself still goes away as usual.
yyc::RValue* __cdecl DirtProcessHook(yyc::CInstance* self, yyc::CInstance* other, yyc::RValue* result, int argc,
                                     yyc::RValue** args) {
    if (!IsGuest() || !Connected()) return g_origDirtProcess(self, other, result, argc, args);
    BeginSuppressSpawns();
    yyc::RValue* r = g_origDirtProcess(self, other, result, argc, args);
    EndSuppressSpawns(self);
    return r;
}

yyc::RValue* __cdecl HandleCraftingHook(yyc::CInstance* self, yyc::CInstance* other, yyc::RValue* result, int argc,
                                        yyc::RValue** args) {
    if (stations::IsMirror(self)) return result;  // the other player's craft, only shown here
    ++g_crafting;
    yyc::RValue* r = g_origHandleCrafting(self, other, result, argc, args);
    --g_crafting;
    return r;
}

// Guest: crafted drops into the inventory.
void CollectCrafted(yyc::CInstance* localPlayer) {
    if (g_craftedDrops.empty()) return;
    size_t collected = 0;
    for (const int id : g_craftedDrops) {
        if (yyc::CInstance* drop = yyc::FindInstance(id)) {
            CollectNow(drop, localPlayer);
            ++collected;
        }
    }
    g_craftedDrops.clear();
    if (collected > 0) Log("sync: %zu crafted items straight into the inventory", collected);
}

// Level and XP are shared and kept by the host (QUESTIONS.md). Host: XP earned while the puppet acts goes to
// the host's own player. Guest: XP is not added here but sent to the host; the shared progress comes back
// as kProgress.
yyc::RValue* __cdecl GainXpHook(yyc::CInstance* self, yyc::CInstance* other, yyc::RValue* result, int argc,
                                yyc::RValue** args) {
    if (g_dryHit) return result;  // the hit's XP comes from the host
    if (argc >= 1 && IsGuest() && Connected() && g_applyingRemote == 0) {
        const XpMsg msg{static_cast<float>(yyc::ToReal(*args[0]))};
        net::Send(kXp, &msg, sizeof(msg));
        return result;
    }
    if (!IsHost() || !puppet::IsActing()) return g_origGainXp(self, other, result, argc, args);
    const puppet::Acting acting = puppet::PauseActing();
    yyc::RValue* r = g_origGainXp(self, other, result, argc, args);
    puppet::ResumeActing(acting);
    return r;
}

// Steam achievements are the host's only (docs/coop/SPEC.md): the guest's game does not unlock any.
yyc::RValue* __cdecl UnlockAchievementHook(yyc::CInstance* self, yyc::CInstance* other, yyc::RValue* result,
                                           int argc, yyc::RValue** args) {
    if (IsGuest() && Connected()) return result;
    return g_origUnlockAchievement(self, other, result, argc, args);
}



// event_user(0) with the player as `other` is also how drops get collected, so only tool use counts as a hit.
bool CalledFromToolUse() {
    const StackFrame* top = *yyc::At<StackFrame**>(yyc::va::StackTraceTop);
    if (top == nullptr || top->name == nullptr) return false;
    return strcmp(top->name, "gml_Script_UsePickaxe") == 0 || strcmp(top->name, "gml_Script_UseSword") == 0;
}

float ReadVar(int id, int slot) {
    yyc::RValue value{};
    yyc::GetVar(id, slot, &value);
    const float result = static_cast<float>(yyc::ToReal(value));
    yyc::Free(&value);
    return result;
}

// Host: after a hit, the guest's copy shows the same health bar.
void SendHp(yyc::CInstance* target) {
    if (!IsHost() || !Connected() || target == nullptr) return;
    const InstanceInfo info = Describe(target);
    const HpMsg msg{info.id, info.object, static_cast<float>(info.x), static_cast<float>(info.y),
                    ReadVar(info.id, kSlotHp), ReadVar(info.id, kSlotHpAlpha), ReadVar(info.id, kSlotHpLast),
                    ReadVar(info.id, kSlotHpWhite)};
    net::Send(kObjectHp, &msg, sizeof(msg));
}

void ApplyHp(yyc::CInstance* self, const HpMsg& msg) {
    const int local = Resolve(self, msg.id, msg.object, msg.x, msg.y);
    if (local == 0) return;
    const int slots[4] = {kSlotHp, kSlotHpAlpha, kSlotHpLast, kSlotHpWhite};
    const float values[4] = {msg.hp, msg.alpha, msg.last, msg.white};
    for (int i = 0; i < 4; ++i) {
        yyc::RValue v = yyc::Real(values[i]);
        yyc::SetVar(local, slots[i], &v);
    }
}

// Guest: a tool hit goes to the host with the guest's own damage (the puppet's would be used there otherwise).
void SendHit(const InstanceInfo& info) {
    const auto host = g_localToHost.find(info.id);
    ObjectMsg msg{host != g_localToHost.end() ? host->second : -1, info.object, static_cast<float>(info.x),
                  static_cast<float>(info.y), 0, kNoItem, kOwnerAnyone, -1, 0};
    if (g_localPlayer != nullptr) {
        msg.depth = ReadVar(static_cast<int>(yyc::GetBuiltinReal(g_localPlayer, yyc::va::YYVar_id)), kSlotDmg);
    }
    if (net::Send(kHit, &msg, sizeof(msg))) ++g_sent[kHit];
}

bool IsEnemy(yyc::CInstance* self, int object) {
    constexpr int kParEnemy = 23;
    return object == kParEnemy || Builtin(self, "object_is_ancestor", {double(object), double(kParEnemy)}) != 0;
}

// Guest: the hit is also shown here - shake, flash, sound, particles - by running the object's own hit event
// with its health padded so that it cannot break, and put back afterwards. Nothing it does is reported or
// kept: no XP, no destroys, no drops (the host decides those, PLAYTEST.md #7).
void DryHit(yyc::CInstance* target, yyc::CInstance* player) {
    constexpr double kPadding = 1e6;
    const int id = static_cast<int>(yyc::GetBuiltinReal(target, yyc::va::YYVar_id));
    yyc::RValue hp{};
    const bool number = yyc::GetVar(id, kSlotHp, &hp) && yyc::IsNumber(hp);
    const double before = number ? yyc::ToReal(hp) : 0;
    yyc::Free(&hp);
    if (!number) return;
    yyc::RValue padded = yyc::Real(before + kPadding);
    yyc::SetVar(id, kSlotHp, &padded);
    g_dryHit = true;
    BeginSuppressSpawns();
    g_origEventUser(target, player, 0);
    EndSuppressSpawns(target);
    g_dryHit = false;
    if (yyc::FindInstance(id) == nullptr) return;
    yyc::RValue restored = yyc::Real(before);
    yyc::SetVar(id, kSlotHp, &restored);
}

// Guest: a tool hit is `with (target) event_user(0)` with the player as `other`; the host decides its outcome.
// Host: its own tool hits are shown on the guest's copy (health bar).
void __cdecl EventUserHook(yyc::CInstance* self, yyc::CInstance* other, int n) {
    if (IsHost() && n == 0 && other != nullptr && other == g_localPlayer && Connected() && CalledFromToolUse()) {
        const int id = static_cast<int>(yyc::GetBuiltinReal(self, yyc::va::YYVar_id));
        const bool synced = IsSynced(self, static_cast<int>(yyc::GetBuiltinReal(self, yyc::va::YYVar_object_index)));
        g_origEventUser(self, other, n);
        if (synced) SendHp(yyc::FindInstance(id));
        return;
    }
    if (n != 0 || !IsGuest() || other == nullptr || other != g_localPlayer || g_applyingRemote > 0 ||
        !Connected() || !CalledFromToolUse()) {
        g_origEventUser(self, other, n);
        return;
    }
    const InstanceInfo info = Describe(self);
    if (!IsSynced(self, info.object)) {
        g_origEventUser(self, other, n);
        return;
    }
    SendHit(info);
    Log("sync: hit object %d at (%.0f, %.0f) sent to the host", info.object, info.x, info.y);
    if (!IsEnemy(self, info.object)) DryHit(self, other);
}

// Host: a drop touching the guest's puppet must not land in the host's inventory; the guest collects
// its own copy and reports it (kGuestDestroyed).
void __cdecl CollectHook(yyc::CInstance* self, yyc::CInstance* other) {
    if (IsHost() && puppet::IsPuppet(other)) return;
    if (Connected()) {
        const int id = static_cast<int>(yyc::GetBuiltinReal(self, yyc::va::YYVar_id));
        if (IsHost() && g_guestDrops.count(id) > 0) return;      // the guest's loot
        if (IsGuest() && g_hostOwnedDrops.count(id) > 0) return;  // the host's loot
    }
    g_origCollect(self, other);
}

bool Hook(uint32_t va, void* detour, void* original) {
    const MH_STATUS status = MH_CreateHook(yyc::At<void*>(va), detour, static_cast<void**>(original));
    if (status != MH_OK) Log("sync: hook @ VA %08X failed: %s", va, MH_StatusToString(status));
    return status == MH_OK;
}

// ---- applying messages ----

ObjectMsg ReadObject(const net::Message& message) {
    ObjectMsg msg{};
    memcpy(&msg, message.payload.data(), sizeof(msg));
    return msg;
}

int CreateReplica(yyc::CInstance* self, int hostId, int object, double x, double y, double depth) {
    ++g_applyingRemote;
    const int id = static_cast<int>(Builtin(self, "instance_create_depth", {x, y, depth, double(object)}));
    --g_applyingRemote;
    if (yyc::FindInstance(id) == nullptr) return 0;
    g_hostToLocal[hostId] = id;
    g_localToHost[id] = hostId;
    return id;
}

void ApplySpawn(yyc::CInstance* self, const ObjectMsg& msg) {
    // An enemy update may have created the replica already (it can overtake the spawn message).
    const auto known = g_hostToLocal.find(msg.id);
    if (known != g_hostToLocal.end() && yyc::FindInstance(known->second) != nullptr) return;
    const int id = CreateReplica(self, msg.id, msg.object, msg.x, msg.y, msg.depth);
    if (id != 0 && msg.item != kNoItem) {
        yyc::RValue item = yyc::Real(msg.item);
        yyc::SetVar(id, kSlotItem, &item);
    }
    if (id != 0 && msg.sprite >= 0) {
        if (yyc::CInstance* replica = yyc::FindInstance(id)) {
            yyc::SetBuiltinReal(replica, yyc::va::YYVar_sprite_index, msg.sprite);
            yyc::SetBuiltinReal(replica, yyc::va::YYVar_image_index, msg.image);
        }
    }
    if (id != 0 && msg.owner == kOwnerHost) g_hostOwnedDrops.insert(id);
    if (id != 0 && msg.owner == kOwnerGuest) {
        if (yyc::CInstance* drop = yyc::FindInstance(id)) {
            CollectNow(drop, self);  // straight into our inventory
            Log("sync: our drop of item %d collected", msg.item);
        }
    }
}

// Guest: destroys of objects the guest has deactivated (off-screen, CullObject). instance_nearest and
// instance_destroy skip deactivated instances, and instance_activate_* only takes effect on the next step,
// so such a destroy wakes the object up and is retried over the next steps. The game culls the rest back.
struct PendingDestroy {
    ObjectMsg msg;
    int local;  // the known replica, or 0: look the object up by position
    int attempts;
};
std::vector<PendingDestroy> g_pendingDestroys;
constexpr int kDestroyAttempts = 5;

// Destroys the object if it is active now; false if it has to be woken up first.
bool TryDestroy(yyc::CInstance* self, const ObjectMsg& msg, int local) {
    if (local == 0) {
        yyc::CInstance* found = FindAt(self, msg.object, msg.x, msg.y);
        if (found == nullptr) return false;
        local = static_cast<int>(yyc::GetBuiltinReal(found, yyc::va::YYVar_id));
    } else if (yyc::FindInstance(local) == nullptr) {
        return true;  // already gone
    } else if (Builtin(self, "instance_exists", {double(local)}) == 0) {  // false for deactivated instances
        return false;
    }
    DestroySilently(self, local);
    return true;
}

void ApplyDestroy(yyc::CInstance* self, const ObjectMsg& msg) {
    g_destroyedOnHost.insert(msg.id);
    int local = 0;
    const auto known = g_hostToLocal.find(msg.id);
    if (known != g_hostToLocal.end()) {
        local = known->second;
        g_localToHost.erase(local);
        g_hostToLocal.erase(known);
    }
    if (TryDestroy(self, msg, local)) return;
    if (local == 0 && IsDrop(self, msg.object)) return;  // the host's own loot, collected at once: never sent
    if (local != 0) {
        Builtin(self, "instance_activate_object", {double(local)});
    } else {
        Builtin(self, "instance_activate_region",
                {msg.x - kMatchDistance, msg.y - kMatchDistance, 2 * kMatchDistance, 2 * kMatchDistance, 1});
    }
    g_pendingDestroys.push_back({msg, local, 0});
}

// `countAttempt`: once per step (from Tick); CullObject runs several times a step and only gets a free try.
void RetryDestroys(yyc::CInstance* self, bool countAttempt) {
    if (g_pendingDestroys.empty()) return;
    std::vector<PendingDestroy> waiting;
    for (PendingDestroy pending : g_pendingDestroys) {
        if (TryDestroy(self, pending.msg, pending.local)) continue;
        if (!countAttempt || ++pending.attempts < kDestroyAttempts) {
            waiting.push_back(pending);
        } else {
            Log("sync: destroy of object %d at (%.0f, %.0f): not found", pending.msg.object, pending.msg.x,
                pending.msg.y);
        }
    }
    g_pendingDestroys = std::move(waiting);
}


// Host: the instance a guest message refers to (by the host id it carries, else by object and position).
yyc::CInstance* ResolveOnHost(yyc::CInstance* self, const ObjectMsg& msg) {
    if (msg.id > 0) {
        if (yyc::CInstance* instance = yyc::FindInstance(msg.id)) return instance;
    }
    return FindAt(self, msg.object, msg.x, msg.y);
}

// `woken`: the second try, after waking up the object's kind (the host may have culled what the guest hit).
void ApplyHit(yyc::CInstance* self, net::PeerId from, const ObjectMsg& msg, bool woken = false) {
    yyc::CInstance* target = ResolveOnHost(self, msg);
    yyc::CInstance* guest = puppet::Instance(from);
    if (target == nullptr && guest != nullptr && !woken) {
        WakeThen(self, msg.object, [self, from, msg] { ApplyHit(self, from, msg, true); });
        return;
    }
    Log("sync: guest hit object %d at (%.0f, %.0f): %s (dmg %.1f, guest %u)", msg.object, msg.x, msg.y,
        target == nullptr ? "not found" : guest == nullptr ? "no puppet" : "applied", msg.depth, from);
    if (target == nullptr || guest == nullptr) return;
    const int targetId = static_cast<int>(yyc::GetBuiltinReal(target, yyc::va::YYVar_id));
    g_hitByGuest[targetId] = from;
    // Like UsePickaxe: marks the hit as the player's, which enemies need to reward XP when they die.
    yyc::RValue killedByPlayer = yyc::Real(yyc::GetBuiltinReal(target, kYYVarRoomSpeed));
    yyc::SetVar(targetId, kSlotKilledByPlayer, &killedByPlayer);
    // The guest's tool decides the damage: the puppet carries the guest's `dmg` for this hit (PLAYTEST.md #6).
    const int guestId = static_cast<int>(yyc::GetBuiltinReal(guest, yyc::va::YYVar_id));
    const float puppetDmg = ReadVar(guestId, kSlotDmg);
    if (msg.depth > 0) {
        yyc::RValue dmg = yyc::Real(msg.depth);
        yyc::SetVar(guestId, kSlotDmg, &dmg);
    }
    puppet::BeginActing(from);
    yyc::PerformEvent(target, guest, yyc::kEvOther, yyc::kEvUser0);
    puppet::EndActing();
    if (msg.depth > 0 && yyc::FindInstance(guestId) != nullptr) {
        yyc::RValue dmg = yyc::Real(puppetDmg);
        yyc::SetVar(guestId, kSlotDmg, &dmg);
    }
    SendHp(yyc::FindInstance(targetId));  // gone if the hit destroyed it
}

// Host: build what the guest built (as the guest, for anything that asks who "the player" is) and tell
// the guest which instance it became.
void ApplyGuestSpawn(yyc::CInstance* self, net::PeerId from, const ObjectMsg& msg) {
    puppet::BeginActing(from);
    ++g_applyingRemote;
    const int id = static_cast<int>(
        Builtin(self, "instance_create_depth", {msg.x, msg.y, msg.depth, double(msg.object)}));
    --g_applyingRemote;
    puppet::EndActing();
    if (yyc::FindInstance(id) == nullptr) return;
    const ObjectMsg ack{id, msg.object, msg.x, msg.y, msg.depth, msg.id};
    net::SendTo(from, kGuestSpawnAck, &ack, sizeof(ack));
    Log("sync: guest built object %d at (%.0f, %.0f) -> %d (guest %u)", msg.object, msg.x, msg.y, id, from);
}

void ApplyGuestDestroyed(yyc::CInstance* self, const ObjectMsg& msg) {
    if (yyc::CInstance* target = ResolveOnHost(self, msg)) {
        DestroySilently(self, static_cast<int>(yyc::GetBuiltinReal(target, yyc::va::YYVar_id)));
    }
}

// Host: XP the guest earned, added to the shared progress.
void ApplyXp(yyc::CInstance* localPlayer, const XpMsg& msg) {
    Log("sync: +%.1f XP from the guest", msg.amount);
    yyc::RValue amount = yyc::Real(msg.amount);
    yyc::RValue* args[1] = {&amount};
    yyc::RValue result{};
    g_origGainXp(localPlayer, localPlayer, &result, 1, args);
    yyc::Free(&result);
}

}  // namespace

bool InstallHooks(bool (*isHost)(), bool (*isGuest)()) {
    g_isHost = isHost;
    g_isGuest = isGuest;
    bool ok = Hook(kScriptResourceSpawn, reinterpret_cast<void*>(&ResourceSpawnHook), &g_origResourceSpawn);
    ok = Hook(kScriptResourceSpawnSingle, reinterpret_cast<void*>(&ResourceSpawnSingleHook),
              &g_origResourceSpawnSingle) && ok;
    ok = Hook(kScriptDirtProcess, reinterpret_cast<void*>(&DirtProcessHook), &g_origDirtProcess) && ok;
    ok = Hook(kScriptGainXp, reinterpret_cast<void*>(&GainXpHook), &g_origGainXp) && ok;
    ok = Hook(kScriptHandleCrafting, reinterpret_cast<void*>(&HandleCraftingHook), &g_origHandleCrafting) && ok;
    ok = Hook(kScriptUnlockAchievement, reinterpret_cast<void*>(&UnlockAchievementHook), &g_origUnlockAchievement) &&
         ok;
    ok = Hook(kEventUser, reinterpret_cast<void*>(&EventUserHook), &g_origEventUser) && ok;
    ok = Hook(kResourceCollect, reinterpret_cast<void*>(&CollectHook), &g_origCollect) && ok;
    ok = Hook(kInstanceDestroy, reinterpret_cast<void*>(&DestroyHook), &g_origDestroy) && ok;
    return ok;
}

// Testing aid: COOP_DEBUG_AUTOHIT=1 makes the guest hit the nearest enemy every 2 s, exactly as a tool
// hit would be reported.
// COOP_DEBUG_AUTOHIT=<object index> hits the nearest instance of that object instead (e.g. 84 = rocks).
void DebugAutoHit(yyc::CInstance* localPlayer) {
    static int target = [] {
        char value[16] = {};
        if (GetEnvironmentVariableA("COOP_DEBUG_AUTOHIT", value, sizeof(value)) == 0) return 0;
        const int object = atoi(value);
        return object > 1 ? object : 23;  // "1": enemies (parEnemy)
    }();
    constexpr unsigned kEverySteps = 120;
    if (target == 0 || !IsGuest() || g_steps % kEverySteps != 0) return;
    const double x = yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_x);
    const double y = yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_y);
    const int id = static_cast<int>(Builtin(localPlayer, "instance_nearest", {x, y, double(target)}));
    yyc::CInstance* enemy = id == kNoOne ? nullptr : yyc::FindInstance(id);
    if (enemy == nullptr) return;
    SendHit(Describe(enemy));
    if (!IsEnemy(localPlayer, target)) DryHit(enemy, localPlayer);  // as a real tool hit shows it
    Log("sync: debug auto-hit on object %d", static_cast<int>(yyc::GetBuiltinReal(enemy, yyc::va::YYVar_object_index)));
}

// Testing aid: COOP_DEBUG_BUILD=<object index> makes the guest build that object next to itself ~5 s
// after entering the world, reported like a real build.
void DebugBuild(yyc::CInstance* localPlayer) {
    constexpr unsigned kAtStep = 300;
    char value[16] = {};
    if (!IsGuest() || g_steps != kAtStep ||
        GetEnvironmentVariableA("COOP_DEBUG_BUILD", value, sizeof(value)) == 0) {
        return;
    }
    const double x = yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_x) + 32;
    const double y = yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_y);
    const int id = static_cast<int>(Builtin(localPlayer, "instance_create_depth", {x, y, -y, double(atoi(value))}));
    if (yyc::CInstance* built = yyc::FindInstance(id)) {
        const InstanceInfo info = Describe(built);
        Send(kGuestSpawn, info, info.id);
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
    const int count = static_cast<int>(Builtin(self, "instance_number", {double(object)}));
    for (int i = 0; i < count; ++i) {
        const int id = static_cast<int>(Builtin(self, "instance_find", {double(object), double(i)}));
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
    const int count = static_cast<int>(Builtin(self, "instance_number", {double(object)}));
    Log("sync: cull test: %d objects at the %s", count, when);
    for (int i = 0; i < count; ++i) {
        const int id = static_cast<int>(Builtin(self, "instance_find", {double(object), double(i)}));
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
        Builtin(localPlayer, "instance_activate_object", {double(object)});
        g_cullTestListing = steps == kStartAt ? "start" : "end";
    } else if (steps == kDestroyAt && IsHost()) {
        Builtin(localPlayer, "instance_activate_object", {double(object)});
        g_cullTestDestroy = true;
    } else if (steps == kCullAt && IsGuest()) {
        const std::vector<int> distant = FarInstances(localPlayer, object, {localPlayer});
        for (const int id : distant) Builtin(localPlayer, "instance_deactivate_object", {double(id)});
        Log("sync: cull test: deactivated %zu distant objects", distant.size());
    }
}

// The shared progress lives in the host's player: level, xp, xpCap (XP for the next level), xpToGain (XP still
// being counted up), and (unused) a slot that is not the player's: skill points live on WorldControl and each
// player has its own (skills.h). The host sends it twice a second; the guest's player shows it.
constexpr int kProgressSlots[] = {0x603, 0x90E, 0x910, 0x912, 0x8F};  // decompiled/code/var_slots.tsv
constexpr unsigned kProgressEverySteps = 30;

void SendProgress(yyc::CInstance* localPlayer) {
    if (g_steps % kProgressEverySteps != 0) return;
    const int id = static_cast<int>(yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_id));
    float values[5] = {};
    for (int i = 0; i < 5; ++i) values[i] = ReadVar(id, kProgressSlots[i]);
    const ProgressMsg msg{values[0], values[1], values[2], values[3], values[4]};
    net::Send(kProgress, &msg, sizeof(msg));
}

void ApplyProgress(yyc::CInstance* localPlayer, const ProgressMsg& msg) {
    static float shownLevel = -1;
    const int id = static_cast<int>(yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_id));
    // The fifth value (skill points) is the host's own: each player has its own points (skills.h).
    const float values[4] = {msg.level, msg.xp, msg.xpCap, msg.xpToGain};
    // A loaded character may keep these as integers: any number is overwritten; anything else (not set up yet)
    // is left alone.
    for (int i = 0; i < 4; ++i) {
        yyc::RValue current{};
        const bool number = yyc::GetVar(id, kProgressSlots[i], &current) && yyc::IsNumber(current);
        const uint32_t kind = current.kind & yyc::kKindMask;
        yyc::Free(&current);
        if (!number) {
            static bool told[4] = {};
            if (!told[i]) Log("sync: progress slot 0x%X is kind %u here - not set", kProgressSlots[i], kind);
            told[i] = true;
            continue;
        }
        yyc::RValue value = yyc::Real(values[i]);
        yyc::SetVar(id, kProgressSlots[i], &value);
    }
    if (msg.level != shownLevel) {
        Log("sync: shared progress: level %.0f, xp %.0f/%.0f (level here now %.0f)", msg.level, msg.xp, msg.xpCap,
            ReadVar(id, kProgressSlots[0]));
        shownLevel = msg.level;
    }
}

// Testing aid: COOP_DEBUG_XP=<amount> gives the host that much XP every ~10 s (level-ups on both sides).
void DebugXp(yyc::CInstance* localPlayer) {
    static const double amount = [] {
        char text[16] = {};
        return GetEnvironmentVariableA("COOP_DEBUG_XP", text, sizeof(text)) > 0 ? atof(text) : 0.0;
    }();
    if (amount <= 0 || !IsHost() || !Connected() || g_steps == 0 || g_steps % 600 != 0) return;
    Log("sync: debug gives the host %.0f XP", amount);
    yyc::RValue value = yyc::Real(amount);
    yyc::RValue* args[1] = {&value};
    yyc::RValue result{};
    g_origGainXp(localPlayer, localPlayer, &result, 1, args);
    yyc::Free(&result);
}

// Testing aid: COOP_DEBUG_DUNGEON=1 tries to enter the first dungeon room ~5 s in (it must be refused).
void DebugDungeon(yyc::CInstance* localPlayer) {
    static const bool enabled = GetEnvironmentVariableA("COOP_DEBUG_DUNGEON", nullptr, 0) > 0;
    static unsigned steps = 0;
    if (!enabled || ++steps != 300) return;
    Log("sync: debug tries to enter room %d", kFirstDungeonRoom);
    Builtin(localPlayer, "room_goto", {double(kFirstDungeonRoom)});
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
    ++g_crafting;
    const int id = static_cast<int>(Builtin(localPlayer, "instance_create_depth", {x, y, -y, double(kParResource)}));
    --g_crafting;
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
        const int count = static_cast<int>(Builtin(self, "instance_number", {double(parent)}));
        for (int i = 0; i < count; ++i) {
            const int id = static_cast<int>(Builtin(self, "instance_find", {double(parent), double(i)}));
            yyc::CInstance* instance = yyc::FindInstance(id);
            if (instance == nullptr) continue;
            const InstanceInfo info = Describe(instance);
            if (std::hypot(info.x - census.x, info.y - census.y) > kCensusRadius) continue;
            fprintf(file,"%d %.0f %.0f %d\n", info.object, info.x, info.y, HostIdOf(info.id));
            ++written;
        }
    }
    fclose(file);
    Log("sync: census #%d: %zu objects", number, written);
}

void DebugCensus(yyc::CInstance* localPlayer) {
    constexpr unsigned kEverySteps = 1800;
    static int number = 0;
    if (!IsHost() || !CensusEnabled() || !Connected() || g_steps % kEverySteps != 0) return;
    ++number;
    const CensusMsg census{number, static_cast<float>(yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_x)),
                           static_cast<float>(yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_y))};
    WriteCensus(localPlayer, census);
    net::Send(kCensus, &census, sizeof(census));
}

void Tick(yyc::CInstance* localPlayer) {
    g_localPlayer = localPlayer;
    DebugAutoHit(localPlayer);
    DebugBuild(localPlayer);
    DebugCensus(localPlayer);
    DebugCullTest(localPlayer);
    if (!g_builtinsHooked) HookBuiltins();
    if (IsHost()) FlushSpawns(localPlayer);
    if (IsHost()) SendProgress(localPlayer);
    if (IsGuest()) RetryDestroys(localPlayer, true);
    if (IsGuest()) CollectCrafted(localPlayer);
    DebugCraftDrop(localPlayer);
    DebugDungeon(localPlayer);
    DebugXp(localPlayer);
    if (++g_steps % kStatsEverySteps == 0) {
        Log("sync: sent spawn %u destroy %u hit %u gone %u | applied spawn %u destroy %u hit %u gone %u",
            g_sent[kSpawn], g_sent[kDestroy], g_sent[kHit], g_sent[kGuestDestroyed], g_applied[kSpawn],
            g_applied[kDestroy], g_applied[kHit], g_applied[kGuestDestroyed]);
    }
}

std::vector<std::function<void()>> g_afterWake;

void WakeThen(yyc::CInstance* self, int object, std::function<void()> action) {
    Builtin(self, "instance_activate_object", {double(object)});
    g_afterWake.push_back(std::move(action));
}

void RunBeforeCull(std::function<void()> action) { g_afterWake.push_back(std::move(action)); }

void BeforeCull(yyc::CInstance* self) {
    if (IsGuest()) RetryDestroys(self, false);
    if (!g_afterWake.empty()) {
        std::vector<std::function<void()>> actions;
        actions.swap(g_afterWake);
        for (const auto& action : actions) action();
    }
    if (g_cullTestListing != nullptr) {
        LogCullTestObjects(self, g_cullTestListing);
        g_cullTestListing = nullptr;
    }
    if (g_cullTestDestroy && g_localPlayer != nullptr) {
        g_cullTestDestroy = false;
        const std::vector<int> distant = FarInstances(self, CullTestObject(), WithPuppets(g_localPlayer));
        for (const int id : distant) Builtin(self, "instance_destroy", {double(id)});
        Log("sync: cull test: destroyed %zu distant objects", distant.size());
    }
}

int ReplicaOf(int hostId) {
    const auto found = g_hostToLocal.find(hostId);
    return found != g_hostToLocal.end() ? found->second : 0;
}

int SpawnReplica(yyc::CInstance* self, int hostId, int object, double x, double y, double depth) {
    return CreateReplica(self, hostId, object, x, y, depth);
}

void RemoveLocal(yyc::CInstance* self, int localId) {
    const auto host = g_localToHost.find(localId);
    if (host != g_localToHost.end()) {
        g_hostToLocal.erase(host->second);
        g_localToHost.erase(host);
    }
    if (yyc::FindInstance(localId) != nullptr) DestroySilently(self, localId);
}

void AdoptReplica(int hostId, int localId) {
    g_hostToLocal[hostId] = localId;
    g_localToHost[localId] = hostId;
}

bool IsReplica(int localId) { return g_localToHost.count(localId) > 0; }

bool WasDestroyed(int hostId) { return g_destroyedOnHost.count(hostId) > 0; }

void BeginSuppressSpawns() { ++g_suppressing; }

void EndSuppressSpawns(yyc::CInstance* self) {
    if (g_suppressing == 0 || --g_suppressing > 0) return;
    for (const int id : g_suppressed) {
        if (yyc::FindInstance(id) != nullptr) DestroySilently(self, id);
    }
    if (!g_suppressed.empty()) Log("sync: removed %zu locally generated objects", g_suppressed.size());
    g_suppressed.clear();
}

int HostIdOf(int localId) {
    if (IsHost()) return localId;
    const auto found = g_localToHost.find(localId);
    return found != g_localToHost.end() ? found->second : -1;
}

int Resolve(yyc::CInstance* self, int hostId, int object, double x, double y) {
    if (hostId > 0) {
        const int local = IsHost() ? hostId : ReplicaOf(hostId);
        if (local != 0 && yyc::FindInstance(local) != nullptr) return local;
    }
    yyc::CInstance* found = FindAt(self, object, x, y);
    return found != nullptr ? static_cast<int>(yyc::GetBuiltinReal(found, yyc::va::YYVar_id)) : 0;
}

bool Handle(const net::Message& message, yyc::CInstance* localPlayer) {
    const size_t size = message.payload.size();
    if (message.type >= kSpawn && message.type <= kGuestDestroyed) ++g_applied[message.type & 15];
    switch (message.type) {
        case kSpawn:
            if (size == sizeof(ObjectMsg) && IsGuest()) ApplySpawn(localPlayer, ReadObject(message));
            return true;
        case kDestroy:
            if (size == sizeof(ObjectMsg) && IsGuest()) ApplyDestroy(localPlayer, ReadObject(message));
            return true;
        case kHit:
            if (size == sizeof(ObjectMsg) && IsHost()) ApplyHit(localPlayer, message.from, ReadObject(message));
            return true;
        case kGuestDestroyed:
            if (size == sizeof(ObjectMsg) && IsHost()) ApplyGuestDestroyed(localPlayer, ReadObject(message));
            return true;
        case kObjectHp:
            if (size == sizeof(HpMsg) && IsGuest()) {
                HpMsg msg{};
                memcpy(&msg, message.payload.data(), sizeof(msg));
                ApplyHp(localPlayer, msg);
            }
            return true;
        case kGuestSpawn:
            if (size == sizeof(ObjectMsg) && IsHost()) ApplyGuestSpawn(localPlayer, message.from, ReadObject(message));
            return true;
        case kGuestSpawnAck:
            if (size == sizeof(ObjectMsg) && IsGuest()) {
                const ObjectMsg ack = ReadObject(message);
                if (yyc::FindInstance(ack.item) != nullptr) AdoptReplica(ack.id, ack.item);
            }
            return true;
        case kCensus:
            if (size == sizeof(CensusMsg) && IsGuest()) {
                CensusMsg census{};
                memcpy(&census, message.payload.data(), sizeof(census));
                WriteCensus(localPlayer, census);
            }
            return true;
        case kProgress:
            if (size == sizeof(ProgressMsg) && IsGuest()) {
                ProgressMsg msg{};
                memcpy(&msg, message.payload.data(), sizeof(msg));
                ApplyProgress(localPlayer, msg);
            }
            return true;
        case kXp:
            if (size == sizeof(XpMsg) && IsHost()) {
                XpMsg msg{};
                memcpy(&msg, message.payload.data(), sizeof(msg));
                ApplyXp(localPlayer, msg);
            }
            return true;
        default:
            return false;
    }
}

void Reset() {
    g_hostToLocal.clear();
    g_localToHost.clear();
    g_hitByGuest.clear();
    g_destroyedOnHost.clear();
    g_suppressing = 0;
    g_suppressed.clear();
    g_guestDrops.clear();
    g_hostOwnedDrops.clear();
    g_pendingSpawns.clear();
    g_pendingDestroys.clear();
    g_craftedDrops.clear();
    g_localDrops.clear();
    g_afterWake.clear();
}

}  // namespace coop::sync
