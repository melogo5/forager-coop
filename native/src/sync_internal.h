// What sync.cpp shares with sync_debug.cpp (its testing aids, kept out of the main file). Not for other modules:
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
