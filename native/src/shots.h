// A player's arrows (objArrow), the nuclear sword's fireballs (objFireball) and the ice rod's shots (objIceBolt) are
// no world objects: they were seen by the shooter only. Each shot goes to the other players, who show a harmless copy of it - it flies the same way,
// but hits nothing and drops nothing (the shooter's own shot does the damage, as before).
#pragma once

#include "net.h"
#include "yyc.h"

namespace coop::shots {

// Creates the hooks (enabled by the caller).
bool InstallHooks();

// From the instance_create hook: `createdId` (an `object`) was just made by `self`.
void OnCreated(yyc::CInstance* self, int createdId, int object);

// Game thread, every local player step while connected: sends the shots made since the last step (by now they
// fly: their direction and speed are set).
void Tick();

// Game thread: applies a shot message; false if `message` is not one.
bool Handle(const net::Message& message, yyc::CInstance* localPlayer);

// The session is over: forget the shots.
void Reset();

}  // namespace coop::shots
