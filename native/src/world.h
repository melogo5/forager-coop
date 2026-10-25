// Host world snapshot transfer. The host saves its game and streams the slot's files; the guest stores them
// in its own save slot (marked with coop_world.txt and reused on every join), ready to be loaded from the menu.
#pragma once

#include <string>

#include "net.h"
#include "yyc.h"

namespace coop::world {

// Host, game thread: save the game now and send the slot to guest `peer` once the async save has finished.
void HostRequestSnapshot(yyc::CInstance* localPlayer, net::PeerId peer);
// Host, game thread: call every step.
void HostTick();

// Guest, receive thread: consumes world transfer messages (net::SetReceiveHandler).
bool HandleMessage(const net::Message& message);

// Guest: the slot the host world is written to, and whether a complete world has arrived.
constexpr int kGuestSlot = 99;
bool GuestWorldReady();

// Game thread: an instance was destroyed - the herb list may hold a gone id now (cleaned before the next HerbProcess).
void OnInstanceDestroyed();
// Guest: saves the game into its working slot (99) to capture its character; async like SaveGame.
// Then read GuestSlotPath()/Data.cin. The slot stays while in the world (rooms reload from it) and is
// removed with RemoveGuestSaveSlot back in the main menu.
void GuestSaveCharacter(yyc::CInstance* localPlayer);
std::wstring GuestSlotPath();
void RemoveGuestSaveSlot();

// Creates the SaveGame hook (enabled by the caller): a guest never saves, progress lives on the host.
// Also clears a temporary slot left behind by a crash.
bool InstallHooks(bool (*isGuest)());

// Guest: forgets the last received world, for joining again.
void Reset();

}  // namespace coop::world
