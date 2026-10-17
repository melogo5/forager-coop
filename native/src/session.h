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

Role CurrentRole();
bool IsConnected();

}  // namespace session

}  // namespace coop
