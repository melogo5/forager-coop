// A treasure chest gives its loot once, to whoever opens it (docs/coop/PLAYTEST.md #11): when a player opens
// one, the other player's copy goes away. (Chests from puzzles could be opened by both: each side kept its own.)
#pragma once

#include "net.h"
#include "yyc.h"

namespace coop::chests {

// Creates the hooks (enabled by the caller).
bool InstallHooks();

// Testing aid: COOP_DEBUG_CHEST=1 opens the nearest treasure chest ~10 s into the world.
void DebugTick(yyc::CInstance* localPlayer);

// Game thread: removes the chest the peer opened; false if `message` is not one.
bool Handle(const net::Message& message, yyc::CInstance* localPlayer);

}  // namespace coop::chests
