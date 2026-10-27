// Dungeon puzzles in coop (PLAYTEST8.md #18). The host's dungeon is the real one:
//  - a guest's E on a puzzle piece (push a cube, turn a mirror, a runic pillar, a brazier, a terminal, a pillar to
//    push down) is not done at the guest: the host does it as the guest's puppet;
//  - a gem socket takes the gem from the inventory and the blood altar opens a choice of boons, so there the guest's
//    own E stays its own and its result (solved, used) goes to the host;
//  - what the pieces are (solved, down, facing, order, a terminal's symbol, electrified, a cube's place) goes from the host
//    to the guests whenever it changes;
//  - doors, barriers and breakable walls are world objects (sync.cpp): opened at the host, gone at the guests.
// Lanterns have their own exchange (dungeons.cpp).
#pragma once

#include "net.h"
#include "yyc.h"

namespace coop::puzzles {

// Creates the hooks (enabled by the caller).
bool InstallHooks(bool (*isHost)(), bool (*isGuest)());

// Game thread, every local player step in a dungeon the session is played in.
void Tick(yyc::CInstance* localPlayer);

// Game thread: applies a puzzle message; false if `message` is not one.
bool Handle(const net::Message& message, yyc::CInstance* localPlayer);

// A new room: what was known about the last one's pieces goes.
void Reset();

}  // namespace coop::puzzles
