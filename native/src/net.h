// TCP link between host and guest. A background thread accepts/connects and receives; the game thread sends
// and drains received messages. Frame on the wire: [u32 body size][body], body = [u8 type][payload]
// (bodies up to 8 MB).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace coop::net {

struct Message {
    uint8_t type = 0;
    std::vector<uint8_t> payload;
};

void StartHost(uint16_t port);
void StartJoin(const std::string& host, uint16_t port);
// Drops the link and stops hosting/reconnecting; a later StartHost/StartJoin starts over.
void Stop();

bool IsConnected();
// Sends one message; drops it (returns false) when not connected.
bool Send(uint8_t type, const void* payload, size_t size);
// Moves every message received since the last call into `out`.
void Drain(std::vector<Message>& out);

// Called on the receive thread for every message before it is queued; return true to consume it.
// For work that must not wait for the game loop (e.g. writing files while the game sits in a menu).
using ReceiveHandler = bool (*)(const Message& message);
void SetReceiveHandler(ReceiveHandler handler);

// Called on the network thread right after a connection is established (before anything is received).
using ConnectHandler = void (*)();
void SetConnectHandler(ConnectHandler handler);

}  // namespace coop::net
