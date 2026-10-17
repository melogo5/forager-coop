// Buying an island: whoever buys it runs SpawnLand as usual and tells the peer, which runs the same
// SpawnLand and removes the purchase sign there. Resources on the new island reach the guest through
// the normal object sync (the guest spawns none of its own).
#pragma once

#include "net.h"
#include "yyc.h"

namespace coop::land {

// Creates the hooks (enabled by the caller). Only the host buys islands; the guest's sale signs are inert.
bool InstallHooks(bool (*isHost)(), bool (*isGuest)());

// Game thread, every local player step (testing aid, see land.cpp).
void DebugTick(yyc::CInstance* localPlayer);

// Game thread: applies a land message; false if `message` is not one.
bool Handle(const net::Message& message, yyc::CInstance* localPlayer);

}  // namespace coop::land
