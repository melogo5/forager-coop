// A guest's own familiars (droids it crafted, its lantern or halo): at the guest they follow the guest itself, not
// the host's puppet as the familiars that come with the host's world do; the host keeps a copy of each next to the
// guest's puppet so it sees them too (PLAYTEST8.md #16). The copies do no work: they are not in the host's
// global.droids, which HandleDroids walks.
#pragma once

#include "net.h"
#include "yyc.h"

namespace coop::familiars {

// Guest, from the instance_create hook: `createdId` (object index `object`) was just made.
void OnCreated(yyc::CInstance* self, int createdId, int object);

// Guest, game thread, every local player step in the world: tells the host about its own familiars.
void GuestTick();

// Host, game thread, every local player step: drops copies whose guest is gone.
void HostTick(yyc::CInstance* localPlayer);

// Game thread: applies a familiars message; false if `message` is not one.
bool Handle(const net::Message& message, yyc::CInstance* localPlayer);

// objDroid draws global.droidNames[droidId] (its look, a sprite): false when that entry is missing here.
bool HasDroidLook(yyc::CInstance* self, int droidId);

// Forgets the last session.
void Reset();

}  // namespace coop::familiars
