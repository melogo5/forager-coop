// Fish traps are the host's (PLAYTEST3.md #2): only the host's traps catch (HandleFishTraps); what a trap holds
// (caught[0]: an item, -4 when empty) goes to the guests whenever it changes there. Whoever collects a catch first
// gets it - a guest collects on its own copy (its drop, as with crafting) and the host empties its trap without a
// drop - so a catch is collected once, not once per player.
#pragma once

#include "net.h"
#include "yyc.h"

namespace coop::traps {

// Creates the hooks (enabled by the caller).
bool InstallHooks(bool (*isHost)(), bool (*isGuest)());

// Game thread: applies a trap message; false if `message` is not one.
bool Handle(const net::Message& message, yyc::CInstance* localPlayer);

}  // namespace coop::traps
