// Interiors (wizard tower, druid tree, museum: rooms 22-24) built inside the world room (docs/coop/INTERIORS.md): in
// a session, going in no longer leaves the world room - which would stop the shared world while the host is inside.
// The interior is put together past the world's edge and the player moved there; its door brings the player back.
// All three (from decompiled/data/rooms_interiors.txt via tools/gen_interiors.py); COOP_ROOM_INTERIORS=1 turns it off.
// Testing aid: COOP_DEBUG_DUMP_ROOMS=1 writes what an interior room is made of to the log.
#pragma once

#include "yyc.h"

namespace coop::interiors {

// Game thread, every local player step (in any room): keeps track of the local player; testing aids.
void DebugTick(yyc::CInstance* localPlayer);

bool Enabled();
// Creates the hooks (enabled by the caller).
bool InstallHooks(bool (*inSession)());
// From the room_goto hook: true if the change of room was taken over (going into or out of an interior).
bool InterceptRoomGoto(yyc::CInstance* self, int room);
// WorldControl's end step has just run (it keeps the camera inside the world room): inside an interior, the camera
// and the culling bounds follow the player.
void AfterWorldEndStep(yyc::CInstance* self);
// The local player is inside an interior built in the world room.
bool IsInside();
// Around a world save while inside (world.cpp): the save sees the player at the door and no interior.
void BeforeSave();
void AfterSave();
// An interior is being put together now (what is created is not news for the other players).
bool IsBuilding();
// `id` is a part of the interior the local player is in.
bool IsInterior(int id);
// The session is over (its room goes anyway).
void Reset();

}  // namespace coop::interiors
