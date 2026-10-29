#include "steam.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <set>
#include <string>

#include "log.h"

namespace coop::steam {

namespace {

// steam_api.dll's flat API (Steamworks SDK ~1.41: SteamClient017, SteamUser019). All __cdecl; the first argument
// of an interface function is the interface. A CSteamID travels as a uint64 both ways (checked in the DLL's code:
// the flat functions return it in EDX:EAX and take it as 8 bytes on the stack).
using SteamClientFn = intptr_t(__cdecl*)();
using HandleFn = int32_t(__cdecl*)();
using GetInterfaceFn = intptr_t(__cdecl*)(intptr_t client, int32_t user, int32_t pipe, const char* version);
using SteamIdFn = uint64_t(__cdecl*)(intptr_t self);
using PersonaNameFn = const char*(__cdecl*)(intptr_t friends);
using CreateLobbyFn = uint64_t(__cdecl*)(intptr_t matchmaking, int type, int maxMembers);
using JoinLobbyFn = uint64_t(__cdecl*)(intptr_t matchmaking, uint64_t lobby);
using LeaveLobbyFn = void(__cdecl*)(intptr_t matchmaking, uint64_t lobby);
using LobbyOwnerFn = uint64_t(__cdecl*)(intptr_t matchmaking, uint64_t lobby);
using LobbyMembersFn = int(__cdecl*)(intptr_t matchmaking, uint64_t lobby);
using LobbyMemberFn = uint64_t(__cdecl*)(intptr_t matchmaking, uint64_t lobby, int index);
using SetLobbyDataFn = bool(__cdecl*)(intptr_t matchmaking, uint64_t lobby, const char* key, const char* value);
using InviteDialogFn = void(__cdecl*)(intptr_t friends, uint64_t lobby);
using SetRichPresenceFn = bool(__cdecl*)(intptr_t friends, const char* key, const char* value);
using ClearRichPresenceFn = void(__cdecl*)(intptr_t friends);
using SendP2PFn = bool(__cdecl*)(intptr_t networking, uint64_t peer, const void* data, uint32_t size, int sendType,
                                 int channel);
using P2PAvailableFn = bool(__cdecl*)(intptr_t networking, uint32_t* size, int channel);
using ReadP2PFn = bool(__cdecl*)(intptr_t networking, void* data, uint32_t capacity, uint32_t* size, uint64_t* from,
                                 int channel);
using PeerFn = bool(__cdecl*)(intptr_t networking, uint64_t peer);
using AllowRelayFn = bool(__cdecl*)(intptr_t networking, bool allow);
using RegisterCallbackFn = void(__cdecl*)(void* callback, int id);
using RegisterCallResultFn = void(__cdecl*)(void* callback, uint64_t call);

constexpr char kFriendsVersion[] = "SteamFriends015";
constexpr char kMatchmakingVersion[] = "SteamMatchMaking009";
constexpr char kNetworkingVersion[] = "SteamNetworking005";
constexpr char kUserVersion[] = "SteamUser019";
constexpr int kLobbyFriendsOnly = 1;     // k_ELobbyTypeFriendsOnly
constexpr int kResultOk = 1;             // k_EResultOK
constexpr uint32_t kEnterSuccess = 1;    // k_EChatRoomEnterResponseSuccess
constexpr int kSendReliable = 2;         // k_EP2PSendReliable
constexpr int kChannel = 0;
constexpr char kConnectPrefix[] = "+connect_lobby";

// Callbacks: ids (k_i...Callbacks + n) and their structs, laid out as steam_api writes them.
constexpr int kGameLobbyJoinRequested = 333;          // a friend's invite accepted in the overlay
constexpr int kGameRichPresenceJoinRequested = 337;   // "Join game" on a friend whose presence has `connect`
constexpr int kLobbyEnter = 504;
constexpr int kLobbyCreated = 513;
constexpr int kP2PSessionRequest = 1202;
constexpr int kP2PSessionConnectFail = 1203;
#pragma pack(push, 8)
struct GameLobbyJoinRequested {
    uint64_t lobby;
    uint64_t friendId;
};
struct GameRichPresenceJoinRequested {
    uint64_t friendId;
    char connect[256];
};
struct LobbyEnter {
    uint64_t lobby;
    uint32_t chatPermissions;
    bool locked;
    uint32_t response;
};
struct LobbyCreated {
    int32_t result;
    uint64_t lobby;
};
struct P2PSessionRequest {
    uint64_t remote;
};
struct P2PSessionConnectFail {
    uint64_t remote;
    uint8_t error;
};
#pragma pack(pop)

struct Api {
    intptr_t friends = 0, matchmaking = 0, networking = 0, user = 0;
    SteamIdFn steamId = nullptr;
    PersonaNameFn personaName = nullptr;
    CreateLobbyFn createLobby = nullptr;
    JoinLobbyFn joinLobby = nullptr;
    LeaveLobbyFn leaveLobby = nullptr;
    LobbyOwnerFn lobbyOwner = nullptr;
    LobbyMembersFn lobbyMembers = nullptr;
    LobbyMemberFn lobbyMember = nullptr;
    SetLobbyDataFn setLobbyData = nullptr;
    InviteDialogFn inviteDialog = nullptr;
    SetRichPresenceFn setRichPresence = nullptr;
    ClearRichPresenceFn clearRichPresence = nullptr;
    SendP2PFn sendP2P = nullptr;
    P2PAvailableFn p2pAvailable = nullptr;
    ReadP2PFn readP2P = nullptr;
    PeerFn acceptP2P = nullptr;
    PeerFn closeP2P = nullptr;
    AllowRelayFn allowRelay = nullptr;
    RegisterCallbackFn registerCallback = nullptr;
    RegisterCallResultFn registerCallResult = nullptr;
};
Api g_api;
std::atomic<bool> g_ready{false};
uint64_t g_self = 0;

// Game thread.
uint64_t g_lobby = 0;         // host: our lobby
bool g_creatingLobby = false;
uint64_t g_joiningLobby = 0;  // guest: the lobby being entered
uint64_t g_joinHost = 0;      // guest: the host to connect to, for the menu to take

std::mutex g_acceptedLock;
std::set<uint64_t> g_accepted;  // P2P peers we talk to

// steam_api's callback object (CCallbackBase in the SDK): the same virtual functions in the same order, so MSVC
// lays out the same table; steam_api fills in the flags and the callback id.
class CallbackBase {
public:
    virtual void Run(void* param) = 0;
    virtual void Run(void* param, bool ioFailure, uint64_t call) = 0;
    virtual int GetCallbackSizeBytes() = 0;
    int Id() const { return callback_; }

protected:
    uint8_t flags_ = 0;
    int callback_ = 0;
};

// A callback (or a call result) that hands its struct to `handler`.
template <typename T>
class Callback : public CallbackBase {
public:
    using Handler = void (*)(const T&);
    Callback(int id, Handler handler) : handler_(handler) { callback_ = id; }
    void Run(void* param) override { handler_(*static_cast<const T*>(param)); }
    void Run(void* param, bool ioFailure, uint64_t) override {
        if (!ioFailure) handler_(*static_cast<const T*>(param));
    }
    int GetCallbackSizeBytes() override { return sizeof(T); }

private:
    Handler handler_;
};

template <typename Fn>
Fn Export(HMODULE module, const char* name, bool* ok) {
    const Fn fn = reinterpret_cast<Fn>(GetProcAddress(module, name));
    if (fn == nullptr) {
        Log("steam: steam_api.dll has no %s", name);
        *ok = false;
    }
    return fn;
}

void Accept(uint64_t peer) {
    std::lock_guard<std::mutex> guard(g_acceptedLock);
    g_accepted.insert(peer);
}

bool InOurLobby(uint64_t user) {
    if (g_lobby == 0) return false;
    const int count = g_api.lobbyMembers(g_api.matchmaking, g_lobby);
    for (int i = 0; i < count; ++i) {
        if (g_api.lobbyMember(g_api.matchmaking, g_lobby, i) == user) return true;
    }
    return false;
}

// "+connect_lobby <id>" (Steam's launch option for a lobby invite, also our rich presence `connect`) -> the id.
uint64_t ParseConnect(const char* text) {
    const char* at = text != nullptr ? strstr(text, kConnectPrefix) : nullptr;
    return at != nullptr ? _strtoui64(at + sizeof(kConnectPrefix) - 1, nullptr, 10) : 0;
}

void JoinLobby(uint64_t lobby) {
    if (lobby == 0 || lobby == g_joiningLobby) return;
    g_joiningLobby = lobby;
    g_api.joinLobby(g_api.matchmaking, lobby);
    Log("steam: joining lobby %llu", static_cast<unsigned long long>(lobby));
}

void OnLobbyCreated(const LobbyCreated& created) {
    if (!g_creatingLobby) return;  // both the callback and the call result arrive
    g_creatingLobby = false;
    if (created.result != kResultOk) {
        Log("steam: creating the lobby failed (result %d)", created.result);
        return;
    }
    g_lobby = created.lobby;
    char connect[64];
    snprintf(connect, sizeof(connect), "%s %llu", kConnectPrefix, static_cast<unsigned long long>(g_lobby));
    g_api.setLobbyData(g_api.matchmaking, g_lobby, "forager_coop", "1");
    g_api.setRichPresence(g_api.friends, "connect", connect);
    g_api.setRichPresence(g_api.friends, "status", "Forager coop");
    Log("steam: hosting in lobby %llu (friends can join; F2 invites)", static_cast<unsigned long long>(g_lobby));
}

void OnLobbyEnter(const LobbyEnter& enter) {
    if (enter.lobby != g_joiningLobby) return;  // e.g. the host entering its own new lobby
    g_joiningLobby = 0;
    if (enter.response != kEnterSuccess) {
        Log("steam: could not enter lobby %llu (response %u)", static_cast<unsigned long long>(enter.lobby),
            enter.response);
        return;
    }
    const uint64_t host = g_api.lobbyOwner(g_api.matchmaking, enter.lobby);
    Log("steam: in lobby %llu, host %llu", static_cast<unsigned long long>(enter.lobby),
        static_cast<unsigned long long>(host));
    if (host == 0 || host == g_self) return;
    g_lobby = enter.lobby;
    Accept(host);
    g_joinHost = host;
}

void OnGameLobbyJoinRequested(const GameLobbyJoinRequested& request) {
    Log("steam: invite accepted - lobby %llu", static_cast<unsigned long long>(request.lobby));
    JoinLobby(request.lobby);
}

void OnRichPresenceJoinRequested(const GameRichPresenceJoinRequested& request) {
    const uint64_t lobby = ParseConnect(request.connect);
    Log("steam: 'join game' on a friend - lobby %llu", static_cast<unsigned long long>(lobby));
    JoinLobby(lobby);
}

// Host: a guest's first packet asks for a session; only members of our lobby get one.
void OnP2PSessionRequest(const P2PSessionRequest& request) {
    const bool member = InOurLobby(request.remote);
    Log("steam: P2P request from %llu - %s", static_cast<unsigned long long>(request.remote),
        member ? "accepted (in our lobby)" : "ignored (not in our lobby)");
    if (!member) return;
    Accept(request.remote);
    g_api.acceptP2P(g_api.networking, request.remote);
}

void OnP2PSessionConnectFail(const P2PSessionConnectFail& fail) {
    Log("steam: P2P session with %llu failed (error %u)", static_cast<unsigned long long>(fail.remote), fail.error);
}

Callback<GameLobbyJoinRequested> g_onInvite(kGameLobbyJoinRequested, &OnGameLobbyJoinRequested);
Callback<GameRichPresenceJoinRequested> g_onJoinGame(kGameRichPresenceJoinRequested, &OnRichPresenceJoinRequested);
Callback<LobbyEnter> g_onLobbyEnter(kLobbyEnter, &OnLobbyEnter);
Callback<LobbyCreated> g_onLobbyCreated(kLobbyCreated, &OnLobbyCreated);
Callback<LobbyCreated> g_onLobbyCreatedResult(kLobbyCreated, &OnLobbyCreated);
Callback<P2PSessionRequest> g_onP2PRequest(kP2PSessionRequest, &OnP2PSessionRequest);
Callback<P2PSessionConnectFail> g_onP2PFail(kP2PSessionConnectFail, &OnP2PSessionConnectFail);

// Real Steam's steam_api.dll loads steamclient.dll from the Steam folder, next to steam.exe. A Steam emulator
// (non-Steam copies) has no such client and answers the flat API with its own interface layouts: hosting crashed
// there right after the lobby was made. Without the real client the coop goes over LAN/IP only.
bool RealSteamClient() {
    const HMODULE client = GetModuleHandleA("steamclient.dll");
    wchar_t path[MAX_PATH] = {};
    const DWORD length = client != nullptr ? GetModuleFileNameW(client, path, MAX_PATH) : 0;
    if (length == 0 || length >= MAX_PATH) return false;
    wchar_t* const slash = wcsrchr(path, L'\\');
    if (slash == nullptr || wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"steam.exe") != 0) return false;
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

bool Connect() {
    const HMODULE module = GetModuleHandleA("steam_api.dll");
    if (module == nullptr) {
        Log("steam: steam_api.dll is not loaded - Steam invites off");
        return false;
    }
    if (!RealSteamClient()) {
        Log("steam: no Steam client next to steam.exe (a Steam emulator?) - Steam invites off");
        return false;
    }
    bool ok = true;
    const auto steamClient = Export<SteamClientFn>(module, "SteamClient", &ok);
    const auto user = Export<HandleFn>(module, "SteamAPI_GetHSteamUser", &ok);
    const auto pipe = Export<HandleFn>(module, "SteamAPI_GetHSteamPipe", &ok);
    const auto getFriends = Export<GetInterfaceFn>(module, "SteamAPI_ISteamClient_GetISteamFriends", &ok);
    const auto getMatchmaking = Export<GetInterfaceFn>(module, "SteamAPI_ISteamClient_GetISteamMatchmaking", &ok);
    const auto getNetworking = Export<GetInterfaceFn>(module, "SteamAPI_ISteamClient_GetISteamNetworking", &ok);
    const auto getUser = Export<GetInterfaceFn>(module, "SteamAPI_ISteamClient_GetISteamUser", &ok);
    Api& a = g_api;
    a.steamId = Export<SteamIdFn>(module, "SteamAPI_ISteamUser_GetSteamID", &ok);
    a.personaName = Export<PersonaNameFn>(module, "SteamAPI_ISteamFriends_GetPersonaName", &ok);
    a.createLobby = Export<CreateLobbyFn>(module, "SteamAPI_ISteamMatchmaking_CreateLobby", &ok);
    a.joinLobby = Export<JoinLobbyFn>(module, "SteamAPI_ISteamMatchmaking_JoinLobby", &ok);
    a.leaveLobby = Export<LeaveLobbyFn>(module, "SteamAPI_ISteamMatchmaking_LeaveLobby", &ok);
    a.lobbyOwner = Export<LobbyOwnerFn>(module, "SteamAPI_ISteamMatchmaking_GetLobbyOwner", &ok);
    a.lobbyMembers = Export<LobbyMembersFn>(module, "SteamAPI_ISteamMatchmaking_GetNumLobbyMembers", &ok);
    a.lobbyMember = Export<LobbyMemberFn>(module, "SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex", &ok);
    a.setLobbyData = Export<SetLobbyDataFn>(module, "SteamAPI_ISteamMatchmaking_SetLobbyData", &ok);
    a.inviteDialog = Export<InviteDialogFn>(module, "SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog", &ok);
    a.setRichPresence = Export<SetRichPresenceFn>(module, "SteamAPI_ISteamFriends_SetRichPresence", &ok);
    a.clearRichPresence = Export<ClearRichPresenceFn>(module, "SteamAPI_ISteamFriends_ClearRichPresence", &ok);
    a.sendP2P = Export<SendP2PFn>(module, "SteamAPI_ISteamNetworking_SendP2PPacket", &ok);
    a.p2pAvailable = Export<P2PAvailableFn>(module, "SteamAPI_ISteamNetworking_IsP2PPacketAvailable", &ok);
    a.readP2P = Export<ReadP2PFn>(module, "SteamAPI_ISteamNetworking_ReadP2PPacket", &ok);
    a.acceptP2P = Export<PeerFn>(module, "SteamAPI_ISteamNetworking_AcceptP2PSessionWithUser", &ok);
    a.closeP2P = Export<PeerFn>(module, "SteamAPI_ISteamNetworking_CloseP2PSessionWithUser", &ok);
    a.allowRelay = Export<AllowRelayFn>(module, "SteamAPI_ISteamNetworking_AllowP2PPacketRelay", &ok);
    a.registerCallback = Export<RegisterCallbackFn>(module, "SteamAPI_RegisterCallback", &ok);
    a.registerCallResult = Export<RegisterCallResultFn>(module, "SteamAPI_RegisterCallResult", &ok);
    if (!ok) return false;
    const intptr_t client = steamClient();
    const int32_t hUser = user();
    const int32_t hPipe = pipe();
    if (client == 0 || hUser == 0 || hPipe == 0) {
        Log("steam: Steam is not running for this game - Steam invites off");
        return false;
    }
    a.friends = getFriends(client, hUser, hPipe, kFriendsVersion);
    a.matchmaking = getMatchmaking(client, hUser, hPipe, kMatchmakingVersion);
    a.networking = getNetworking(client, hUser, hPipe, kNetworkingVersion);
    a.user = getUser(client, hUser, hPipe, kUserVersion);
    if (a.friends == 0 || a.matchmaking == 0 || a.networking == 0 || a.user == 0) {
        Log("steam: an interface is missing - Steam invites off");
        return false;
    }
    g_self = a.steamId(a.user);
    a.allowRelay(a.networking, true);
    for (CallbackBase* callback : std::initializer_list<CallbackBase*>{&g_onInvite, &g_onJoinGame, &g_onLobbyEnter,
                                                                       &g_onLobbyCreated, &g_onP2PRequest,
                                                                       &g_onP2PFail}) {
        a.registerCallback(callback, callback->Id());
    }
    Log("steam: ready - signed in as '%s' (%llu)", a.personaName(a.friends), static_cast<unsigned long long>(g_self));
    return true;
}

}  // namespace

void Init(const char* commandLineLobby) {
    static bool tried = false;
    if (tried) return;
    tried = true;
    g_ready = Connect();
    if (!g_ready) return;
    const uint64_t lobby = ParseConnect(commandLineLobby);
    if (lobby != 0) {
        Log("steam: started by an invite - lobby %llu", static_cast<unsigned long long>(lobby));
        JoinLobby(lobby);
    }
}

bool Ready() { return g_ready; }

void HostLobby(int maxPlayers) {
    if (!g_ready || g_lobby != 0 || g_creatingLobby) return;
    g_creatingLobby = true;
    const uint64_t call = g_api.createLobby(g_api.matchmaking, kLobbyFriendsOnly, maxPlayers);
    g_api.registerCallResult(&g_onLobbyCreatedResult, call);
    Log("steam: creating a friends-only lobby for %d", maxPlayers);
}

void LeaveLobby() {
    if (!g_ready) return;
    if (g_lobby != 0) {
        g_api.leaveLobby(g_api.matchmaking, g_lobby);
        g_api.clearRichPresence(g_api.friends);
        Log("steam: left lobby %llu", static_cast<unsigned long long>(g_lobby));
    }
    g_lobby = 0;
    g_creatingLobby = false;
    g_joiningLobby = 0;
    g_joinHost = 0;
    std::lock_guard<std::mutex> guard(g_acceptedLock);
    g_accepted.clear();
}

bool HasLobby() { return g_lobby != 0; }

void OpenInviteDialog() {
    if (!g_ready || g_lobby == 0) return;
    g_api.inviteDialog(g_api.friends, g_lobby);
    Log("steam: invite dialog opened");
}

uint64_t TakeJoinRequest() {
    const uint64_t host = g_joinHost;
    g_joinHost = 0;
    return host;
}

bool SendP2P(uint64_t peer, const void* data, uint32_t size) {
    return g_ready && g_api.sendP2P(g_api.networking, peer, data, size, kSendReliable, kChannel);
}

bool ReadP2P(std::vector<uint8_t>* data, uint64_t* from) {
    uint32_t size = 0;
    if (!g_ready || !g_api.p2pAvailable(g_api.networking, &size, kChannel)) return false;
    data->resize(size);
    uint32_t got = 0;
    if (!g_api.readP2P(g_api.networking, data->data(), size, &got, from, kChannel)) return false;
    data->resize(got);
    return true;
}

void CloseP2P(uint64_t peer) {
    if (g_ready) g_api.closeP2P(g_api.networking, peer);
}

bool IsAccepted(uint64_t peer) {
    std::lock_guard<std::mutex> guard(g_acceptedLock);
    return g_accepted.count(peer) > 0;
}

void DebugTick() {
    static const bool enabled = GetEnvironmentVariableA("COOP_DEBUG_STEAM", nullptr, 0) > 0;
    static bool asked = false, shown = false;
    if (!enabled || !g_ready) return;
    if (!asked) {
        asked = true;
        HostLobby(4);
    } else if (!shown && g_lobby != 0) {
        shown = true;
        OpenInviteDialog();
    }
}

}  // namespace coop::steam
