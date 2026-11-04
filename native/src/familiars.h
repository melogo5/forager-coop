// Every player's own familiars (droids, the lantern, the halo) are seen by all: each player keeps copies of the
// others' next to their puppets (PLAYTEST8.md #16, PLAYTEST10.md #11). A guest tells the host its own; the host passes
// them and its own on to every guest. At a guest the host's familiars that came with the world are its own already:
// only those made later are copied. The copies do no work: they are not in global.droids, which HandleDroids walks.
// A guest's own familiars follow the guest itself, not the host's puppet as the world's familiars do.
#pragma once

#include <string>

#include "net.h"
#include "yyc.h"

namespace coop::familiars {

// Guest, from the instance_create hook: `createdId` (object index `object`) was just made.
void OnCreated(yyc::CInstance* self, int createdId, int object);

// Guest, game thread, every local player step in the world: tells the host about its own familiars, keeps the
// copies of the others' (and makes again the droids it had in the last game, once the host sends them).
void GuestTick(yyc::CInstance* localPlayer);

// Host, game thread: guest `name` (`peer`) has introduced itself - it gets back the droids it had.
void HostOnGuest(const std::string& name, net::PeerId peer);

// Host, game thread, every local player step: tells the guests about its own familiars, keeps the copies.
void HostTick(yyc::CInstance* localPlayer);

// Game thread: player `peer` left - its copies go (the host tells the other guests).
void OnPeerLeft(yyc::CInstance* localPlayer, net::PeerId peer, bool host);

// Game thread: applies a familiars message; false if `message` is not one.
bool Handle(const net::Message& message);

// objDroid draws global.droidNames[droidId] (its look, a sprite): false when that entry is missing here.
bool HasDroidLook(yyc::CInstance* self, int droidId);

// Forgets the last session.
void Reset();

}  // namespace coop::familiars
