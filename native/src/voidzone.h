// The Void, together (docs/coop/VOID.md). In the game it is not a room but a zone of the world room: the Void
// portal's VoidLoad raises global.gameModifier (the level) and ZoneGoTo(1) rebuilds rmGame as the Void, where
// VoidControl generates the level, counts the timer down and opens a portal on once every enemy is dead; a timer
// at 0 (or a death) shows the leaderboard, whose button goes back to the world (ZoneGoTo(0)).
// In coop the host plays the level and the guests follow it, as into dungeons: each level is a place of its own
// (kPlaceVoid + level) in dungeons.cpp's shared-room logic. Every side generates the level from the host's seed,
// but what it spawns comes from the host only (the guests' copies are removed, the host's sent as usual), and the
// host's land map puts the guests' terrain right. The timer and "level complete" are the host's.
#pragma once

#include "net.h"
#include "yyc.h"

namespace coop::voidzone {

constexpr int kPlaceVoid = 1000;  // the place of Void level N is kPlaceVoid + N

bool IsVoidPlace(int place);

// The local game is in the Void zone (IsVoidZone).
bool InVoid(yyc::CInstance* any);

// Where this player is: its room, or kPlaceVoid + level in the Void.
int Place(yyc::CInstance* any);

// Creates the hooks (enabled by the caller).
bool InstallHooks(bool (*isHost)(), bool (*isGuest)());

// Guest: goes where the host said (`place`: a Void level, or the world from the Void). False if `place` is not the
// Void's business (a dungeon, the world from a dungeon).
bool GuestGo(yyc::CInstance* player, int place);

// Host: a guest asked for `place` (the next level through a portal, or back to the world): the host goes there.
// False if it is not the Void's business.
bool HostGo(yyc::CInstance* player, int place);

// Game thread: applies a Void message; false if `message` is not one.
bool Handle(const net::Message& message, yyc::CInstance* localPlayer);

// Game thread, every step in the shared place: the host sends the level's state, a guest acts on it.
void Tick(yyc::CInstance* localPlayer);

// Where the level at `place` put this player (its generation does, after the place has begun); false if unknown.
bool StartPoint(int place, double* x, double* y);

// Forgets the last session.
void Reset();

}  // namespace coop::voidzone
