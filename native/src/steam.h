// Steam, through the game's own steam_api.dll (the runner has already initialised it and runs its callbacks every
// frame): a friends-only lobby per hosted session, the overlay's invite dialog, joining by invite, and the P2P
// packets the network layer sends Steam guests' traffic with (docs/coop/STEAM.md). Without Steam all of it just
// reports "not available" and the coop works over LAN/IP as before.
#pragma once

#include <cstdint>
#include <vector>

#include "yyc.h"

namespace coop::steam {

// Game thread, at the main menu: connects to the Steam interfaces once and registers the callbacks.
void Init(const char* commandLineLobby);
bool Ready();

// Host: opens a friends-only lobby for the session (friends see "Join game"; invites point at it), and leaves it.
void HostLobby(int maxPlayers);
void LeaveLobby();
bool HasLobby();
// Host: the overlay's "invite friends" dialog for the lobby.
void OpenInviteDialog();

// Guest: the host to connect to after accepting an invite (or starting with +connect_lobby); 0 if none. Taken once.
uint64_t TakeJoinRequest();

// P2P packets (thread-safe; reliable, in order). The network layer frames and reassembles them.
bool SendP2P(uint64_t peer, const void* data, uint32_t size);
// The next packet that has arrived from anyone: false if none.
bool ReadP2P(std::vector<uint8_t>* data, uint64_t* from);
void CloseP2P(uint64_t peer);
// The P2P session with `peer` is up or still being set up.
bool P2PAlive(uint64_t peer);
// Host: Steam accounts whose P2P requests were accepted (members of our lobby). Guest: the host.
bool IsAccepted(uint64_t peer);

// Testing aid: COOP_DEBUG_STEAM=1 at the main menu creates a lobby and opens the invite dialog, as hosting would.
void DebugTick();

}  // namespace coop::steam
