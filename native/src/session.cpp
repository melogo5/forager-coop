// Coop session: started from the Coop menu (or -coop launch options for testing).
// Each side sends its player's position/sprite ~20 times a second; the other side shows it as a puppet.
#include "session.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "MinHook.h"
#include "lan.h"
#include "log.h"
#include "net.h"
#include "character.h"
#include "clock.h"
#include "land.h"
#include "mobs.h"
#include "puppet.h"
#include "storage.h"
#include "sync.h"
#include "world.h"
#include "yyc.h"

#pragma comment(lib, "shell32.lib")

namespace coop {

namespace {

constexpr uint32_t kProtocolVersion = 3;
constexpr unsigned kSendEverySteps = 3;       // 60 fps game step -> 20 updates/s
constexpr unsigned kAnnounceEverySteps = 30;  // refresh the LAN announcement twice a second
constexpr int kSlotGodMode = 0x54C;           // global.godMode (decompiled/code/var_slots.tsv)
constexpr int kSlotHp = 0x573;
constexpr int kSlotHpMax = 0x576;
constexpr int kSlotInvulFrames = 0x5A7;
constexpr double kRespawnX = 672;  // the starting island: where a new world puts the player (rmGame centre)
constexpr double kRespawnY = 672;
constexpr double kRespawnInvulSteps = 120;  // ~2 s
constexpr uint32_t kScriptRestartGame = 0x004A5620;  // RestartGame(): what game over uses to reach the menu
constexpr uint32_t kScriptIsMenuOpen = 0x00835040;
constexpr int kObjPlayer = 22;
constexpr int kParMenu = 238;
constexpr int kLuiBaseClass = 227;
constexpr int kMaxObjects = 512;

enum MessageType : uint8_t {
    kHello = 1,
    kPlayerState = 2,
    kBye = 7,  // host -> guest: the host left its world; the session is over
};

#pragma pack(push, 1)
struct HelloMsg {
    uint32_t version;
};
struct PlayerStateMsg {
    float x, y;
    float imageIndex, xscale, yscale, depth;
    int32_t sprite;
};
#pragma pack(pop)

using session::Role;

Role g_role = Role::kNone;
bool g_playedWorld = false;  // this session has been in a world (back in the menu = the session is over)
unsigned g_steps = 0;
bool g_wasConnected = false;
bool g_haveRemote = false;
PlayerStateMsg g_remote{};

bool ParseArgs(Role* role, std::string* address, uint16_t* port) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr) return false;
    bool ok = false;
    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"-coop") != 0) continue;
        if (i + 2 < argc && wcscmp(argv[i + 1], L"host") == 0) {
            *role = Role::kHost;
            *port = static_cast<uint16_t>(_wtoi(argv[i + 2]));
            ok = *port != 0;
        } else if (i + 3 < argc && wcscmp(argv[i + 1], L"join") == 0) {
            char buffer[256] = {};
            WideCharToMultiByte(CP_UTF8, 0, argv[i + 2], -1, buffer, sizeof(buffer) - 1, nullptr, nullptr);
            *role = Role::kGuest;
            *address = buffer;
            *port = static_cast<uint16_t>(_wtoi(argv[i + 3]));
            ok = *port != 0 && !address->empty();
        }
        break;
    }
    LocalFree(argv);
    return ok;
}

PlayerStateMsg CaptureState(yyc::CInstance* player) {
    PlayerStateMsg s{};
    s.x = static_cast<float>(yyc::GetBuiltinReal(player, yyc::va::YYVar_x));
    s.y = static_cast<float>(yyc::GetBuiltinReal(player, yyc::va::YYVar_y));
    s.imageIndex = static_cast<float>(yyc::GetBuiltinReal(player, yyc::va::YYVar_image_index));
    s.xscale = static_cast<float>(yyc::GetBuiltinReal(player, yyc::va::YYVar_image_xscale));
    s.yscale = static_cast<float>(yyc::GetBuiltinReal(player, yyc::va::YYVar_image_yscale));
    s.depth = static_cast<float>(yyc::GetBuiltinReal(player, yyc::va::YYVar_depth));
    s.sprite = static_cast<int32_t>(yyc::GetBuiltinReal(player, yyc::va::YYVar_sprite_index));
    return s;
}

void HandleMessages(yyc::CInstance* localPlayer) {
    std::vector<net::Message> inbox;
    net::Drain(inbox);
    for (const auto& message : inbox) {
        if (sync::Handle(message, localPlayer) || mobs::Handle(message, localPlayer) ||
            storage::Handle(message, localPlayer) || land::Handle(message, localPlayer) ||
            clock::Handle(message, localPlayer)) {
            continue;
        }
        bool guestIntroduced = false;
        if (g_role == Role::kHost && character::HostHandle(message, localPlayer, &guestIntroduced)) {
            // The guest's character answer is out; now the world, so it arrives after it.
            if (guestIntroduced) world::HostRequestSnapshot(localPlayer);
            continue;
        }
        if (message.type == kBye && g_role == Role::kGuest) {
            Log("session: the host left; back to the main menu");
            yyc::RValue result{};
            yyc::At<yyc::ScriptFn>(kScriptRestartGame)(localPlayer, localPlayer, &result, 0, nullptr);
            yyc::Free(&result);
            return;  // the room is going away; the rest does not matter any more
        }
        if (message.type == kHello && message.payload.size() == sizeof(HelloMsg)) {
            HelloMsg hello{};
            memcpy(&hello, message.payload.data(), sizeof(hello));
            Log("session: peer protocol %u (ours %u)", hello.version, kProtocolVersion);
        } else if (message.type == kPlayerState && message.payload.size() == sizeof(PlayerStateMsg)) {
            memcpy(&g_remote, message.payload.data(), sizeof(g_remote));
            g_haveRemote = true;
        }
    }
}

void Announce(bool connected) {
    const yyc::RValue* slot = yyc::GlobalVar(yyc::slot::saveFile);
    const int slotNumber = slot != nullptr ? static_cast<int>(yyc::ToReal(*slot)) : 0;
    lan::SetAnnouncement(true, "World " + std::to_string(slotNumber + 1), connected ? 2 : 1);
}

// Testing aid: COOP_DEBUG_GODMODE=1 keeps the game's own god mode on, so test runs survive enemies.
void ApplyDebugGodMode() {
    static const bool enabled = GetEnvironmentVariableA("COOP_DEBUG_GODMODE", nullptr, 0) > 0;
    if (!enabled) return;
    if (yyc::RValue* godMode = yyc::GlobalVar(kSlotGodMode)) {
        yyc::Free(godMode);
        *godMode = yyc::Real(1);
    }
}

// In a coop session a death is not a game over (which also reloads the menu and drops the peer):
// the player gets back up on the starting island with full health. No penalty (docs/coop/SPEC.md).
void RespawnIfDead(yyc::CInstance* localPlayer) {
    if (g_role == Role::kNone) return;
    const int id = static_cast<int>(yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_id));
    yyc::RValue hp{};
    yyc::GetVar(id, kSlotHp, &hp);
    const bool dead = (hp.kind & yyc::kKindMask) == yyc::kReal && hp.real < 1;
    yyc::Free(&hp);
    if (!dead) return;
    yyc::RValue hpMax{};
    yyc::GetVar(id, kSlotHpMax, &hpMax);
    yyc::RValue full = yyc::Real((std::max)(1.0, yyc::ToReal(hpMax)));
    yyc::Free(&hpMax);
    yyc::SetVar(id, kSlotHp, &full);
    yyc::RValue invulnerable = yyc::Real(kRespawnInvulSteps);
    yyc::SetVar(id, kSlotInvulFrames, &invulnerable);
    yyc::SetBuiltinReal(localPlayer, yyc::va::YYVar_x, kRespawnX);
    yyc::SetBuiltinReal(localPlayer, yyc::va::YYVar_y, kRespawnY);
    Log("session: died - respawned on the starting island with %.0f hp", full.real);
}

// Forager has no global pause: world objects (enemies, chests, ...) call IsMenuOpen() themselves and
// freeze while a menu is open. With two players that would stop the shared world whenever either one
// opens the inventory, so in a session only the player and the UI get the real answer.
yyc::ScriptFn g_origIsMenuOpen = nullptr;
int8_t g_menuAware[kMaxObjects] = {};  // 0 unknown, 1 player/UI, -1 world object

bool IsMenuAware(yyc::CInstance* self, int object) {
    if (object < 0 || object >= kMaxObjects) return true;
    int8_t& cached = g_menuAware[object];
    if (cached == 0) {
        cached = -1;
        if (object == kObjPlayer) cached = 1;
        for (const int parent : {kParMenu, kLuiBaseClass}) {
            yyc::RValue args[2] = {yyc::Real(object), yyc::Real(parent)};
            yyc::RValue result{};
            yyc::CallBuiltin("object_is_ancestor", self, 2, args, &result);
            if (object == parent || yyc::ToReal(result) != 0) cached = 1;
            yyc::Free(&result);
        }
    }
    return cached == 1;
}

yyc::RValue* __cdecl IsMenuOpenHook(yyc::CInstance* self, yyc::CInstance* other, yyc::RValue* result, int argc,
                                    yyc::RValue** args) {
    if (g_role == Role::kNone || self == nullptr ||
        IsMenuAware(self, static_cast<int>(yyc::GetBuiltinReal(self, yyc::va::YYVar_object_index)))) {
        return g_origIsMenuOpen(self, other, result, argc, args);
    }
    yyc::Free(result);
    *result = yyc::Real(0);
    return result;
}

// Runs after the local player's Step_0, i.e. only while in a world.
void Tick(yyc::CInstance* localPlayer) {
    ApplyDebugGodMode();
    if (g_role == Role::kNone) return;
    g_playedWorld = true;
    const bool connected = net::IsConnected();
    if (g_role == Role::kHost && g_steps % kAnnounceEverySteps == 0) Announce(connected);
    if (connected && !g_wasConnected) {
        const HelloMsg hello{kProtocolVersion};
        net::Send(kHello, &hello, sizeof(hello));
    }
    if (g_role == Role::kHost) world::HostTick();
    if (g_role == Role::kGuest) {
        character::GuestTick(localPlayer);
    }
    if (!connected && g_wasConnected) {
        Log("session: peer gone");
        g_haveRemote = false;
        puppet::Destroy(localPlayer);
        sync::Reset();  // a guest coming back starts over: new replicas, new drops
        storage::Reset();
    }
    g_wasConnected = connected;
    ++g_steps;
    if (!connected) {
        HandleMessages(localPlayer);  // what arrived before the link went, e.g. the host saying goodbye
        return;
    }

    sync::Tick(localPlayer);
    storage::DebugTick(localPlayer);
    land::DebugTick(localPlayer);
    if (g_role == Role::kHost) mobs::HostTick(localPlayer);
    if (g_role == Role::kHost) clock::HostTick(localPlayer);
    HandleMessages(localPlayer);
    if (g_steps % kSendEverySteps == 0) {
        const PlayerStateMsg state = CaptureState(localPlayer);
        net::Send(kPlayerState, &state, sizeof(state));
    }
    if (g_haveRemote) {
        const puppet::State target{g_remote.x,      g_remote.y,     g_remote.imageIndex, g_remote.xscale,
                                   g_remote.yscale, g_remote.depth, g_remote.sprite};
        puppet::Update(localPlayer, target);
    }
}

}  // namespace

namespace session {

void Host() {
    if (g_role != Role::kNone) return;
    g_role = Role::kHost;
    net::StartHost(kGamePort);
    lan::StartAnnouncing(kGamePort);
    Log("session: hosting on port %u", kGamePort);
}

void Join(const std::string& address, uint16_t port) {
    if (g_role != Role::kNone) return;
    g_role = Role::kGuest;
    Log("session: joining %s:%u", address.c_str(), port);
    net::SetReceiveHandler([](const net::Message& m) {
        return world::HandleMessage(m) || character::GuestHandleReceive(m);
    });
    net::SetConnectHandler(&character::GuestOnConnect);
    net::StartJoin(address, port);
}

void Leave() {
    if (g_role == Role::kNone) return;
    const bool host = g_role == Role::kHost;
    if (host) net::Send(kBye, nullptr, 0);
    net::Stop();
    g_role = Role::kNone;
    g_playedWorld = false;
    g_wasConnected = false;
    g_haveRemote = false;
    character::Reset();
    world::Reset();
    sync::Reset();
    mobs::Reset();
    storage::Reset();
    Log(host ? "session: stopped hosting; can create again" : "session: left the host's world; can join again");
}

bool PlayedWorld() { return g_playedWorld; }

Role CurrentRole() { return g_role; }
bool IsConnected() { return net::IsConnected(); }

}  // namespace session

bool SessionInstall() {
    if (!puppet::InstallHooks(&Tick, &RespawnIfDead)) return false;
    if (MH_CreateHook(yyc::At<void*>(kScriptIsMenuOpen), reinterpret_cast<void*>(&IsMenuOpenHook),
                      reinterpret_cast<void**>(&g_origIsMenuOpen)) != MH_OK) {
        Log("session: hook IsMenuOpen failed");
        return false;
    }
    if (!world::InstallHooks([] { return g_role == Role::kGuest; })) return false;
    const auto isHost = [] { return g_role == Role::kHost; };
    const auto isGuest = [] { return g_role == Role::kGuest; };
    if (!sync::InstallHooks(isHost, isGuest) || !mobs::InstallHooks(isHost, isGuest) ||
        !character::InstallHooks(isGuest) || !storage::InstallHooks() || !land::InstallHooks(isHost, isGuest)) {
        return false;
    }
    Role role = Role::kNone;
    std::string address;
    uint16_t port = 0;
    if (ParseArgs(&role, &address, &port)) {
        if (role == Role::kHost) {
            session::Host();
        } else {
            session::Join(address, port);
        }
    }
    return true;
}

}  // namespace coop
