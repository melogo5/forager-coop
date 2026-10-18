// Structures unlocked by skills count for both players (docs/coop/SPEC.md): whatever the host can build, the
// guest can build too. The host sends the structures it has unlocked (StructureGet(s, 11)); the guest unlocks
// the missing ones with StructureEdit(s, 11, true), as buying a skill does. Nothing is ever locked again.
#pragma once

#include "net.h"
#include "yyc.h"

namespace coop::unlocks {

// Game thread, every local player step while connected (host only).
void HostTick(yyc::CInstance* localPlayer);

// Game thread: applies an unlocks message; false if `message` is not one.
bool Handle(const net::Message& message, yyc::CInstance* localPlayer);

// Forgets what was last sent (the session is over).
void Reset();

}  // namespace coop::unlocks
