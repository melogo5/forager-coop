// TCP links between the host and its guests (a star: guests talk only to the host). Background threads accept,
// connect and receive; the game thread sends and drains received messages. Frame on the wire:
// [u32 body size][body], body = [u8 type][payload] (bodies up to 8 MB). The sender is not on the wire: the
// receiving side knows it from the connection.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace coop::net {

// A player in the session: 0 is the host; on the host each guest connection gets the next unused id (never
// reused while the host keeps running, so a late message from a guest that left is not taken for a newcomer's).
using PeerId = uint8_t;
constexpr PeerId kHostPeer = 0;

struct Message {
    uint8_t type = 0;
    PeerId from = kHostPeer;  // on a guest always the host
    std::vector<uint8_t> payload;
};

// Host: accepts up to `maxGuests` guests at a time; more are turned away.
void StartHost(uint16_t port, int maxGuests);
void StartJoin(const std::string& host, uint16_t port);
// Drops every link and stops hosting/reconnecting; a later StartHost/StartJoin starts over.
void Stop();

// Any link is up (a guest: to the host; the host: to at least one guest).
bool IsConnected();
// The peers linked right now, in the order they connected.
std::vector<PeerId> Peers();
// Host: to every guest; guest: to the host. False if not connected or a send failed.
bool Send(uint8_t type, const void* payload, size_t size);
// To one peer only; false if it is not connected or the send failed.
bool SendTo(PeerId peer, uint8_t type, const void* payload, size_t size);
// Host: to every guest but `except` (passing on what that guest said).
void SendExcept(PeerId except, uint8_t type, const void* payload, size_t size);
// Moves every message received since the last call into `out`.
void Drain(std::vector<Message>& out);

// Called on the receive thread for every message before it is queued; return true to consume it.
// For work that must not wait for the game loop (e.g. writing files while the game sits in a menu).
using ReceiveHandler = bool (*)(const Message& message);
void SetReceiveHandler(ReceiveHandler handler);

// Called on a network thread right after a link is established (before anything is received from it).
using ConnectHandler = void (*)(PeerId peer);
void SetConnectHandler(ConnectHandler handler);

}  // namespace coop::net
