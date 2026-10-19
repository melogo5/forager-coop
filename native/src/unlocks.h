// Structures unlocked by skills count for both players (docs/coop/SPEC.md): whatever one player can build, the
// other can build too, although each has its own skills (skills.h). Each side sends the structures it has
// unlocked (StructureGet(s, 11)); the other unlocks the missing ones with StructureEdit(s, 11, true), as buying
// a skill does. Nothing is ever locked again; the host's world keeps the guest's unlocks.
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
