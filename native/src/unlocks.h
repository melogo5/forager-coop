// Structures and items (recipes) unlocked by skills count for all players (docs/coop/SPEC.md): whatever one player
// can build or craft, the others can too. Each side sends what it has unlocked (StructureGet(s, 11), ItemGet(i, 12));
// the other unlocks the missing ones with StructureEdit / ItemEdit, as buying a skill does. Nothing is ever locked
// again; the host's world keeps the guest's unlocks.
#pragma once

#include "net.h"
#include "yyc.h"

namespace coop::unlocks {

// Game thread, every local player step while connected (both sides).
void Tick(yyc::CInstance* localPlayer);

// Game thread: applies an unlocks message; false if `message` is not one.
bool Handle(const net::Message& message, yyc::CInstance* localPlayer);

// Forgets what was last sent (the session is over).
void Reset();

}  // namespace coop::unlocks
