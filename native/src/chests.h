// Treasure chests: every player opens each one once and gets its loot (playtest 4, bug 9; it was "whoever opens it
// first" before, docs/coop/PLAYTEST.md #11). A chest the guest opens goes away only at the guest; the host keeps
// a list per guest (saveDataN/coop_guests/<name>.chests) and hands it over on the guest's next visit. A chest the
// host opens goes away only at the host. Their spawns are still the host's, and so is where a new one comes to rest.
#pragma once

#include "net.h"
#include "yyc.h"

namespace coop::chests {

// Creates the hooks (enabled by the caller).
bool InstallHooks(bool (*isHost)());

// objTreasureChest and its kin: each player's own once spawned - no destroys are sent for them, no reconciling.
bool IsTreasureChest(int object);

// Game thread, every local player step while connected: the host sends where new chests came to rest; the guest
// removes chests it has opened before.
void Tick(yyc::CInstance* localPlayer);

// Host: guest `peer` has introduced itself; it hears which chests it opened before.
void HostOnGuest(net::PeerId peer);

// Testing aid: COOP_DEBUG_CHEST=1 opens the nearest treasure chest ~10 s into the world.
void DebugTick(yyc::CInstance* localPlayer);

// Game thread: handles a chest message; false if `message` is not one.
bool Handle(const net::Message& message, yyc::CInstance* localPlayer);

void Reset();

}  // namespace coop::chests
