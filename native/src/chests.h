// Treasure chests in a session. A key chest (objTreasureChest) opens once every player has put a key in it (pressing E
// with one), and then all of them get the same reward at once; a keyless one opened by anyone opens for everyone, each
// with their own loot (playtest 5: with each player's own chest, a rejoin brought back the host's world, where the
// host had opened it). Their spawns are the host's, and so is where a new one comes to rest.
#pragma once

#include "net.h"
#include "yyc.h"

namespace coop::chests {

// Creates the hooks (enabled by the caller).
bool InstallHooks(bool (*isHost)());

// objTreasureChest and its kin: each side destroys its own copy as it opens - no destroys are sent for them, no
// reconciling.
bool IsTreasureChest(int object);

// Game thread, every local player step while connected: the host sends where new chests came to rest.
void Tick(yyc::CInstance* localPlayer);

// Testing aid: COOP_DEBUG_CHEST=1 opens the nearest treasure chest ~10 s into the world.
void DebugTick(yyc::CInstance* localPlayer);

// Game thread: handles a chest message; false if `message` is not one.
bool Handle(const net::Message& message, yyc::CInstance* localPlayer);

// Host, game thread: `peer` left the session - its key no longer counts, and a chest that now has enough opens.
void OnPeerLeft(yyc::CInstance* localPlayer, net::PeerId peer);

void Reset();

}  // namespace coop::chests
