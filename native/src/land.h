// Buying an island: the host runs SpawnLand as usual and tells the guest, which runs the same SpawnLand
// (same seed) and removes the purchase sign there. A guest's purchase (paid from the guest's coins) is sent
// to the host, which builds it the same way. Resources on the new island reach the guest through the
// normal object sync (the guest spawns none of its own).
#pragma once

#include "net.h"
#include "yyc.h"

namespace coop::land {

// Creates the hooks (enabled by the caller).
bool InstallHooks(bool (*isHost)(), bool (*isGuest)());

// Game thread, every local player step (testing aid, see land.cpp).
void DebugTick(yyc::CInstance* localPlayer);

// Game thread: applies a land message; false if `message` is not one.
bool Handle(const net::Message& message, yyc::CInstance* localPlayer);

}  // namespace coop::land
