// Dungeons (rooms 14-21: tomb, skull maze, crystal cave, fire temple, the space towers) are entered by everyone
// together (the user's choice, 2026-10-27): the host decides. The host's own way in or out of a dungeon goes ahead
// and every guest is told to follow; a guest's is refused and asked of the host, who then goes - so everyone does.
// Going in, each player saves its world first as the game does (the guest its copy), and gets it back on the way out.
// While a guest is not yet in the host's room, the host's news waits (session.cpp holds the inbox).
#pragma once

#include "net.h"
#include "yyc.h"

namespace coop::dungeons {

// Tells the module this player's role.
void Install(bool (*isGuest)());

// From the room_goto hook: true when the call is refused (a guest's own way in or out: asked of the host).
bool InterceptRoomGoto(yyc::CInstance* self, int room);

// The place the session is played in now: the host's, as far as this player knows - a room, or a level of the Void
// (voidzone::Place). Outside it nothing is synced.
int SharedRoom(yyc::CInstance* localPlayer);

// Host: everyone into `place` (the host is on its way there) - the Void's levels and its way out (voidzone.cpp).
void HostAnnounce(yyc::CInstance* self, int place);

// Guest: asks the host to go to `place` (the guest's own way there was refused).
void GuestAsk(yyc::CInstance* self, int place);

// Game thread: applies a dungeon message; false if `message` is not one. Sets *stop when the rest of the inbox
// has to wait for the new room.
bool Handle(const net::Message& message, yyc::CInstance* localPlayer, bool* stop);

// Game thread, every step in the shared room: lanterns lit here go to the others.
void Tick(yyc::CInstance* localPlayer);

// Guest, game thread, every step while not in the host's room: if no fade is taking it there (a lost one), starts
// one again after a while.
void GuestTick(yyc::CInstance* localPlayer);

// Testing aid: COOP_DEBUG_DUNGEON=<room>[,<seconds>] - the host goes in (as an entrance would) that many seconds into
// the world (default 8), and back out 20 s later.
void DebugTick(yyc::CInstance* localPlayer, bool isHost);

// The session's place changed to `place` (the local player is there now): remembers where a dungeon begins.
void OnRoomEntered(yyc::CInstance* localPlayer, int place);

// Where a player who died in a dungeon gets back up (where it came in) or in the Void (where the level put it,
// voidzone::StartPoint); false elsewhere. A dungeon's minecarts go back to their starts, so it can ride the way again.
bool RespawnPoint(yyc::CInstance* localPlayer, double* x, double* y);

// Forgets the last session.
void Reset();

}  // namespace coop::dungeons
