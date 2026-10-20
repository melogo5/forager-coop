// LAN server discovery over UDP broadcast (works across LAN, Hamachi and Radmin virtual networks).
// A hosting game announces itself once a second on every IPv4 adapter's broadcast address;
// the Coop menu listens and lists what it hears.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace coop::lan {

constexpr uint16_t kDiscoveryPort = 7791;

struct Server {
    std::string address;  // where the announcement came from
    uint16_t port = 0;    // game (TCP) port
    std::string hostName;
    std::string worldName;
    int players = 1;
    int capacity = 2;  // players the session takes
};

// Host side. Announcements go out only while active (the host is in its world).
void StartAnnouncing(uint16_t gamePort);
void SetAnnouncement(bool active, const std::string& worldName, int players, int capacity);

// Menu side.
void StartListening();
std::vector<Server> Servers();  // heard within the last few seconds, sorted by host name

}  // namespace coop::lan
