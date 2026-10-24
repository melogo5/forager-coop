#include "net.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

#include "log.h"
#include "steam.h"

#pragma comment(lib, "ws2_32.lib")

namespace coop::net {

namespace {

constexpr int kRetryMs = 1000;
constexpr size_t kMaxBody = 8 * 1024 * 1024;  // a whole late-game save fits; bigger means a broken peer
constexpr size_t kHeaderSize = 4;              // u32 little-endian body size
constexpr uint8_t kNetClose = 0;               // a frame of this type: the sender closes the link (Steam P2P has no
                                               // "connection closed" of its own); never handed up
constexpr size_t kP2PChunk = 512 * 1024;       // a reliable Steam P2P packet takes up to 1 MB
constexpr DWORD kSteamIdleMs = 2;              // the Steam receive thread's nap when nothing has arrived
constexpr DWORD kSteamGraceMs = 15000;         // a new Steam link is not checked for this long (being set up)
constexpr uint8_t kNetPing = 0xFF;             // a frame of this type: "still here" (Steam links); never handed up
constexpr DWORD kPingEveryMs = 1000;
// A Steam link silent this long is gone. Steam's own session state is no help: as long as we keep sending, Steam
// keeps "connecting" to a peer that has quit (PLAYTEST2: a guest who left stayed in the game).
constexpr DWORD kSteamSilentMs = 10000;

// One link: a TCP socket, or a Steam account reached through Steam's P2P network. Sends to it are serialized by its
// own lock, so a long send (a world snapshot) to one guest does not hold up the others.
struct Connection {
    SOCKET socket = INVALID_SOCKET;  // guarded by sendLock once shared
    uint64_t steamId = 0;            // a Steam link (then no socket)
    PeerId id = kHostPeer;
    std::mutex sendLock;
    std::atomic<bool> dropped{false};  // Stop() let go of it: whatever still arrives is not queued
    std::vector<uint8_t> pending;      // Steam: bytes of a frame still coming (Steam receive thread only)
    DWORD since = GetTickCount();
    std::atomic<DWORD> heard{GetTickCount()};  // Steam: when the last packet came
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
std::atomic<ReceiveHandler> g_handler{nullptr};  // set by the game thread, read by the receive threads
std::atomic<ConnectHandler> g_onConnect{nullptr};
int g_maxGuests = 0;                          // host
std::atomic<bool> g_steamHosting{false};      // host: Steam guests are taken too
std::atomic<unsigned> g_steamGeneration{0};   // Stop() bumps it: the Steam receive thread ends
std::atomic<bool> g_steamThread{false};       // a Steam receive thread is running

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
    // A peer gone without a word (VPN cut, power off) would otherwise stay linked for good, holding its slot,
    // puppet, stations and storages: probe after 10 s of silence, every 2 s; Windows drops the link after 10 misses.
    tcp_keepalive keepalive{1, 10000, 2000};
    DWORD unused = 0;
    WSAIoctl(s, SIO_KEEPALIVE_VALS, &keepalive, sizeof(keepalive), nullptr, 0, &unused, nullptr, nullptr);
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
    if (connection->steamId != 0) {
        steam::CloseP2P(connection->steamId);
        connection->steamId = 0;
        return;
    }
    closesocket(connection->socket);
    connection->socket = INVALID_SOCKET;
}

// A frame's body from `connection`: to the receive handler or into the inbox.
void Deliver(const ConnectionPtr& connection, const uint8_t* body, size_t size) {
    if (connection->dropped) return;
    Message message;
    message.type = body[0];
    message.from = connection->id;
    message.payload.assign(body + 1, body + size);
    const ReceiveHandler handler = g_handler.load();
    if (handler != nullptr && handler(message)) return;
    std::lock_guard<std::mutex> guard(g_lock);
    g_inbox.push_back(std::move(message));
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
        Deliver(connection, body.data(), size);
    }
    Log("net: connection to peer %u closed", connection->id);
    Unregister(connection);
}

void Connected(SOCKET s, PeerId id) {
    ConnectionPtr connection = Register(s, id);
    if (const ConnectHandler onConnect = g_onConnect.load()) onConnect(id);
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

// A blocking send under the link's lock; send() may take a large frame in several pieces. Over Steam the frame
// goes in packets of up to kP2PChunk, which arrive in order and are joined again (FeedSteam).
bool SendFrame(Connection& connection, const std::vector<uint8_t>& frame) {
    std::lock_guard<std::mutex> guard(connection.sendLock);
    if (connection.steamId != 0) {
        for (size_t at = 0; at < frame.size(); at += kP2PChunk) {
            const size_t size = (std::min)(kP2PChunk, frame.size() - at);
            if (!steam::SendP2P(connection.steamId, frame.data() + at, static_cast<uint32_t>(size))) return false;
        }
        return true;
    }
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

// ---- Steam P2P links ----

ConnectionPtr FindSteam(uint64_t steamId) {
    std::lock_guard<std::mutex> guard(g_lock);
    for (const auto& connection : g_connections) {
        if (connection->steamId == steamId) return connection;
    }
    return nullptr;
}

ConnectionPtr RegisterSteam(uint64_t steamId, PeerId id) {
    auto connection = std::make_shared<Connection>();
    connection->steamId = steamId;
    connection->id = id;
    std::lock_guard<std::mutex> guard(g_lock);
    g_connections.push_back(connection);
    return connection;
}

// Adds a packet's bytes to the link's stream and hands up every complete frame; false when the link is over (the
// peer closed it, or sent garbage).
bool FeedSteam(const ConnectionPtr& connection, const std::vector<uint8_t>& data) {
    std::vector<uint8_t>& pending = connection->pending;
    pending.insert(pending.end(), data.begin(), data.end());
    size_t at = 0;
    while (pending.size() - at >= kHeaderSize) {
        const uint8_t* h = pending.data() + at;
        const size_t size = h[0] | (h[1] << 8) | (h[2] << 16) | (static_cast<size_t>(h[3]) << 24);
        if (size == 0 || size > kMaxBody) {
            Log("net: bad frame size %zu from peer %u", size, connection->id);
            return false;
        }
        if (pending.size() - at < kHeaderSize + size) break;
        const uint8_t* body = h + kHeaderSize;
        if (body[0] == kNetClose) return false;
        if (body[0] != kNetPing) Deliver(connection, body, size);
        at += kHeaderSize + size;
    }
    pending.erase(pending.begin(), pending.begin() + at);
    return true;
}

// Host: a packet from a Steam account with no link yet - a new guest if steam.cpp accepted it (in our lobby).
ConnectionPtr NewSteamGuest(uint64_t steamId) {
    if (!g_steamHosting || !steam::IsAccepted(steamId)) return nullptr;
    PeerId id = kHostPeer;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        if (static_cast<int>(g_connections.size()) < g_maxGuests) id = NextGuestId();
    }
    if (id == kHostPeer) {
        Log("net: rejecting Steam account %llu, the session is full", static_cast<unsigned long long>(steamId));
        steam::CloseP2P(steamId);
        return nullptr;
    }
    Log("net: guest %u connected through Steam (%llu)", id, static_cast<unsigned long long>(steamId));
    return RegisterSteam(steamId, id);
}

// Every second: a ping down each Steam link, and links silent for too long (the peer quit, crashed or lost Steam)
// are forgotten once past their grace period.
void CheckSteamLinks() {
    const std::vector<uint8_t> ping = Frame(kNetPing, nullptr, 0);
    const DWORD now = GetTickCount();
    for (const auto& connection : Connections()) {
        if (connection->steamId == 0) continue;
        if (now - connection->since >= kSteamGraceMs && now - connection->heard > kSteamSilentMs) {
            Log("net: Steam link to peer %u is gone (silent for %lu ms)", connection->id, now - connection->heard);
            Unregister(connection);
            continue;
        }
        SendFrame(*connection, ping);
    }
}

void SteamThread(unsigned generation) {
    std::vector<uint8_t> data;
    DWORD lastCheck = GetTickCount();
    while (g_steamGeneration == generation) {
        bool any = false;
        uint64_t from = 0;
        while (g_steamGeneration == generation && steam::ReadP2P(&data, &from)) {
            any = true;
            ConnectionPtr connection = FindSteam(from);
            if (connection == nullptr) connection = NewSteamGuest(from);
            if (connection == nullptr) continue;
            connection->heard = GetTickCount();
            if (FeedSteam(connection, data)) continue;
            Log("net: Steam link to peer %u closed", connection->id);
            Unregister(connection);
        }
        if (GetTickCount() - lastCheck >= kPingEveryMs) {
            lastCheck = GetTickCount();
            CheckSteamLinks();
        }
        if (!any) Sleep(kSteamIdleMs);
    }
    g_steamThread = false;
}

void EnsureSteamThread() {
    if (g_steamThread.exchange(true)) return;
    std::thread(SteamThread, g_steamGeneration.load()).detach();
}

}  // namespace

void StartHost(uint16_t port, int maxGuests) {
    g_maxGuests = maxGuests;
    if (Startup()) std::thread(HostThread, port, maxGuests, g_hostGeneration.load()).detach();
}

void HostSteam() {
    if (!steam::Ready()) return;
    g_steamHosting = true;
    EnsureSteamThread();
    Log("net: taking guests through Steam too");
}

void StartJoinSteam(uint64_t hostSteamId) {
    if (!steam::Ready()) return;
    EnsureSteamThread();
    Log("net: connecting to Steam account %llu", static_cast<unsigned long long>(hostSteamId));
    RegisterSteam(hostSteamId, kHostPeer);
    if (const ConnectHandler onConnect = g_onConnect.load()) onConnect(kHostPeer);  // its first packet opens the P2P session
}

void StartJoin(const std::string& host, uint16_t port) {
    if (Startup()) std::thread(JoinThread, host, port, g_joinGeneration.load()).detach();
}

void Stop() {
    ++g_joinGeneration;
    ++g_hostGeneration;
    ++g_steamGeneration;
    g_steamHosting = false;
    const std::vector<uint8_t> close = Frame(kNetClose, nullptr, 0);
    std::lock_guard<std::mutex> guard(g_lock);
    for (const auto& connection : g_connections) {
        connection->dropped = true;
        if (connection->steamId != 0) {
            SendFrame(*connection, close);  // Steam says nothing when a link ends: we do
            steam::CloseP2P(connection->steamId);
            continue;
        }
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

void SendExcept(PeerId except, uint8_t type, const void* payload, size_t size) {
    if (size + 1 > kMaxBody) return;
    const std::vector<uint8_t> frame = Frame(type, payload, size);
    for (const auto& connection : Connections()) {
        if (connection->id != except) SendFrame(*connection, frame);
    }
}

void SetReceiveHandler(ReceiveHandler handler) { g_handler = handler; }
void SetConnectHandler(ConnectHandler handler) { g_onConnect = handler; }

void Drain(std::vector<Message>& out) {
    std::lock_guard<std::mutex> guard(g_lock);
    for (auto& message : g_inbox) out.push_back(std::move(message));
    g_inbox.clear();
}

}  // namespace coop::net
