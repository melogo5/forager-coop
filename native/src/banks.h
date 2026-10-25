// Banks are the host's (playtest 6, bug 3): only the host's banks earn (HandleBanks); how many coins a bank holds
// (crafting[1]) goes to the guests whenever it changes there. Collecting works as with fish traps: a guest asks
// the host, which pays it out for that guest - so the coins are collected once, not once per player.
#pragma once

#include "net.h"
#include "yyc.h"

namespace coop::banks {

// Creates the hooks (enabled by the caller).
bool InstallHooks(bool (*isHost)(), bool (*isGuest)());

// Game thread: applies a bank message; false if `message` is not one.
bool Handle(const net::Message& message, yyc::CInstance* localPlayer);

}  // namespace coop::banks
