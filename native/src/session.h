#pragma once

#include <cstdint>
#include <string>

namespace coop {

constexpr uint16_t kGamePort = 7790;

// Creates the session hooks (enabled by the caller). `-coop host|join` launch options still work for testing.
// Returns false only if a hook could not be created.
bool SessionInstall();

namespace session {

enum class Role { kNone, kHost, kGuest };

// Host: accept a guest and announce this game on the LAN once the player is in a world.
void Host();
// Guest: connect to a host; its world arrives in world::kGuestSlot.
void Join(const std::string& address, uint16_t port);
// Guest: the same through Steam, to the host's Steam account (after accepting its invite).
void JoinSteam(uint64_t hostSteamId);
// Back in the main menu after playing: ends the session (the host tells the guest first) and forgets it,
// so Host/Join work again.
void Leave();
// Host, back in the main menu, every step before Leave: tells the guests and waits (a few seconds at most) for
// their last character saves. True when Leave may go.
bool HostFarewellDone();
// The current session has been in a world.
bool PlayedWorld();
// A message for the COOP screen about how the last session ended (empty if none); taken once.
std::string TakeNotice();

Role CurrentRole();
bool IsConnected();

}  // namespace session

}  // namespace coop
