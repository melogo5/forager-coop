// What sync.cpp shares with its parts - sync_hits.cpp (hits, health bars), sync_progress.cpp (XP, the shared level),
// sync_wake.cpp (waking sleeping instances) - and with sync_debug.cpp (its testing aids). Not for other modules:
// they use sync.h.
#pragma once

#include <cstdint>

#include "net.h"
#include "yyc.h"

namespace coop::sync {

struct InstanceInfo {
    int id;
    int object;
    double x, y, depth;
};

constexpr int kParResource = 281;  // drops; `item` says what they give
constexpr int kSlotItem = 0x5B8;   // decompiled/code/var_slots.tsv
constexpr int kNoOne = -4;
constexpr uint8_t kCensusType = 18;    // host -> guest (testing aid): write down your world objects now

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
constexpr int32_t kNoItem = -1;
constexpr int32_t kOwnerAnyone = 0;
constexpr int32_t kOwnerHost = 1;
constexpr int32_t kOwnerGuest = 2;

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
    // kSpawn of a skeleton: its bigSkeleton (a bone pile's, PLAYTEST8.md #4); the guest's replica was small.
    float big;
};
#pragma pack(pop)

extern yyc::CInstance* g_localPlayer;
extern int g_applyingRemote;  // >0 while carrying out a peer's change: do not report it back
extern bool g_dryHit;         // guest: inside a hit shown locally only (DryHit)
extern int g_suppressing;     // guest: inside BeginSuppressSpawns/EndSuppressSpawns
extern unsigned g_sent[16];   // messages sent, by type & 15 (the stats line)

bool IsHost();
bool IsGuest();
bool Connected();
bool IsSynced(yyc::CInstance* self, int object);  // one of the world's objects (kSyncedParents)
InstanceInfo Describe(yyc::CInstance* instance);
bool IsEnemy(yyc::CInstance* self, int object);
float ReadVar(int id, int slot);
// Host: the instance a guest message refers to, or nullptr.
yyc::CInstance* ResolveOnHost(yyc::CInstance* self, const ObjectMsg& msg);
bool Hook(uint32_t va, void* detour, void* original);

// sync_hits.cpp
bool InstallHitHooks();
bool HandleHits(const net::Message& message, yyc::CInstance* localPlayer);  // false: not a hit or health message
// Host: the guest whose hit was the last on instance `id` (it gets what destroying it gives); forgotten here.
bool TakeGuestHitter(int id, net::PeerId* guest);
void ForgetHits();

// sync_progress.cpp
bool InstallProgressHooks();
bool HandleProgress(const net::Message& message, yyc::CInstance* localPlayer);  // false: not XP or progress
void SendProgress(yyc::CInstance* localPlayer, unsigned steps);                 // host, every step

// sync_wake.cpp
void WakeKind(yyc::CInstance* self, int object);
void WakeInstance(yyc::CInstance* self, int id);
void RunAfterWake();  // the actions queued for this cull
void ForgetAfterWake();

namespace internal {

bool IsHost();
bool IsGuest();
bool Connected();
InstanceInfo Describe(yyc::CInstance* instance);
bool IsEnemy(yyc::CInstance* self, int object);
// Guest: reports a tool hit on `target` to the host, and shows it here as a real hit would (not for enemies).
void Hit(yyc::CInstance* localPlayer, yyc::CInstance* target);
// Guest: reports something built here to the host.
void ReportBuild(const InstanceInfo& info);
// Adds XP to the local player's (shared) progress, as the game's GainXp.
void GiveXp(yyc::CInstance* localPlayer, double amount);
// While one lives, what is created counts as crafted (a finished craft's drop goes into the inventory).
struct CraftingScope {
    CraftingScope();
    ~CraftingScope();
    CraftingScope(const CraftingScope&) = delete;
    CraftingScope& operator=(const CraftingScope&) = delete;
};

// sync_debug.cpp: the testing aids, switched on by COOP_DEBUG_* variables.
void DebugTick(yyc::CInstance* localPlayer, unsigned steps);  // every local player step
void DebugBeforeCull(yyc::CInstance* self, yyc::CInstance* localPlayer);
// A census message from the host (kCensusType); false if `message` is not one.
bool DebugHandle(const net::Message& message, yyc::CInstance* localPlayer);

}  // namespace internal

}  // namespace coop::sync
