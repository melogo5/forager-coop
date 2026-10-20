#include "net.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

#include "log.h"

#pragma comment(lib, "ws2_32.lib")

namespace coop::net {

namespace {

constexpr int kRetryMs = 1000;
constexpr size_t kMaxBody = 8 * 1024 * 1024;  // a whole late-game save fits; bigger means a broken peer
constexpr size_t kHeaderSize = 4;              // u32 little-endian body size

// One link. Sends to it are serialized by its own lock, so a long send (a world snapshot) to one guest does not
// hold up the others.
struct Connection {
    SOCKET socket = INVALID_SOCKET;  // guarded by sendLock once shared
    PeerId id = kHostPeer;
    std::mutex sendLock;
    std::atomic<bool> dropped{false};  // Stop() let go of it: whatever still arrives is not queued
};
using ConnectionPtr = std::shared_ptr<Connection>;

std::mutex g_lock;                         // guards g_connections, g_listener, g_inbox, g_nextGuestId
std::vector<ConnectionPtr> g_connections;  // in the order they connected
std::atomic<unsigned> g_joinGeneration{0};  // Stop() bumps it: older join threads give up
std::atomic<unsigned> g_hostGeneration{0};  // ... and older host threads
SOCKET g_listener = INVALID_SOCKET;
std::vector<Message> g_inbox;
PeerId g_nextGuestId = 1;
bool g_started = false;
ReceiveHandler g_handler = nullptr;  // set before Start*, read only by the receive threads
ConnectHandler g_onConnect = nullptr;

bool RecvExact(SOCKET s, uint8_t* buffer, size_t size) {
    size_t done = 0;
    while (done < size) {
        const int n = recv(s, reinterpret_cast<char*>(buffer + done), static_cast<int>(size - done), 0);
        if (n <= 0) return false;
        done += static_cast<size_t>(n);
    }
    return true;
}

bool IdInUse(PeerId id) {  // under g_lock
    return std::any_of(g_connections.begin(), g_connections.end(), [id](const ConnectionPtr& c) { return c->id == id; });
}

// Under g_lock: the next guest id, skipping the host's and any still in use.
PeerId NextGuestId() {
    while (g_nextGuestId == kHostPeer || IdInUse(g_nextGuestId)) ++g_nextGuestId;
    return g_nextGuestId++;
}

ConnectionPtr Register(SOCKET s, PeerId id) {
    const BOOL noDelay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
    auto connection = std::make_shared<Connection>();
    connection->socket = s;
    connection->id = id;
    std::lock_guard<std::mutex> guard(g_lock);
    g_connections.push_back(connection);
    return connection;
}

void Unregister(const ConnectionPtr& connection) {
    {
        std::lock_guard<std::mutex> guard(g_lock);
        g_connections.erase(std::remove(g_connections.begin(), g_connections.end(), connection), g_connections.end());
    }
    std::lock_guard<std::mutex> guard(connection->sendLock);
    closesocket(connection->socket);
    connection->socket = INVALID_SOCKET;
}

// Receives frames until the peer goes away, then forgets the link.
void ReceiveLoop(ConnectionPtr connection) {
    const SOCKET s = connection->socket;  // only Unregister, below, changes it
    uint8_t header[kHeaderSize];
    std::vector<uint8_t> body;
    while (RecvExact(s, header, sizeof(header))) {
        const size_t size = header[0] | (header[1] << 8) | (header[2] << 16) | (static_cast<size_t>(header[3]) << 24);
        if (size == 0 || size > kMaxBody) {
            Log("net: bad frame size %zu from peer %u, dropping connection", size, connection->id);
            break;
        }
        body.resize(size);
        if (!RecvExact(s, body.data(), size)) break;
        if (connection->dropped) continue;
        Message message;
        message.type = body[0];
        message.from = connection->id;
        message.payload.assign(body.begin() + 1, body.end());
        if (g_handler != nullptr && g_handler(message)) continue;
        std::lock_guard<std::mutex> guard(g_lock);
        g_inbox.push_back(std::move(message));
    }
    Log("net: connection to peer %u closed", connection->id);
    Unregister(connection);
}

void Connected(SOCKET s, PeerId id) {
    ConnectionPtr connection = Register(s, id);
    if (g_onConnect != nullptr) g_onConnect(id);
    ReceiveLoop(std::move(connection));
}

void HostThread(uint16_t port, int maxGuests, unsigned generation) {
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (listener == INVALID_SOCKET || bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(listener, SOMAXCONN) != 0) {
        Log("net: cannot listen on port %u (error %d)", port, WSAGetLastError());
        return;
    }
    {
        std::lock_guard<std::mutex> guard(g_lock);
        if (g_hostGeneration != generation) {  // stopped meanwhile
            closesocket(listener);
            return;
        }
        g_listener = listener;
    }
    Log("net: hosting on port %u for up to %d guests", port, maxGuests);
    while (g_hostGeneration == generation) {
        sockaddr_in peer{};
        int peerSize = sizeof(peer);
        const SOCKET client = accept(listener, reinterpret_cast<sockaddr*>(&peer), &peerSize);
        if (client == INVALID_SOCKET) continue;  // Stop() closes the listener: the loop then ends
        char ip[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        PeerId id = kHostPeer;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            if (static_cast<int>(g_connections.size()) < maxGuests) id = NextGuestId();
        }
        if (id == kHostPeer) {
            Log("net: rejecting %s, the session is full (%d guests)", ip, maxGuests);
            closesocket(client);
            continue;
        }
        Log("net: guest %u connected from %s", id, ip);
        std::thread(Connected, client, id).detach();
    }
    Log("net: stopped hosting");
}

void JoinThread(std::string host, uint16_t port, unsigned generation) {
    while (g_joinGeneration == generation) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* result = nullptr;
        const std::string service = std::to_string(port);
        SOCKET s = INVALID_SOCKET;
        if (getaddrinfo(host.c_str(), service.c_str(), &hints, &result) == 0 && result != nullptr) {
            s = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
            if (s != INVALID_SOCKET && connect(s, result->ai_addr, static_cast<int>(result->ai_addrlen)) != 0) {
                closesocket(s);
                s = INVALID_SOCKET;
            }
            freeaddrinfo(result);
        }
        if (s == INVALID_SOCKET) {
            Sleep(kRetryMs);
            continue;
        }
        if (g_joinGeneration != generation) {
            closesocket(s);
            break;
        }
        Log("net: connected to %s:%u", host.c_str(), port);
        Connected(s, kHostPeer);
        Sleep(kRetryMs);
    }
}

bool Startup() {
    if (g_started) return true;
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        Log("net: WSAStartup failed");
        return false;
    }
    g_started = true;
    return true;
}

std::vector<uint8_t> Frame(uint8_t type, const void* payload, size_t size) {
    const size_t body = size + 1;
    std::vector<uint8_t> frame(kHeaderSize + body);
    for (size_t i = 0; i < kHeaderSize; ++i) frame[i] = static_cast<uint8_t>(body >> (8 * i));
    frame[kHeaderSize] = type;
    if (size > 0) memcpy(frame.data() + kHeaderSize + 1, payload, size);
    return frame;
}

// A blocking send under the link's lock; send() may take a large frame in several pieces.
bool SendFrame(Connection& connection, const std::vector<uint8_t>& frame) {
    std::lock_guard<std::mutex> guard(connection.sendLock);
    if (connection.socket == INVALID_SOCKET) return false;
    size_t sent = 0;
    while (sent < frame.size()) {
        const int n = send(connection.socket, reinterpret_cast<const char*>(frame.data() + sent),
                           static_cast<int>(frame.size() - sent), 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

std::vector<ConnectionPtr> Connections() {
    std::lock_guard<std::mutex> guard(g_lock);
    return g_connections;
}

}  // namespace

void StartHost(uint16_t port, int maxGuests) {
    if (Startup()) std::thread(HostThread, port, maxGuests, g_hostGeneration.load()).detach();
}

void StartJoin(const std::string& host, uint16_t port) {
    if (Startup()) std::thread(JoinThread, host, port, g_joinGeneration.load()).detach();
}

void Stop() {
    ++g_joinGeneration;
    ++g_hostGeneration;
    std::lock_guard<std::mutex> guard(g_lock);
    for (const auto& connection : g_connections) {
        connection->dropped = true;
        shutdown(connection->socket, SD_BOTH);  // its receive loop then closes it
    }
    g_connections.clear();
    if (g_listener != INVALID_SOCKET) {
        closesocket(g_listener);  // wakes the host thread up from accept()
        g_listener = INVALID_SOCKET;
    }
    g_inbox.clear();
}

bool IsConnected() {
    std::lock_guard<std::mutex> guard(g_lock);
    return !g_connections.empty();
}

std::vector<PeerId> Peers() {
    std::lock_guard<std::mutex> guard(g_lock);
    std::vector<PeerId> peers;
    for (const auto& connection : g_connections) peers.push_back(connection->id);
    return peers;
}

bool Send(uint8_t type, const void* payload, size_t size) {
    if (size + 1 > kMaxBody) return false;
    const std::vector<ConnectionPtr> connections = Connections();
    if (connections.empty()) return false;
    const std::vector<uint8_t> frame = Frame(type, payload, size);
    bool ok = true;
    for (const auto& connection : connections) ok = SendFrame(*connection, frame) && ok;
    return ok;
}

bool SendTo(PeerId peer, uint8_t type, const void* payload, size_t size) {
    if (size + 1 > kMaxBody) return false;
    for (const auto& connection : Connections()) {
        if (connection->id == peer) return SendFrame(*connection, Frame(type, payload, size));
    }
    return false;
}

void SetReceiveHandler(ReceiveHandler handler) { g_handler = handler; }
void SetConnectHandler(ConnectHandler handler) { g_onConnect = handler; }

void Drain(std::vector<Message>& out) {
    std::lock_guard<std::mutex> guard(g_lock);
    for (auto& message : g_inbox) out.push_back(std::move(message));
    g_inbox.clear();
}

}  // namespace coop::net
