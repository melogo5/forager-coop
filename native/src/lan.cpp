#include "lan.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iterator>
#include <map>
#include <mutex>
#include <thread>

#include "log.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

namespace coop::lan {

namespace {

using Clock = std::chrono::steady_clock;

constexpr char kMagic[] = "FCOOP1";
constexpr size_t kMagicSize = sizeof(kMagic) - 1;
constexpr int kAnnounceIntervalMs = 1000;
constexpr auto kServerTimeout = std::chrono::seconds(4);
constexpr size_t kMaxName = 48;

struct Entry {
    Server server;
    Clock::time_point lastSeen;
};

std::mutex g_lock;
std::map<std::string, Entry> g_servers;  // key: address:port

constexpr size_t kMaxServers = 64;

// Under g_lock: forgets hosts not heard for a while.
void PruneStale() {
    const auto now = Clock::now();
    for (auto it = g_servers.begin(); it != g_servers.end();) {
        it = now - it->second.lastSeen > kServerTimeout ? g_servers.erase(it) : std::next(it);
    }
}

bool g_announceActive = false;
std::string g_worldName;
int g_players = 1;
int g_capacity = 2;
uint32_t g_nonce = 0;  // identifies our own announcements
bool g_announcing = false;
bool g_listening = false;

bool InitWinsock() {
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;  // ref-counted, safe to call more than once
}

std::string ComputerName() {
    char name[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD size = sizeof(name);
    return GetComputerNameA(name, &size) ? std::string(name, size) : std::string("Forager");
}

// Broadcast address of every up IPv4 adapter, plus loopback for two games on one PC.
std::vector<in_addr> BroadcastAddresses() {
    std::vector<in_addr> result;
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buffer(size);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &size) == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    }
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &size) == NO_ERROR) {
        for (auto* a = adapters; a != nullptr; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp) continue;
            for (auto* u = a->FirstUnicastAddress; u != nullptr; u = u->Next) {
                const auto* ip = reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr);
                const ULONG prefix = u->OnLinkPrefixLength;
                if (prefix == 0 || prefix >= 32) continue;
                const uint32_t mask = htonl(0xFFFFFFFFu << (32 - prefix));
                in_addr broadcast{};
                broadcast.s_addr = ip->sin_addr.s_addr | ~mask;
                result.push_back(broadcast);
            }
        }
    }
    in_addr loopback{};
    loopback.s_addr = htonl(INADDR_LOOPBACK);
    result.push_back(loopback);
    return result;
}

// The capacity byte goes last: older builds, which have no such byte, still read the rest.
std::vector<uint8_t> BuildPacket(uint16_t port, const std::string& host, const std::string& world, int players,
                                 int capacity) {
    std::vector<uint8_t> p(kMagic, kMagic + kMagicSize);
    p.push_back(static_cast<uint8_t>(port & 0xFF));
    p.push_back(static_cast<uint8_t>(port >> 8));
    p.push_back(static_cast<uint8_t>(players));
    for (const std::string* s : {&host, &world}) {
        const size_t n = (std::min)(s->size(), kMaxName);
        p.push_back(static_cast<uint8_t>(n));
        p.insert(p.end(), s->begin(), s->begin() + n);
    }
    for (int i = 0; i < 4; ++i) p.push_back(static_cast<uint8_t>(g_nonce >> (8 * i)));
    p.push_back(static_cast<uint8_t>(capacity));
    return p;
}

bool ParsePacket(const uint8_t* p, size_t size, Server* out, uint32_t* nonce) {
    if (size < kMagicSize + 3 || memcmp(p, kMagic, kMagicSize) != 0) return false;
    size_t at = kMagicSize;
    out->port = static_cast<uint16_t>(p[at] | (p[at + 1] << 8));
    out->players = p[at + 2];
    at += 3;
    for (std::string* s : {&out->hostName, &out->worldName}) {
        if (at >= size) return false;
        const size_t n = p[at++];
        if (n > kMaxName || at + n > size) return false;
        s->assign(reinterpret_cast<const char*>(p + at), n);
        at += n;
    }
    if (at + 4 != size && at + 5 != size) return false;
    *nonce = p[at] | (p[at + 1] << 8) | (p[at + 2] << 16) | (static_cast<uint32_t>(p[at + 3]) << 24);
    out->capacity = at + 5 == size ? p[at + 4] : 2;  // a build without the byte takes two players
    return out->port != 0;
}

void AnnounceThread(uint16_t port) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    const BOOL on = TRUE;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof(on));
    const std::string host = ComputerName();
    for (;;) {
        std::vector<uint8_t> packet;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            if (g_announceActive) packet = BuildPacket(port, host, g_worldName, g_players, g_capacity);
        }
        if (!packet.empty()) {
            for (const in_addr& target : BroadcastAddresses()) {
                sockaddr_in to{};
                to.sin_family = AF_INET;
                to.sin_port = htons(kDiscoveryPort);
                to.sin_addr = target;
                sendto(s, reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()), 0,
                       reinterpret_cast<sockaddr*>(&to), sizeof(to));
            }
        }
        Sleep(kAnnounceIntervalMs);
    }
}

void ListenThread() {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    const BOOL on = TRUE;
    // Several games on one PC (testing) must all hear the announcements.
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&on), sizeof(on));
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(kDiscoveryPort);
    if (bind(s, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
        Log("lan: cannot listen on UDP %u (error %d)", kDiscoveryPort, WSAGetLastError());
        return;
    }
    uint8_t buffer[512];
    for (;;) {
        sockaddr_in from{};
        int fromSize = sizeof(from);
        const int n = recvfrom(s, reinterpret_cast<char*>(buffer), sizeof(buffer), 0,
                               reinterpret_cast<sockaddr*>(&from), &fromSize);
        if (n <= 0) continue;
        Server server;
        uint32_t nonce = 0;
        if (!ParsePacket(buffer, static_cast<size_t>(n), &server, &nonce) || nonce == g_nonce) continue;
        char ip[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
        server.address = ip;
        // The same host is heard on several adapters (and loopback); keep one entry per host+port.
        const std::string key = server.hostName + "|" + std::to_string(server.port) + "|" + std::to_string(nonce);
        std::lock_guard<std::mutex> guard(g_lock);
        // Only Servers() pruned the list, and only while the COOP screen is open: a flood of announcements (each
        // a new nonce) grew it meanwhile. Stale entries go here too, and past kMaxServers new hosts wait.
        if (g_servers.count(key) == 0 && g_servers.size() >= kMaxServers) {
            PruneStale();
            if (g_servers.size() >= kMaxServers) continue;
        }
        auto& entry = g_servers[key];
        const bool keepAddress = !entry.server.address.empty() && entry.server.address != "127.0.0.1";
        const std::string previous = entry.server.address;
        entry.server = server;
        if (keepAddress && server.address == "127.0.0.1") entry.server.address = previous;
        entry.lastSeen = Clock::now();
    }
}

}  // namespace

void StartAnnouncing(uint16_t gamePort) {
    if (g_announcing || !InitWinsock()) return;
    g_announcing = true;
    if (g_nonce == 0) g_nonce = GetTickCount() ^ (GetCurrentProcessId() << 16);
    std::thread(AnnounceThread, gamePort).detach();
}

void SetAnnouncement(bool active, const std::string& worldName, int players, int capacity) {
    std::lock_guard<std::mutex> guard(g_lock);
    g_announceActive = active;
    g_worldName = worldName;
    g_players = players;
    g_capacity = capacity;
}

void StartListening() {
    if (g_listening || !InitWinsock()) return;
    g_listening = true;
    if (g_nonce == 0) g_nonce = GetTickCount() ^ (GetCurrentProcessId() << 16);
    std::thread(ListenThread).detach();
}

std::vector<Server> Servers() {
    std::vector<Server> result;
    std::lock_guard<std::mutex> guard(g_lock);
    PruneStale();
    for (const auto& [key, entry] : g_servers) result.push_back(entry.server);
    std::sort(result.begin(), result.end(),
              [](const Server& a, const Server& b) { return a.hostName < b.hostName; });
    return result;
}

}  // namespace coop::lan
