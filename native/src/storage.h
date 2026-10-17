// Shared chests (objVault): when a player closes a storage menu, the chest's contents (its 2D `slot`
// array) go to the peer, which overwrites its copy. The last player to close a chest wins.
#pragma once

#include "net.h"
#include "yyc.h"

namespace coop::storage {

// Creates the storage menu hook (enabled by the caller).
bool InstallHooks();

// Game thread, every local player step (testing aid, see storage.cpp).
void DebugTick(yyc::CInstance* localPlayer);

// Game thread: applies a storage message; false if `message` is not one.
bool Handle(const net::Message& message, yyc::CInstance* localPlayer);

}  // namespace coop::storage
