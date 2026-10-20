#include "steam.h"

#include <windows.h>

#include <cstdint>

#include "MinHook.h"
#include "log.h"

namespace coop::steam {

namespace {

// steam_api.dll's flat API (Steamworks SDK ~1.41: SteamClient017, SteamUser019). All __cdecl; the first argument
// of an interface function is the interface.
using SteamClientFn = intptr_t(__cdecl*)();
using HandleFn = int32_t(__cdecl*)();
using GetInterfaceFn = intptr_t(__cdecl*)(intptr_t client, int32_t user, int32_t pipe, const char* version);
using PersonaNameFn = const char*(__cdecl*)(intptr_t friends);
using CreateLobbyFn = uint64_t(__cdecl*)(intptr_t matchmaking, int type, int maxMembers);
using SetLobbyDataFn = bool(__cdecl*)(intptr_t matchmaking, uint64_t lobby, const char* key, const char* value);
using InviteDialogFn = void(__cdecl*)(intptr_t friends, uint64_t lobby);
using RegisterCallbackFn = void(__cdecl*)(void* callback, int id);
using RegisterCallResultFn = void(__cdecl*)(void* callback, uint64_t call);
using RunCallbacksFn = void(__cdecl*)();

constexpr char kFriendsVersion[] = "SteamFriends015";
constexpr char kMatchmakingVersion[] = "SteamMatchMaking009";
constexpr int kLobbyCreated = 513;      // LobbyCreated_t: k_iSteamMatchmakingCallbacks + 13
constexpr int kLobbyFriendsOnly = 1;    // k_ELobbyTypeFriendsOnly
constexpr int kMaxPlayers = 4;
constexpr int kResultOk = 1;            // k_EResultOK
constexpr unsigned kCountCallbacksFor = 300;  // steps (~5 s)

#pragma pack(push, 8)  // Steam's callback structs on Windows
struct LobbyCreated {
    int32_t result;
    uint64_t lobby;
};
#pragma pack(pop)

// steam_api's callback object (CCallbackBase in the SDK): the same virtual functions in the same order, so MSVC
// lays out the same table; steam_api fills in the flags and the callback id.
class CallbackBase {
public:
    virtual void Run(void* param) = 0;
    virtual void Run(void* param, bool ioFailure, uint64_t call) = 0;
    virtual int GetCallbackSizeBytes() = 0;

protected:
    uint8_t flags_ = 0;
    int callback_ = 0;
};

struct Api {
    intptr_t friends = 0;
    intptr_t matchmaking = 0;
    CreateLobbyFn createLobby = nullptr;
    SetLobbyDataFn setLobbyData = nullptr;
    InviteDialogFn inviteDialog = nullptr;
    PersonaNameFn personaName = nullptr;
    RegisterCallbackFn registerCallback = nullptr;
    RegisterCallResultFn registerCallResult = nullptr;
};
Api g_api;

RunCallbacksFn g_origRunCallbacks = nullptr;
volatile LONG g_callbackRuns = 0;

void __cdecl RunCallbacksHook() {
    InterlockedIncrement(&g_callbackRuns);
    g_origRunCallbacks();
}

void OnLobbyCreated(const LobbyCreated& created);

// Both a callback (posted to everyone) and the call result of CreateLobby arrive: the first one is used.
bool g_lobbyHandled = false;
class LobbyCreatedHandler : public CallbackBase {
public:
    LobbyCreatedHandler() { callback_ = kLobbyCreated; }
    void Run(void* param) override { Handle(param); }
    void Run(void* param, bool ioFailure, uint64_t) override {
        if (ioFailure) {
            Log("steam: creating the lobby failed (I/O)");
            return;
        }
        Handle(param);
    }
    int GetCallbackSizeBytes() override { return sizeof(LobbyCreated); }

private:
    void Handle(void* param) {
        if (g_lobbyHandled) return;
        g_lobbyHandled = true;
        OnLobbyCreated(*static_cast<const LobbyCreated*>(param));
    }
};
LobbyCreatedHandler g_lobbyCreated;
LobbyCreatedHandler g_lobbyCreatedResult;

template <typename Fn>
Fn Export(HMODULE module, const char* name) {
    const Fn fn = reinterpret_cast<Fn>(GetProcAddress(module, name));
    if (fn == nullptr) Log("steam: steam_api.dll has no %s", name);
    return fn;
}

bool Connect() {
    const HMODULE module = GetModuleHandleA("steam_api.dll");
    if (module == nullptr) {
        Log("steam: steam_api.dll is not loaded");
        return false;
    }
    const auto steamClient = Export<SteamClientFn>(module, "SteamClient");
    const auto user = Export<HandleFn>(module, "SteamAPI_GetHSteamUser");
    const auto pipe = Export<HandleFn>(module, "SteamAPI_GetHSteamPipe");
    const auto getFriends = Export<GetInterfaceFn>(module, "SteamAPI_ISteamClient_GetISteamFriends");
    const auto getMatchmaking = Export<GetInterfaceFn>(module, "SteamAPI_ISteamClient_GetISteamMatchmaking");
    g_api.createLobby = Export<CreateLobbyFn>(module, "SteamAPI_ISteamMatchmaking_CreateLobby");
    g_api.setLobbyData = Export<SetLobbyDataFn>(module, "SteamAPI_ISteamMatchmaking_SetLobbyData");
    g_api.inviteDialog = Export<InviteDialogFn>(module, "SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog");
    g_api.personaName = Export<PersonaNameFn>(module, "SteamAPI_ISteamFriends_GetPersonaName");
    g_api.registerCallback = Export<RegisterCallbackFn>(module, "SteamAPI_RegisterCallback");
    g_api.registerCallResult = Export<RegisterCallResultFn>(module, "SteamAPI_RegisterCallResult");
    const auto runCallbacks = Export<RunCallbacksFn>(module, "SteamAPI_RunCallbacks");
    if (steamClient == nullptr || user == nullptr || pipe == nullptr || getFriends == nullptr ||
        getMatchmaking == nullptr || g_api.createLobby == nullptr || g_api.setLobbyData == nullptr ||
        g_api.inviteDialog == nullptr || g_api.personaName == nullptr || g_api.registerCallback == nullptr ||
        g_api.registerCallResult == nullptr || runCallbacks == nullptr) {
        return false;
    }
    const intptr_t client = steamClient();
    const int32_t hUser = user();
    const int32_t hPipe = pipe();
    Log("steam: client %p, user %d, pipe %d", reinterpret_cast<void*>(client), hUser, hPipe);
    if (client == 0 || hUser == 0 || hPipe == 0) return false;
    g_api.friends = getFriends(client, hUser, hPipe, kFriendsVersion);
    g_api.matchmaking = getMatchmaking(client, hUser, hPipe, kMatchmakingVersion);
    Log("steam: %s %p, %s %p", kFriendsVersion, reinterpret_cast<void*>(g_api.friends), kMatchmakingVersion,
        reinterpret_cast<void*>(g_api.matchmaking));
    if (g_api.friends == 0 || g_api.matchmaking == 0) return false;
    Log("steam: signed in as '%s'", g_api.personaName(g_api.friends));
    const bool hooked =
        MH_CreateHook(reinterpret_cast<void*>(runCallbacks), reinterpret_cast<void*>(&RunCallbacksHook),
                      reinterpret_cast<void**>(&g_origRunCallbacks)) == MH_OK &&
        MH_EnableHook(reinterpret_cast<void*>(runCallbacks)) == MH_OK;
    if (!hooked) Log("steam: SteamAPI_RunCallbacks hook FAILED");
    return true;
}

void OnLobbyCreated(const LobbyCreated& created) {
    Log("steam: lobby created: result %d, lobby %llu", created.result,
        static_cast<unsigned long long>(created.lobby));
    if (created.result != kResultOk) return;
    g_api.setLobbyData(g_api.matchmaking, created.lobby, "forager_coop", "1");
    g_api.inviteDialog(g_api.friends, created.lobby);
    Log("steam: invite dialog opened for the lobby");
}

}  // namespace

void Tick(yyc::CInstance*) {
    static const bool enabled = GetEnvironmentVariableA("COOP_DEBUG_STEAM", nullptr, 0) > 0;
    static unsigned steps = 0;
    static bool connected = false;
    if (!enabled) return;
    ++steps;
    if (steps == 1) {
        connected = Connect();
        if (!connected) return;
        g_api.registerCallback(&g_lobbyCreated, kLobbyCreated);
        const uint64_t call = g_api.createLobby(g_api.matchmaking, kLobbyFriendsOnly, kMaxPlayers);
        g_api.registerCallResult(&g_lobbyCreatedResult, call);
        Log("steam: creating a friends-only lobby for %d (call %llu)", kMaxPlayers,
            static_cast<unsigned long long>(call));
    } else if (steps == 1 + kCountCallbacksFor && connected) {
        Log("steam: the game ran Steam callbacks %ld times in %u steps", g_callbackRuns, kCountCallbacksFor);
    }
}

}  // namespace coop::steam
