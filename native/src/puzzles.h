// Puzzles in coop (PLAYTEST8.md #18): a dungeon's, and the world's pieces (the desert's pillars to push down). The
// host's pieces are the real ones:
//  - a guest's E on a puzzle piece (push a cube, turn a mirror, a runic pillar, a brazier, a terminal, a pillar to
//    push down) is not done at the guest: the host does it as the guest's puppet;
//  - a gem socket takes the gem from the inventory and the blood altar opens a choice of boons, so there the guest's
//    own E stays its own and its result (solved, used) goes to the host;
//  - what the pieces are (solved, down, facing, order, a terminal's symbol, electrified, a cube's place) goes from the host
//    to the guests whenever it changes;
//  - doors, barriers and breakable walls are world objects (sync.cpp): opened at the host, gone at the guests;
//  - ice (a frozen block with the boss key, a frozen door) melts at everyone from anyone's fire; anyone's E on the boss
//    key makes every player's key follow that player (the boss door opens for a key following the local player).
// Lanterns have their own exchange (dungeons.cpp).
#pragma once

#include "net.h"
#include "yyc.h"

namespace coop::puzzles {

// Creates the hooks (enabled by the caller).
bool InstallHooks(bool (*isHost)(), bool (*isGuest)());

// Game thread, every local player step in the shared room (the world or a dungeon).
void Tick(yyc::CInstance* localPlayer);

// Game thread: applies a puzzle message; false if `message` is not one.
bool Handle(const net::Message& message, yyc::CInstance* localPlayer);

// Ice blocks (objFrozenChest): each player's block melts by itself, from the fire synced here, and makes that
// player's own boss key - so a melted block is no destroy for the others (sync.cpp, reconcile.cpp).
bool MeltsAtEachSide(int object);

// Game thread: `id` (object `object`) was destroyed here; a melted ice block melts at the others too.
void OnDestroyed(int id, int object);

// From the instance_create hook, while connected: a guest's puzzle reward chest (the host's reaches it) is removed.
void OnCreated(int createdId, int object);

// Guest, from sync.cpp: a replica of the host's object `object` was just made here as `id`. The ice's reward chest is
// made already `grounded` at the host (objFrozenChest's Step_2): its replica would play the whole fall; so is it here.
void OnReplicaCreated(yyc::CInstance* self, int id, int object);

// /puzzlereset (commands.cpp): puts puzzle `number` (the puzzle list's numbering, the wiki's order) back as it was -
// pieces empty and unsolved - here, and at every other player when `share`. False if there is no such puzzle or it
// cannot be reset yet. The supported ones: 5 eye statue, 9 skull braziers, 11 gem sockets (colored pedestals),
// 14 stone princess; and two whose piece leaves the world when solved, made again by the host where it stood (noted in
// saveDataN/coop_guests/coop_puzzle_spots.txt) with the reward chests around it removed: 15 frozen chest (the ice block),
// 20 giant crystal (not one of the 19 puzzles: its island's feat); and 18, the fire island's four burning chests, made
// again where they stood (noted while burning), on fire.
bool ResetPuzzle(yyc::CInstance* self, int number, bool share);

// A new room: what was known about the last one's pieces goes.
void Reset();

}  // namespace coop::puzzles
