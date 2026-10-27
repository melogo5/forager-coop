#include "sync.h"
#include "sync_internal.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "MinHook.h"
#include "chests.h"
#include "familiars.h"
#include "log.h"
#include "mobs.h"
#include "mutant.h"
#include "puppet.h"
#include "interiors.h"
#include "skilltab.h"
#include "stations.h"
#include "world.h"

namespace coop::sync {

void WakeInstance(yyc::CInstance* self, int id);

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
    kGuestSummon = 25,     // guest -> host: a creature the guest's sigil or scroll made (ObjectMsg)
};
constexpr int kSlotHp = yyc::slot::hp;
constexpr int kSlotHpAlpha = 0x574;
constexpr int kSlotHpLast = 0x575;
constexpr int kSlotHpWhite = 0x577;
constexpr int kSlotDmg = 0x197;  // objPlayer.dmg: what a hit takes off an object (parObject Other_10)
constexpr int kSlotStack = 0x80E;    // a drop's `stack`: how many of its item it gives
constexpr float kMaxStack = 10000;  // sanity bound for a stack from the network

#pragma pack(push, 1)
struct ObjectMsg {
    int32_t id;  // the host's instance id; -1 when the sender does not know it
    int32_t object;
    float x, y;
    float depth;    // kSpawn: the depth; kHit: the guest's `dmg` (its tool's damage)
    int32_t item;   // kSpawn of a drop: its `item`; kGuestSpawn of a scroll spawner: its `type`; else kNoItem
    int32_t owner;  // kSpawn of a drop: who may collect it (kOwnerAnyone/Host/Guest - the receiving guest)
    float sprite;   // kSpawn: the look it rolled after creation (ore vs plain rock...); -1 = keep
    float image;
    // kSpawn of a drop: its `stack`, 0 = keep. DropItem adds a new drop to one lying near (stack + 1) - all the
    // more with lighthouses and amulets; the guest's copy stayed at 1 (PLAYTEST8.md #14, #15).
    float stack;
};
struct HpMsg {
    int32_t id;  // the host's instance id
    int32_t object;
    float x, y;
    float hp, alpha, last, white;  // what the health bar is drawn from
    uint8_t hitter;  // the player whose tool hit it (net::PeerId), kNoHitter for a repair
};
constexpr uint8_t kNoHitter = 0xFF;
constexpr int32_t kOwnerAnyone = 0;
constexpr int32_t kOwnerHost = 1;
constexpr int32_t kOwnerGuest = 2;
struct XpMsg {
    float amount;
};

struct ProgressMsg {
    float level, xp, xpCap, xpToGain;
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
constexpr int kSlotKilledByPlayer = 0x5DF;
constexpr uint32_t kYYVarRoomSpeed = 0x01AD8750;
constexpr int32_t kNoItem = -1;
constexpr uint32_t kResourceCollect = 0x00FF5580;  // parResource Other_10: collected by `other`
constexpr int kMaxObjects = 512;
constexpr double kMatchDistance = 4;  // pixels; objects loaded from the same save sit on the same spot

constexpr uint32_t kScriptResourceSpawn = 0x00749E80;
constexpr uint32_t kScriptResourceSpawnSingle = 0x00750710;
constexpr uint32_t kScriptDirtProcess = 0x004BEB20;  // dirt left by broken objects grows herbs (wheat, cotton...)
constexpr uint32_t kScriptGainXp = 0x00730600;
constexpr uint32_t kScriptUnlockAchievement = 0x004A6DA0;  // UnlockAchievement(name): Steam achievement
constexpr uint32_t kScriptHandleCrafting = 0x004AB790;  // a structure works on its craft; DropItem when done
constexpr uint32_t kScriptUseRepairKit = 0x006D6120;    // every structure near the player back to hpMax
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
// Guest: host ids reported destroyed (ids are never reused) - the latest kRememberDestroyed: what stops a late update
// bringing one back is recent, and a whole session's worth only grew.
std::unordered_set<int> g_destroyedOnHost;
std::deque<int> g_destroyedOrder;
constexpr size_t kRememberDestroyed = 20000;

yyc::ScriptFn g_origResourceSpawn = nullptr;
yyc::ScriptFn g_origResourceSpawnSingle = nullptr;
yyc::ScriptFn g_origDirtProcess = nullptr;
yyc::ScriptFn g_origGainXp = nullptr;
yyc::ScriptFn g_origHandleCrafting = nullptr;
yyc::ScriptFn g_origRepairKit = nullptr;
yyc::ScriptFn g_origUnlockAchievement = nullptr;
EventUserFn g_origEventUser = nullptr;
yyc::BuiltinFn g_origCreate = nullptr;
using DestroyFn = void(__cdecl*)(yyc::CInstance* self, yyc::CInstance* other, int id, int executeEvent);
constexpr uint32_t kInstanceDestroy = 0x016E6BA0;
DestroyFn g_origDestroy = nullptr;

// The YYC call stack: one frame per running GML function.
using yyc::StackFrame;

bool IsHost() { return g_isHost != nullptr && g_isHost(); }
bool IsGuest() { return g_isGuest != nullptr && g_isGuest(); }
bool Connected() { return net::IsConnected(); }

using yyc::CallReal;

bool IsSynced(yyc::CInstance* self, int object) {
    if (object < 0 || object >= kMaxObjects) return false;
    int8_t& cached = g_syncedCache[object];
    if (cached == 0) {
        cached = -1;
        for (const int parent : kSyncedParents) {
            if (object == parent || CallReal(self, "object_is_ancestor", {double(object), double(parent)}) != 0) {
                cached = 1;
                break;
            }
        }
    }
    return cached == 1;
}

InstanceInfo Describe(yyc::CInstance* instance) {
    return {static_cast<int>(yyc::GetBuiltinReal(instance, yyc::va::YYVar_id)),
            static_cast<int>(yyc::GetBuiltinReal(instance, yyc::va::YYVar_object_index)),
            yyc::GetBuiltinReal(instance, yyc::va::YYVar_x), yyc::GetBuiltinReal(instance, yyc::va::YYVar_y),
            yyc::GetBuiltinReal(instance, yyc::va::YYVar_depth)};
}

unsigned g_sent[16] = {};
std::unordered_map<int, unsigned> g_spawnKinds;  // host: objects sent as spawns since the last stats line, by kind

// Host: what the last stats period sent as new objects, most first (PLAYTEST3: bursts of 120+ with no cause in
// the log).
void LogSpawnKinds() {
    if (g_spawnKinds.empty()) return;
    std::vector<std::pair<int, unsigned>> kinds(g_spawnKinds.begin(), g_spawnKinds.end());
    std::sort(kinds.begin(), kinds.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    std::string text;
    for (size_t i = 0; i < kinds.size() && i < 6; ++i) {
        text += " " + std::to_string(kinds[i].first) + "x" + std::to_string(kinds[i].second);
    }
    Log("sync: spawns sent by object:%s", text.c_str());
    g_spawnKinds.clear();
}

unsigned g_applied[16] = {};
unsigned g_steps = 0;
constexpr unsigned kStatsEverySteps = 600;

float ReadVar(int id, int slot);

ObjectMsg MakeObjectMsg(MessageType type, const InstanceInfo& info, int id, int32_t item, int32_t owner) {
    ObjectMsg msg{id, info.object, static_cast<float>(info.x), static_cast<float>(info.y),
                  static_cast<float>(info.depth), item, owner, -1, 0};
    if (type == kSpawn) {
        if (yyc::CInstance* instance = yyc::FindInstance(id)) {
            msg.sprite = static_cast<float>(yyc::GetBuiltinReal(instance, yyc::va::YYVar_sprite_index));
            msg.image = static_cast<float>(yyc::GetBuiltinReal(instance, yyc::va::YYVar_image_index));
        }
        if (item != kNoItem) msg.stack = ReadVar(id, kSlotStack);
    }
    return msg;
}

// To everyone, or only to guest `to` when it is set.
void Send(MessageType type, const InstanceInfo& info, int id, int32_t item = kNoItem,
          int32_t owner = kOwnerAnyone, int to = -1) {
    const ObjectMsg msg = MakeObjectMsg(type, info, id, item, owner);
    const bool sent = to < 0 ? net::Send(type, &msg, sizeof(msg))
                             : net::SendTo(static_cast<net::PeerId>(to), type, &msg, sizeof(msg));
    if (sent) ++g_sent[type & 15];
}

// Host: what guest `from` did to the world, for the other guests as if the host had done it (with 3-4 players
// a guest's build or removal reached nobody else: the host carries it out quietly, without reporting it).
void SendToOtherGuests(net::PeerId from, MessageType type, const InstanceInfo& info, int id) {
    const ObjectMsg msg = MakeObjectMsg(type, info, id, kNoItem, kOwnerAnyone);
    net::SendExcept(from, type, &msg, sizeof(msg));
    ++g_sent[type & 15];
}

// `instance` is an `object` or one of its children.
bool IsKind(yyc::CInstance* self, yyc::CInstance* instance, int object) {
    const int own = static_cast<int>(yyc::GetBuiltinReal(instance, yyc::va::YYVar_object_index));
    return own == object || (object >= 0 && CallReal(self, "object_is_ancestor", {double(own), double(object)}) != 0);
}

bool IsDrop(yyc::CInstance* self, int object) {
    return object == kParResource ||
           (object >= 0 && CallReal(self, "object_is_ancestor", {double(object), double(kParResource)}) != 0);
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
float ReadVar(int id, int slot);

void FlushSpawns(yyc::CInstance* self) {
    for (const int id : g_pendingSpawns) {
        // Whatever it turns out to be, its entry goes now: one kept for anything but a delivered drop (a guest's
        // build, a drop gone within the step) stayed for the whole session.
        const auto guestDrop = g_guestDrops.find(id);
        const bool byGuest = guestDrop != g_guestDrops.end();
        const net::PeerId guest = byGuest ? guestDrop->second : net::kHostPeer;
        if (byGuest) g_guestDrops.erase(guestDrop);
        yyc::CInstance* instance = yyc::FindInstance(id);
        if (instance == nullptr) continue;
        const InstanceInfo info = Describe(instance);
        int32_t item = kNoItem;
        int32_t owner = kOwnerAnyone;
        if (IsDrop(self, info.object)) {
            yyc::RValue value{};
            yyc::GetVar(id, kSlotItem, &value);
            item = static_cast<int32_t>(yyc::ToReal(value));
            yyc::Free(&value);
            owner = byGuest ? kOwnerGuest : kOwnerHost;
        }
        if (owner == kOwnerHost) {
            CollectNow(instance, self);  // the host's loot goes straight into its inventory
            continue;
        }
        ++g_spawnKinds[info.object];
        if (owner != kOwnerGuest) {
            Send(kSpawn, info, id, item, owner);
            continue;
        }
        // Only its guest hears of it: that guest collects its copy on arrival, ours goes - for the others it
        // never existed.
        Send(kSpawn, info, id, item, owner, guest);
        Log("sync: drop of item %d goes to guest %u", item, guest);
        DestroySilently(self, id);
    }
    g_pendingSpawns.clear();
}

// The active instance of `object` standing at (x, y), or nullptr. Off-screen objects are deactivated
// (CullObject) and not found here.
yyc::CInstance* FindAt(yyc::CInstance* self, int object, double x, double y) {
    const int id = static_cast<int>(CallReal(self, "instance_nearest", {x, y, double(object)}));
    if (id == kNoOne) return nullptr;
    yyc::CInstance* instance = yyc::FindInstance(id);
    if (instance == nullptr) return nullptr;
    const double dx = yyc::GetBuiltinReal(instance, yyc::va::YYVar_x) - x;
    const double dy = yyc::GetBuiltinReal(instance, yyc::va::YYVar_y) - y;
    return std::sqrt(dx * dx + dy * dy) <= kMatchDistance ? instance : nullptr;
}

void DestroySilently(yyc::CInstance* self, int id) {
    ++g_applyingRemote;
    CallReal(self, "instance_destroy", {double(id), 0});  // execute_event = false: no drops, no XP
    --g_applyingRemote;
}

// ---- builtin hooks ----

// Guest: what the player builds or places (build menu, placeable items, seeds) must exist on the host too.
// An NPC's reward (Other_12 when its quest is done): the druid's chest for its second quest stayed the guest's
// own (PLAYTEST8.md #12). The quest is done on the guest's copy of the NPC, so the guest reports what it made.
bool CreatedByNpcReward(yyc::CInstance* self) {
    constexpr int kParNpc = 47;  // decompiled/data/objects.tsv
    const StackFrame* top = yyc::TopFrame();
    if (top == nullptr || top->name == nullptr || self == nullptr) return false;
    const size_t length = strlen(top->name);
    constexpr char kReward[] = "_Other_12";
    constexpr size_t kRewardLength = sizeof(kReward) - 1;
    return length > kRewardLength && strcmp(top->name + length - kRewardLength, kReward) == 0 &&
           IsKind(self, self, kParNpc);
}

bool CreatedByGuestAction() {
    const StackFrame* top = yyc::TopFrame();
    if (top == nullptr || top->name == nullptr) return false;
    // A seed is planted by the placement cursor UseSeed creates (objNewSeed), not by UseSeed itself.
    return strcmp(top->name, "gml_Object_objNewBuilding_Step_0") == 0 ||
           strcmp(top->name, "gml_Script_UsePlaceableItem") == 0 || strcmp(top->name, "gml_Script_UseSeed") == 0 ||
           strcmp(top->name, "gml_Object_objNewSeed_Step_0") == 0;
}

// Scrolls (UseScroll) that grow the world: the druid's makes objBigResSpawner, the starfall one objStarfallSpawner.
bool IsScrollSpawner(int object) {
    constexpr int kObjBigResSpawner = 297, kObjStarfallSpawner = 156;  // decompiled/data/objects.tsv
    return object == kObjBigResSpawner || object == kObjStarfallSpawner;
}

// UseScroll sets the spawner's `type` (what it grows: trees, rocks...) after creating it: the guest's are sent a
// step later, with it. Sent at once, the host's spawner kept its Create_0 default - the miner's scroll grew trees.
constexpr int kSlotType = 0x8C0;  // var_slots.tsv
std::vector<int> g_scrollSpawners;  // guest

void SendScrollSpawners() {
    for (const int id : g_scrollSpawners) {
        yyc::CInstance* spawner = yyc::FindInstance(id);
        if (spawner == nullptr) continue;
        yyc::RValue type{};
        const bool known = yyc::GetVar(id, kSlotType, &type) && yyc::IsNumber(type);
        const int32_t kind = known ? static_cast<int32_t>(yyc::ToReal(type)) : kNoItem;
        yyc::Free(&type);
        const InstanceInfo info = Describe(spawner);
        Send(kGuestSpawn, info, info.id, kind);
        Log("sync: scroll spawner %d (type %d) at (%.0f, %.0f), sent to the host", info.object, kind, info.x,
            info.y);
    }
    g_scrollSpawners.clear();
}

bool CreatedByScroll() {
    const StackFrame* top = yyc::TopFrame();
    return top != nullptr && top->name != nullptr && strcmp(top->name, "gml_Script_UseScroll") == 0;
}

// Playtest 8: a guest's sigil showed only the boss's name, the wizard's scroll cleared the ground but no animals
// came - creatures are the host's (mobs.cpp streams them; the guest drops its own). The host makes them instead.
bool CreatedBySummon() {
    const StackFrame* top = yyc::TopFrame();
    return top != nullptr && top->name != nullptr &&
           (strcmp(top->name, "gml_Script_UseSigil") == 0 || strcmp(top->name, "gml_Script_UseScroll") == 0);
}

// Enemies (bosses too) and critters - what mobs.cpp streams.
bool IsCreature(yyc::CInstance* self, int object) {
    constexpr int kParCritter = 64;  // decompiled/data/objects.tsv
    for (const int parent : {static_cast<int>(yyc::obj::parEnemy), kParCritter}) {
        if (object == parent || CallReal(self, "object_is_ancestor", {double(object), double(parent)}) != 0) return true;
    }
    return false;
}

// The world is being loaded: the room entered again (back from the skill tree, a building), a zone read from the
// save. What that creates is in every player's world already; sent as new, it would be doubled at the guests
// (PLAYTEST3.md #1-#3).
bool CreatedByLoading() {
    for (const StackFrame* frame = yyc::TopFrame(); frame != nullptr;
         frame = frame->prev) {
        if (frame->name == nullptr) continue;
        if (strcmp(frame->name, "gml_Script_LoadZone") == 0 || strncmp(frame->name, "gml_Script_Deserialize", 22) == 0) {
            return true;
        }
    }
    return false;
}

int g_highestCreated = 0;  // the highest instance id created through instance_create_* (seen in a session)

void __cdecl CreateHook(yyc::RValue* result, yyc::CInstance* self, yyc::CInstance* other, int argc,
                        yyc::RValue* args) {
    g_origCreate(result, self, other, argc, args);
    g_highestCreated = (std::max)(g_highestCreated, static_cast<int>(yyc::ToReal(*result)));
    if (g_applyingRemote > 0 || !Connected() || interiors::IsBuilding()) return;
    if (IsHost() && CreatedByLoading()) {
        static DWORD lastLog = 0;
        static unsigned skipped = 0;
        ++skipped;
        if (GetTickCount() - lastLog > 5000) {
            lastLog = GetTickCount();
            Log("sync: %u objects loaded from the save so far - not sent", skipped);
        }
        return;
    }
    const int id = static_cast<int>(yyc::ToReal(*result));
    yyc::CInstance* created = yyc::FindInstance(id);
    if (created == nullptr) return;
    const int object = static_cast<int>(yyc::GetBuiltinReal(created, yyc::va::YYVar_object_index));
    if (IsGuest()) familiars::OnCreated(self, id, object);
    if (IsGuest() && IsScrollSpawner(object) && CreatedByScroll()) {
        // Its spawns would be the guest's own (skipped here): the host runs the spawner (playtest 4, bug 4).
        g_scrollSpawners.push_back(id);
        return;
    }
    if (IsGuest() && CreatedBySummon() && IsCreature(self, object)) {
        const InstanceInfo info = Describe(created);
        Send(kGuestSummon, info, info.id);
        Log("sync: summoned creature %d at (%.0f, %.0f), sent to the host", info.object, info.x, info.y);
        return;
    }
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
    } else if (IsGuest() && (CreatedByGuestAction() || (CreatedByNpcReward(self) && !IsDrop(self, object)))) {
        const InstanceInfo info = Describe(created);
        Send(kGuestSpawn, info, info.id);
        Log("sync: built object %d at (%.0f, %.0f), sent to the host", info.object, info.x, info.y);
    }
}

// Every instance_destroy ends up here (YYC calls it directly, not through the builtin table).
// id == -1: self; otherwise an instance id (or an object index, which is not tracked).
void __cdecl DestroyHook(yyc::CInstance* self, yyc::CInstance* other, int id, int executeEvent) {
    yyc::CInstance* target = id == -1 ? self : id >= 100000 ? yyc::FindInstance(id) : nullptr;
    const bool report = target != nullptr && g_applyingRemote == 0 && !g_dryHit && Connected() &&
                        !interiors::IsInterior(static_cast<int>(yyc::GetBuiltinReal(target, yyc::va::YYVar_id)));
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
    const int targetId = target != nullptr ? static_cast<int>(yyc::GetBuiltinReal(target, yyc::va::YYVar_id)) : 0;
    g_origDestroy(self, other, id, executeEvent);
    if (forGuest) puppet::EndActing();
    if (targetId != 0 && yyc::FindInstance(targetId) == nullptr) {
        mobs::ForgetInstance(self, targetId);
        world::OnInstanceDestroyed();
        g_hostOwnedDrops.erase(targetId);  // destroyed quietly too (a host destroy): not only when reported
    }
    if (!synced || chests::IsTreasureChest(info.object)) return;  // an opened chest is gone for its opener only

    if (IsHost()) {
        // Created and gone within one step: the guest never heard of it.
        const auto pending = std::find(g_pendingSpawns.begin(), g_pendingSpawns.end(), info.id);
        if (pending != g_pendingSpawns.end()) {
            g_pendingSpawns.erase(pending);
            g_guestDrops.erase(info.id);
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
constexpr int kLastDungeonRoom = 21;   // ... rmVolcanoSpaceTower; 22-24 are interiors (wizard tower, druid tree, museum)
yyc::BuiltinFn g_origRoomGoto = nullptr;

void __cdecl RoomGotoHook(yyc::RValue* result, yyc::CInstance* self, yyc::CInstance* other, int argc,
                          yyc::RValue* args) {
    const int room = argc >= 1 ? static_cast<int>(yyc::ToReal(args[0])) : -1;
    if (skilltab::InterceptRoomGoto(self, room)) return;  // the skill tree opens/closes in the world room
    if (interiors::InterceptRoomGoto(self, room)) return;  // interiors are built in the world room
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
    if (IsGuest() && Connected() && !stations::IsOwnCraft(self)) return result;  // the host's, from its save
    ++g_crafting;
    yyc::RValue* r = g_origHandleCrafting(self, other, result, argc, args);
    --g_crafting;
    return r;
}

// Guest: a repair kit (UseRepairKit: every structure near the player back to hpMax) mends the host's structures,
// not only the guest's copies of them (playtest 4, bug 6).
yyc::RValue* __cdecl RepairKitHook(yyc::CInstance* self, yyc::CInstance* other, yyc::RValue* result, int argc,
                                   yyc::RValue** args) {
    if (!IsGuest() || !Connected()) return g_origRepairKit(self, other, result, argc, args);
    constexpr int kParStructure = yyc::obj::parStructure;
    std::vector<std::pair<int, float>> before;
    const int count = static_cast<int>(CallReal(self, "instance_number", {double(kParStructure)}));
    for (int i = 0; i < count; ++i) {
        const int id = static_cast<int>(CallReal(self, "instance_find", {double(kParStructure), double(i)}));
        if (yyc::FindInstance(id) != nullptr) before.emplace_back(id, ReadVar(id, kSlotHp));
    }
    yyc::RValue* r = g_origRepairKit(self, other, result, argc, args);
    for (const auto& [id, hp] : before) {
        yyc::CInstance* structure = yyc::FindInstance(id);
        if (structure == nullptr || ReadVar(id, kSlotHp) == hp) continue;
        const InstanceInfo info = Describe(structure);
        const HpMsg msg{HostIdOf(id), info.object, static_cast<float>(info.x), static_cast<float>(info.y),
                        ReadVar(id, kSlotHp), ReadVar(id, kSlotHpAlpha), ReadVar(id, kSlotHpLast),
                        ReadVar(id, kSlotHpWhite), kNoHitter};
        net::Send(kObjectHp, &msg, sizeof(msg));
        Log("sync: repaired structure %d at (%.0f, %.0f): hp %.0f -> %.0f, sent to the host", info.object, info.x,
            info.y, hp, msg.hp);
    }
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
    // The guest's own run of world code (SpawnLand for an island someone bought, dirt growing) earns nothing: it
    // gave ~45 XP per island bought (playtest 4, bug 8).
    if (IsGuest() && g_suppressing > 0) return result;
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
    const StackFrame* top = yyc::TopFrame();
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

// Host: after a hit, the guest's copy shows the same health bar (and the hit itself, when `hitter` is not that guest).
void SendHp(yyc::CInstance* target, uint8_t hitter = kNoHitter) {
    if (!IsHost() || !Connected() || target == nullptr) return;
    const InstanceInfo info = Describe(target);
    const HpMsg msg{info.id, info.object, static_cast<float>(info.x), static_cast<float>(info.y),
                    ReadVar(info.id, kSlotHp), ReadVar(info.id, kSlotHpAlpha), ReadVar(info.id, kSlotHpLast),
                    ReadVar(info.id, kSlotHpWhite), hitter};
    net::Send(kObjectHp, &msg, sizeof(msg));
}

void ApplyHp(yyc::CInstance* self, const HpMsg& msg) {
    if (!std::isfinite(msg.hp) || !std::isfinite(msg.alpha) || !std::isfinite(msg.last) || !std::isfinite(msg.white)) {
        return;
    }
    if (IsHost() && !IsSynced(self, msg.object)) return;  // a guest repairs world objects only
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
    constexpr int kParEnemy = yyc::obj::parEnemy;
    return object == kParEnemy || CallReal(self, "object_is_ancestor", {double(object), double(kParEnemy)}) != 0;
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

// Guest: another player's tool hit (the host's, or a guest's passed on by the host) is shown here as well, as our
// own hits are. Our own come back too: we have no puppet of ourselves, so those are not shown twice.
void ShowOthersHit(yyc::CInstance* self, const HpMsg& msg) {
    if (msg.hitter == kNoHitter || IsEnemy(self, msg.object)) return;
    yyc::CInstance* hitter = puppet::Instance(msg.hitter);
    const int local = hitter != nullptr ? Resolve(self, msg.id, msg.object, msg.x, msg.y) : 0;
    yyc::CInstance* target = local != 0 ? yyc::FindInstance(local) : nullptr;
    if (target == nullptr) return;
    puppet::BeginActing(msg.hitter);
    DryHit(target, hitter);
    puppet::EndActing();
}

// Guest: a tool hit is `with (target) event_user(0)` with the player as `other`; the host decides its outcome.
// Host: its own tool hits are shown on the guest's copy (health bar).
void __cdecl EventUserHook(yyc::CInstance* self, yyc::CInstance* other, int n) {
    if (IsHost() && n == 0 && other != nullptr && other == g_localPlayer && Connected() && CalledFromToolUse()) {
        const int id = static_cast<int>(yyc::GetBuiltinReal(self, yyc::va::YYVar_id));
        g_hitByGuest.erase(id);  // the last hit decides who gets the drop (playtest 4, bug 1)
        const bool synced = IsSynced(self, static_cast<int>(yyc::GetBuiltinReal(self, yyc::va::YYVar_object_index)));
        g_origEventUser(self, other, n);
        if (synced) SendHp(yyc::FindInstance(id), net::kHostPeer);
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
    const int id = static_cast<int>(CallReal(self, "instance_create_depth", {x, y, depth, double(object)}));
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
        if (msg.stack >= 1 && msg.stack <= kMaxStack) {
            yyc::RValue stack = yyc::Real(std::floor(msg.stack));
            yyc::SetVar(id, kSlotStack, &stack);
        }
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
            Log("sync: our drop of item %d x%.0f collected", msg.item, msg.stack);
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
    } else if (CallReal(self, "instance_exists", {double(local)}) == 0) {  // false for deactivated instances
        return false;
    }
    DestroySilently(self, local);
    return true;
}

void ApplyDestroy(yyc::CInstance* self, const ObjectMsg& msg) {
    if (g_destroyedOnHost.insert(msg.id).second) {
        g_destroyedOrder.push_back(msg.id);
        if (g_destroyedOrder.size() > kRememberDestroyed) {
            g_destroyedOnHost.erase(g_destroyedOrder.front());
            g_destroyedOrder.pop_front();
        }
    }
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
        WakeInstance(self, local);
    } else {
        WakeRegion(self, msg.x - kMatchDistance, msg.y - kMatchDistance, 2 * kMatchDistance, 2 * kMatchDistance);
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


// Host: the instance a guest message refers to (by the host id it carries, else by object and position) - only a
// world object of the kind the message names (an id alone could name anything, the host's own player too).
yyc::CInstance* ResolveOnHost(yyc::CInstance* self, const ObjectMsg& msg) {
    if (!IsSynced(self, msg.object)) return nullptr;
    if (msg.id > 0) {
        yyc::CInstance* instance = yyc::FindInstance(msg.id);
        if (instance != nullptr && IsKind(self, instance, msg.object)) return instance;
        if (instance != nullptr) {
            Log("sync: a guest named instance %d as object %d, but it is object %.0f", msg.id, msg.object,
                yyc::GetBuiltinReal(instance, yyc::va::YYVar_object_index));
        }
    }
    return FindAt(self, msg.object, msg.x, msg.y);
}

bool IsFinite(const ObjectMsg& msg) { return std::isfinite(msg.x) && std::isfinite(msg.y) && std::isfinite(msg.depth); }

// `woken`: the second try, after waking up the object's kind (the host may have culled what the guest hit).
void ApplyHit(yyc::CInstance* self, net::PeerId from, const ObjectMsg& msg, bool woken = false) {
    yyc::CInstance* target = ResolveOnHost(self, msg);
    yyc::CInstance* guest = puppet::Instance(from);
    if (target == nullptr && guest != nullptr && !woken) {
        WakeThen(self, msg.object, [self, from, msg] { ApplyHit(self, from, msg, true); });
        return;
    }
    Log("sync: guest hit object %d (host id %d) at (%.0f, %.0f): %s (dmg %.1f, guest %u)", msg.object, msg.id,
        msg.x, msg.y, target == nullptr ? "not found" : guest == nullptr ? "no puppet" : "applied", msg.depth, from);
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
    SendHp(yyc::FindInstance(targetId), from);  // gone if the hit destroyed it
}

// Host: build what the guest built (as the guest, for anything that asks who "the player" is) and tell
// the guest which instance it became.
void ApplyGuestSpawn(yyc::CInstance* self, net::PeerId from, const ObjectMsg& msg) {
    if (!IsFinite(msg) || (!IsSynced(self, msg.object) && !IsScrollSpawner(msg.object))) {
        Log("sync: guest %u asked to build object %d - not something players build; ignored", from, msg.object);
        return;
    }
    puppet::BeginActing(from);
    ++g_applyingRemote;
    const int id = static_cast<int>(
        CallReal(self, "instance_create_depth", {msg.x, msg.y, msg.depth, double(msg.object)}));
    --g_applyingRemote;
    puppet::EndActing();
    yyc::CInstance* built = yyc::FindInstance(id);
    if (built == nullptr) return;
    if (IsScrollSpawner(msg.object) && msg.item != kNoItem) {
        yyc::RValue type = yyc::Real(msg.item);
        yyc::SetVar(id, kSlotType, &type);
    }
    const ObjectMsg ack{id, msg.object, msg.x, msg.y, msg.depth, msg.id};
    net::SendTo(from, kGuestSpawnAck, &ack, sizeof(ack));
    SendToOtherGuests(from, kSpawn, Describe(built), id);
    Log("sync: guest built object %d at (%.0f, %.0f) -> %d (guest %u)", msg.object, msg.x, msg.y, id, from);
}

// Host: the creature a guest's sigil or scroll made. Ours from here: streamed to the guests like any enemy (the
// guest's own copy is adopted by position or dropped).
void ApplyGuestSummon(yyc::CInstance* self, net::PeerId from, const ObjectMsg& msg) {
    if (!IsFinite(msg) || !IsCreature(self, msg.object)) {
        Log("sync: guest %u summoned object %d - not a creature; ignored", from, msg.object);
        return;
    }
    puppet::BeginActing(from);
    ++g_applyingRemote;
    const int id = static_cast<int>(
        CallReal(self, "instance_create_depth", {msg.x, msg.y, msg.depth, double(msg.object)}));
    --g_applyingRemote;
    puppet::EndActing();
    Log("sync: guest %u summoned object %d at (%.0f, %.0f) -> %d", from, msg.object, msg.x, msg.y, id);
}

void ApplyGuestDestroyed(yyc::CInstance* self, net::PeerId from, const ObjectMsg& msg) {
    yyc::CInstance* target = ResolveOnHost(self, msg);
    if (target == nullptr) return;
    const InstanceInfo info = Describe(target);
    // Enemies live and die at the host: a replica the guest lost (a death it played out ahead of the host) must
    // not take the real one away without its drops and XP.
    if (IsEnemy(self, info.object)) return;
    DestroySilently(self, info.id);
    SendToOtherGuests(from, kDestroy, info, info.id);
}

void GiveXp(yyc::CInstance* localPlayer, double amount) {
    yyc::RValue value = yyc::Real(amount);
    yyc::RValue* args[1] = {&value};
    yyc::RValue result{};
    g_origGainXp(localPlayer, localPlayer, &result, 1, args);
    yyc::Free(&result);
}

// Host: XP the guest earned, added to the shared progress.
void ApplyXp(yyc::CInstance* localPlayer, const XpMsg& msg) {
    constexpr float kMaxXpAtOnce = 1e6f;
    if (!(msg.amount >= 0 && msg.amount <= kMaxXpAtOnce)) return;  // NaN fails both
    Log("sync: +%.1f XP from the guest", msg.amount);
    GiveXp(localPlayer, msg.amount);
}

}  // namespace

// What sync_debug.cpp may use (sync_internal.h).
namespace internal {

bool IsHost() { return sync::IsHost(); }
bool IsGuest() { return sync::IsGuest(); }
bool Connected() { return sync::Connected(); }
InstanceInfo Describe(yyc::CInstance* instance) { return sync::Describe(instance); }
bool IsEnemy(yyc::CInstance* self, int object) { return sync::IsEnemy(self, object); }

void Hit(yyc::CInstance* localPlayer, yyc::CInstance* target) {
    const InstanceInfo info = sync::Describe(target);
    SendHit(info);
    if (!sync::IsEnemy(localPlayer, info.object)) DryHit(target, localPlayer);  // as a real tool hit shows it
}

void ReportBuild(const InstanceInfo& info) { Send(kGuestSpawn, info, info.id); }
void GiveXp(yyc::CInstance* localPlayer, double amount) { sync::GiveXp(localPlayer, amount); }
CraftingScope::CraftingScope() { ++g_crafting; }
CraftingScope::~CraftingScope() { --g_crafting; }

}  // namespace internal

bool InstallHooks(bool (*isHost)(), bool (*isGuest)()) {
    g_isHost = isHost;
    g_isGuest = isGuest;
    bool ok = Hook(kScriptResourceSpawn, reinterpret_cast<void*>(&ResourceSpawnHook), &g_origResourceSpawn);
    ok = Hook(kScriptResourceSpawnSingle, reinterpret_cast<void*>(&ResourceSpawnSingleHook),
              &g_origResourceSpawnSingle) && ok;
    ok = Hook(kScriptDirtProcess, reinterpret_cast<void*>(&DirtProcessHook), &g_origDirtProcess) && ok;
    ok = Hook(kScriptGainXp, reinterpret_cast<void*>(&GainXpHook), &g_origGainXp) && ok;
    ok = Hook(kScriptHandleCrafting, reinterpret_cast<void*>(&HandleCraftingHook), &g_origHandleCrafting) && ok;
    ok = Hook(kScriptUseRepairKit, reinterpret_cast<void*>(&RepairKitHook), &g_origRepairKit) && ok;
    ok = Hook(kScriptUnlockAchievement, reinterpret_cast<void*>(&UnlockAchievementHook), &g_origUnlockAchievement) &&
         ok;
    ok = Hook(kEventUser, reinterpret_cast<void*>(&EventUserHook), &g_origEventUser) && ok;
    ok = Hook(kResourceCollect, reinterpret_cast<void*>(&CollectHook), &g_origCollect) && ok;
    ok = Hook(kInstanceDestroy, reinterpret_cast<void*>(&DestroyHook), &g_origDestroy) && ok;
    return ok;
}

// The shared progress lives in the host's player: level, xp, xpCap (XP for the next level), xpToGain (XP still
// being counted up). Skill points are each player's own (skills.h). The host sends it twice a second; the guest's
// player shows it.
constexpr int kProgressSlots[] = {0x603, 0x90E, 0x910, 0x912};  // decompiled/code/var_slots.tsv
constexpr unsigned kProgressEverySteps = 30;

void SendProgress(yyc::CInstance* localPlayer) {
    if (g_steps % kProgressEverySteps != 0) return;
    const int id = static_cast<int>(yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_id));
    float values[4] = {};
    for (int i = 0; i < 4; ++i) values[i] = ReadVar(id, kProgressSlots[i]);
    const ProgressMsg msg{values[0], values[1], values[2], values[3]};
    net::Send(kProgress, &msg, sizeof(msg));
}

void ApplyProgress(yyc::CInstance* localPlayer, const ProgressMsg& msg) {
    static float shownLevel = -1;
    const int id = static_cast<int>(yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_id));
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

void Tick(yyc::CInstance* localPlayer) {
    g_localPlayer = localPlayer;
    internal::DebugTick(localPlayer, g_steps);
    if (!g_builtinsHooked) HookBuiltins();
    if (IsHost()) FlushSpawns(localPlayer);
    if (IsHost()) SendProgress(localPlayer);
    if (IsGuest()) RetryDestroys(localPlayer, true);
    if (IsGuest()) CollectCrafted(localPlayer);
    if (IsGuest()) SendScrollSpawners();
    if (++g_steps % kStatsEverySteps == 0) {
        Log("sync: sent spawn %u destroy %u hit %u gone %u | applied spawn %u destroy %u hit %u gone %u",
            g_sent[kSpawn], g_sent[kDestroy], g_sent[kHit], g_sent[kGuestDestroyed], g_applied[kSpawn],
            g_applied[kDestroy], g_applied[kHit], g_applied[kGuestDestroyed]);
        LogSpawnKinds();
    }
}

// Actions for the next CullObject (WakeThen, RunBeforeCull). They hold the player and other instances as they were:
// one that waited long (the player was in another room, where nothing culls) may point at instances gone since, so
// past kAfterWakeMs it is dropped; and the queue is capped.
struct AfterWake {
    DWORD at;
    std::function<void()> action;
};
std::vector<AfterWake> g_afterWake;
constexpr DWORD kAfterWakeMs = 2000;
constexpr size_t kMaxAfterWake = 256;

void QueueAfterWake(std::function<void()> action) {
    if (g_afterWake.size() >= kMaxAfterWake) {
        Log("sync: too many actions waiting for the next cull - the oldest dropped");
        g_afterWake.erase(g_afterWake.begin());
    }
    g_afterWake.push_back({GetTickCount(), std::move(action)});
}

// What ProcessCulls sets on an instance it wakes. `active` = 1 above all: the game's IsDeactivated reads that
// variable, not the runtime's flag. Left 0, the next TrueInstanceExists/Find on the kind took the awake instance for
// a sleeper - TemporaryActivate, then ClearTemporaryInstances put it to sleep on no list: no camera woke it again
// (playtest 6: trees, fish traps, the bank and vaults vanishing at the guest, reconcile adding an empty vault in the
// hidden one's place). And off the queue ClearTemporaryInstances puts back to sleep: one TemporaryActivate woke for
// a moment (still on the sleep lists) went back to sleep after we took it off them (the fairy queen in
// sleeplists.ps1).
void MarkAwake(yyc::CInstance* self, int id) {
    constexpr int kSlotActive = 0x4F, kSlotStashId = 0x823, kSlotStashList = 0x824, kSlotTempActive = 0x860;
    constexpr int kSlotTemporaryActivations = 0x863;  // global.temporaryInstanceActivations
    yyc::RValue active = yyc::Real(1);
    yyc::SetVar(id, kSlotActive, &active);
    yyc::RValue notTemporary = yyc::Real(0);
    yyc::SetVar(id, kSlotTempActive, &notTemporary);
    for (const int slot : {kSlotStashId, kSlotStashList}) {
        yyc::RValue none{};
        none.kind = yyc::kUndefined;
        yyc::SetVar(id, slot, &none);
    }
    const yyc::RValue* temporary = yyc::GlobalVar(kSlotTemporaryActivations);
    if (temporary == nullptr || !yyc::IsNumber(*temporary)) return;
    const double list = yyc::ToReal(*temporary);
    for (double at; (at = CallReal(self, "ds_list_find_index", {list, double(id)})) >= 0;) {
        CallReal(self, "ds_list_delete", {list, at});
    }
}

// Waking as ProcessCulls does: off global.deactivatedInstances and off the cell's list it was culled into
// (`stashList`), then activated and marked awake. A bare instance_activate_* left the id in those lists: when the
// instance moved on and died, ProcessCulls later met it in the old cell - "Unable to find any instance" (a GML
// error: the host's crash after a guest killed a woken boar) - and SaveGame met sleepers it did not know of
// (playtest 4, bug 10).
void WakeWhere(yyc::CInstance* self, const std::function<bool(yyc::CInstance*)>& pick) {
    constexpr int kSlotDeactivated = 0x16C;  // decompiled/code/var_slots.tsv
    const yyc::RValue* listValue = yyc::GlobalVar(kSlotDeactivated);
    if (listValue == nullptr || !yyc::IsNumber(*listValue)) return;
    const double list = yyc::ToReal(*listValue);
    std::vector<int> woken;
    const bool bare = Mutant("wake");  // the old way: activated, left on the lists
    for (int i = static_cast<int>(CallReal(self, "ds_list_size", {list})) - 1; i >= 0; --i) {
        const int id = static_cast<int>(CallReal(self, "ds_list_find_value", {list, double(i)}));
        yyc::CInstance* instance = yyc::FindInstance(id);
        if (instance != nullptr && !pick(instance)) continue;
        if (!bare) CallReal(self, "ds_list_delete", {list, double(i)});  // gone ones go too: nothing can wake them
        if (instance == nullptr) continue;
        if (std::find(woken.begin(), woken.end(), id) == woken.end()) woken.push_back(id);
    }
    for (const int id : woken) {
        // The cell lists hold [id, box] arrays: looking the id up there as a number (until 2026-10-25) found nothing.
        if (!bare) mobs::ForgetInstance(self, id);
        CallReal(self, "instance_activate_object", {double(id)});
        if (!bare && !Mutant("active")) MarkAwake(self, id);  // the mutant: `active` left 0, as until playtest 6
    }
}

void WakeKind(yyc::CInstance* self, int object) {
    WakeWhere(self, [self, object](yyc::CInstance* instance) {
        const int kind = static_cast<int>(yyc::GetBuiltinReal(instance, yyc::va::YYVar_object_index));
        return kind == object || CallReal(self, "object_is_ancestor", {double(kind), double(object)}) != 0;
    });
}

void WakeRegion(yyc::CInstance* self, double left, double top, double width, double height) {
    WakeWhere(self, [=](yyc::CInstance* instance) {
        const double x = yyc::GetBuiltinReal(instance, yyc::va::YYVar_x);
        const double y = yyc::GetBuiltinReal(instance, yyc::va::YYVar_y);
        return x >= left && x <= left + width && y >= top && y <= top + height;
    });
}

void WakeInstance(yyc::CInstance* self, int id) {
    WakeWhere(self, [id](yyc::CInstance* instance) {
        return static_cast<int>(yyc::GetBuiltinReal(instance, yyc::va::YYVar_id)) == id;
    });
}

void WakeThen(yyc::CInstance* self, int object, std::function<void()> action) {
    WakeKind(self, object);
    QueueAfterWake(std::move(action));
}

void RunBeforeCull(std::function<void()> action) { QueueAfterWake(std::move(action)); }

void BeforeCull(yyc::CInstance* self) {
    if (IsGuest()) RetryDestroys(self, false);
    if (!g_afterWake.empty()) {
        std::vector<AfterWake> actions;
        actions.swap(g_afterWake);
        const DWORD now = GetTickCount();
        size_t stale = 0;
        for (const AfterWake& waiting : actions) {
            if (now - waiting.at > kAfterWakeMs) {
                ++stale;
                continue;
            }
            waiting.action();
        }
        if (stale > 0) Log("sync: %zu actions waited too long for a cull (away from the world?) - dropped", stale);
    }
    internal::DebugBeforeCull(self, g_localPlayer);
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

int HighestCreatedId() { return g_highestCreated; }

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
        yyc::CInstance* instance = local != 0 ? yyc::FindInstance(local) : nullptr;
        if (instance != nullptr && IsKind(self, instance, object)) return local;
    }
    yyc::CInstance* found = FindAt(self, object, x, y);
    return found != nullptr ? static_cast<int>(yyc::GetBuiltinReal(found, yyc::va::YYVar_id)) : 0;
}

bool Handle(const net::Message& message, yyc::CInstance* localPlayer) {
    if (internal::DebugHandle(message, localPlayer)) return true;
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
            if (size == sizeof(ObjectMsg) && IsHost()) ApplyGuestDestroyed(localPlayer, message.from, ReadObject(message));
            return true;
        case kObjectHp:
            if (size == sizeof(HpMsg)) {
                HpMsg msg{};
                memcpy(&msg, message.payload.data(), sizeof(msg));
                if (IsGuest()) ShowOthersHit(localPlayer, msg);
                ApplyHp(localPlayer, msg);
                if (IsHost()) {  // a guest's repair: the other guests see it too
                    const int local = Resolve(localPlayer, msg.id, msg.object, msg.x, msg.y);
                    if (local != 0) SendHp(yyc::FindInstance(local));
                }
            }
            return true;
        case kGuestSpawn:
            if (size == sizeof(ObjectMsg) && IsHost()) ApplyGuestSpawn(localPlayer, message.from, ReadObject(message));
            return true;
        case kGuestSummon:
            if (size == sizeof(ObjectMsg) && IsHost()) ApplyGuestSummon(localPlayer, message.from, ReadObject(message));
            return true;
        case kGuestSpawnAck:
            if (size == sizeof(ObjectMsg) && IsGuest()) {
                const ObjectMsg ack = ReadObject(message);
                if (yyc::FindInstance(ack.item) != nullptr) AdoptReplica(ack.id, ack.item);
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

void ResetTransient() {
    g_applyingRemote = 0;
    g_dryHit = false;
    g_crafting = 0;
}

void Reset() {
    ResetTransient();
    g_hostToLocal.clear();
    g_localToHost.clear();
    g_hitByGuest.clear();
    g_destroyedOnHost.clear();
    g_destroyedOrder.clear();
    g_suppressing = 0;
    g_suppressed.clear();
    g_guestDrops.clear();
    g_hostOwnedDrops.clear();
    g_pendingSpawns.clear();
    g_pendingDestroys.clear();
    g_craftedDrops.clear();
    g_localDrops.clear();
    g_scrollSpawners.clear();
    g_afterWake.clear();
}

}  // namespace coop::sync
