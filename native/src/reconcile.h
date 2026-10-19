// The guest's surroundings are checked against the host's world every few seconds (docs/coop/PLAYTEST.md #3,
// #4): single spawn/destroy messages can be missed (the guest was in the skill tree, an object was asleep...),
// and then a rock broken by one player stays for the other. The host lists the world objects (rocks, trees,
// plants, structures, bridges) in an area around the guest; the guest removes what the host does not have and
// adds what it lacks. Only differences seen in two checks in a row are acted on: a build or a destroy may
// still be on its way.
#pragma once

#include "net.h"
#include "yyc.h"

namespace coop::reconcile {

// Host, game thread, every local player step while connected.
void HostTick(yyc::CInstance* localPlayer);

// Guest, game thread: compares with the host's list; false if `message` is not one.
bool Handle(const net::Message& message, yyc::CInstance* localPlayer);

// Testing aid (both sides): COOP_DEBUG_DESYNC=1 quietly removes the 3 world objects nearest to the guest
// ~8 s in, so the next checks have something to repair (gone at the host: removed at the guest too; gone at
// the guest: added back).
void DebugTick(yyc::CInstance* localPlayer);

// Forgets earlier checks (the session is over).
void Reset();

}  // namespace coop::reconcile
