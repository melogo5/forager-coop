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

// The room the session is played in now: the host's, as far as this player knows. Outside it nothing is synced.
int SharedRoom(yyc::CInstance* localPlayer);

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

// The session's room changed to `room` (the local player is there now): remembers where a dungeon begins.
void OnRoomEntered(yyc::CInstance* localPlayer, int room);

// Where a player who died in a dungeon gets back up (where it came in); false outside dungeons.
bool RespawnPoint(yyc::CInstance* localPlayer, double* x, double* y);

// Forgets the last session.
void Reset();

}  // namespace coop::dungeons
