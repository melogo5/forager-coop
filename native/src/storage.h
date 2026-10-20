// Shared chests (objVault): when a player closes a storage menu, and whenever a chest's contents change
// otherwise (crafting takes from vaults), the chest's contents (its 2D `slot` array) go to the peer, which
// overwrites its copy. The last change wins.
#pragma once

#include "net.h"
#include "yyc.h"

namespace coop::storage {

// Creates the storage menu hook (enabled by the caller).
bool InstallHooks();

// Game thread, every local player step: watches the vaults for changes (and a testing aid, see storage.cpp).
void DebugTick(yyc::CInstance* localPlayer);

// Game thread: applies a storage message; false if `message` is not one.
bool Handle(const net::Message& message, yyc::CInstance* localPlayer);

// Forgets which chests the last peer had open.
// Host: a guest that has just joined hears which chests other guests have open.
void OnPeerJoined(net::PeerId peer);
// Host: guest `peer` left; chests it had open are free again (for everyone: their contents go out).
void OnPeerLeft(yyc::CInstance* localPlayer, net::PeerId peer);

void Reset();

}  // namespace coop::storage
