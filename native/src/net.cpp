#include "net.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <mutex>
#include <thread>

#include "log.h"

#pragma comment(lib, "ws2_32.lib")

namespace coop::net {

namespace {

constexpr int kRetryMs = 1000;
constexpr size_t kMaxBody = 8 * 1024 * 1024;  // a whole late-game save fits; bigger means a broken peer
constexpr size_t kHeaderSize = 4;              // u32 little-endian body size

std::mutex g_lock;  // guards g_socket (for Send) and g_inbox
SOCKET g_socket = INVALID_SOCKET;
std::vector<Message> g_inbox;
bool g_started = false;
ReceiveHandler g_handler = nullptr;  // set before Start*, read only by the receive thread
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

void SetConnected(SOCKET s) {
    const BOOL noDelay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
    {
        std::lock_guard<std::mutex> guard(g_lock);
        g_socket = s;
    }
    if (g_onConnect != nullptr) g_onConnect();
}

void Disconnect(SOCKET s) {
    {
        std::lock_guard<std::mutex> guard(g_lock);
        if (g_socket == s) g_socket = INVALID_SOCKET;
    }
    closesocket(s);
}

// Receives frames until the peer goes away.
void ReceiveLoop(SOCKET s) {
    uint8_t header[kHeaderSize];
    std::vector<uint8_t> body;
    while (RecvExact(s, header, sizeof(header))) {
        const size_t size = header[0] | (header[1] << 8) | (header[2] << 16) | (static_cast<size_t>(header[3]) << 24);
        if (size == 0 || size > kMaxBody) {
            Log("net: bad frame size %zu, dropping connection", size);
            break;
        }
        body.resize(size);
        if (!RecvExact(s, body.data(), size)) break;
        Message message;
        message.type = body[0];
        message.payload.assign(body.begin() + 1, body.end());
        if (g_handler != nullptr && g_handler(message)) continue;
        std::lock_guard<std::mutex> guard(g_lock);
        g_inbox.push_back(std::move(message));
    }
    Log("net: connection closed");
    Disconnect(s);
}

void HostThread(uint16_t port) {
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (listener == INVALID_SOCKET || bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(listener, 1) != 0) {
        Log("net: cannot listen on port %u (error %d)", port, WSAGetLastError());
        return;
    }
    Log("net: hosting on port %u", port);
    for (;;) {
        sockaddr_in peer{};
        int peerSize = sizeof(peer);
        const SOCKET client = accept(listener, reinterpret_cast<sockaddr*>(&peer), &peerSize);
        if (client == INVALID_SOCKET) continue;
        char ip[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        if (IsConnected()) {
            Log("net: rejecting %s, a guest is already connected", ip);
            closesocket(client);
            continue;
        }
        Log("net: guest connected from %s", ip);
        SetConnected(client);
        // One guest at a time: serve it here, then go back to accepting.
        ReceiveLoop(client);
    }
}

void JoinThread(std::string host, uint16_t port) {
    for (;;) {
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
        Log("net: connected to %s:%u", host.c_str(), port);
        SetConnected(s);
        ReceiveLoop(s);
        Sleep(kRetryMs);
    }
}

bool Startup() {
    if (g_started) return false;
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        Log("net: WSAStartup failed");
        return false;
    }
    g_started = true;
    return true;
}

}  // namespace

void StartHost(uint16_t port) {
    if (Startup()) std::thread(HostThread, port).detach();
}

void StartJoin(const std::string& host, uint16_t port) {
    if (Startup()) std::thread(JoinThread, host, port).detach();
}

bool IsConnected() {
    std::lock_guard<std::mutex> guard(g_lock);
    return g_socket != INVALID_SOCKET;
}

bool Send(uint8_t type, const void* payload, size_t size) {
    const size_t body = size + 1;
    if (body > kMaxBody) return false;
    std::vector<uint8_t> frame(kHeaderSize + body);
    for (size_t i = 0; i < kHeaderSize; ++i) frame[i] = static_cast<uint8_t>(body >> (8 * i));
    frame[kHeaderSize] = type;
    if (size > 0) memcpy(frame.data() + kHeaderSize + 1, payload, size);
    std::lock_guard<std::mutex> guard(g_lock);
    if (g_socket == INVALID_SOCKET) return false;
    // A blocking send under the lock; send() may take a large frame in several pieces.
    size_t sent = 0;
    while (sent < frame.size()) {
        const int n = send(g_socket, reinterpret_cast<const char*>(frame.data() + sent),
                           static_cast<int>(frame.size() - sent), 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

void SetReceiveHandler(ReceiveHandler handler) { g_handler = handler; }
void SetConnectHandler(ConnectHandler handler) { g_onConnect = handler; }

void Drain(std::vector<Message>& out) {
    std::lock_guard<std::mutex> guard(g_lock);
    for (auto& message : g_inbox) out.push_back(std::move(message));
    g_inbox.clear();
}

}  // namespace coop::net
