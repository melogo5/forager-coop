// Coop session: started from the Coop menu (or -coop launch options for testing).
// Each side sends its player's position/sprite ~20 times a second; the other side shows it as a puppet.
#include "session.h"

#include <windows.h>
#include <shellapi.h>

#pragma comment(lib, "user32.lib")  // MessageBoxW: the version warning

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "MinHook.h"
#include "gui.h"
#include "lan.h"
#include "log.h"
#include "net.h"
#include "character.h"
#include "chests.h"
#include "clock.h"
#include "coins.h"
#include "debugport.h"
#include "look.h"
#include "traps.h"
#include "land.h"
#include "mobs.h"
#include "progress.h"
#include "puppet.h"
#include "quests.h"
#include "reconcile.h"
#include "skills.h"
#include "fishing.h"
#include "skilltab.h"
#include "stations.h"
#include "steam.h"
#include "storage.h"
#include "unlocks.h"
#include "sync.h"
#include "world.h"
#include "yyc.h"

#pragma comment(lib, "shell32.lib")

namespace coop {

namespace {

constexpr uint32_t kProtocolVersion = 10;  // 10: fish traps are the host's
constexpr unsigned kSendEverySteps = 3;       // 60 fps game step -> 20 updates/s
constexpr unsigned kAnnounceEverySteps = 30;  // refresh the LAN announcement twice a second
constexpr int kSlotGodMode = 0x54C;           // global.godMode (decompiled/code/var_slots.tsv)
constexpr int kSlotHp = 0x573;
constexpr int kSlotHpMax = 0x576;
constexpr int kSlotInvulFrames = 0x5A7;
constexpr double kRespawnX = 672;  // the starting island: where a new world puts the player (rmGame centre)
constexpr double kRespawnY = 672;
constexpr double kRespawnInvulSteps = 120;  // ~2 s
constexpr uint32_t kScriptToolSelected = 0x007047B0;  // ToolSelected(): the item in the player's hand
constexpr uint32_t kScriptRestartGame = 0x004A5620;  // RestartGame(): what game over uses to reach the menu
constexpr int kSlotGameMode = 0x527;  // global.gameMode: 0 classic, 1 single island, 2 speedrun, 3 hard
constexpr unsigned kGuestGiveUpSteps = 300;          // ~5 s without the host: the guest goes back to the menu
constexpr uint32_t kScriptIsMenuOpen = 0x00835040;
constexpr int kObjPlayer = 22;
constexpr int kParMenu = 238;
constexpr int kLuiBaseClass = 227;
constexpr int kMaxObjects = 512;
constexpr int kMaxGuests = 3;  // 4 players (docs/coop/MULTIPLAYER.md)

enum MessageType : uint8_t {
    kHello = 1,
    kPlayerState = 2,
    kBye = 7,        // host -> guest: the host left its world; the session is over
    kPeerState = 8,  // host -> guests: another guest's PlayerStateMsg (PeerStateMsg)
    kPeerGone = 9,   // host -> guests: u8 player - that guest left
};

#pragma pack(push, 1)
struct HelloMsg {
    uint32_t version;
};
struct PlayerStateMsg {
    float x, y;
    float imageIndex, xscale, yscale, depth;
    int32_t sprite;
    int32_t tool;  // item in hand (ToolSelected()), -4 = none
    float hp, hpMax;
    char name[16];  // not 0-terminated when all 16 are used
    look::Look look;  // the costume, for its puppet
};
struct PeerStateMsg {
    uint8_t player;
    PlayerStateMsg state;
};
#pragma pack(pop)

using session::Role;

Role g_role = Role::kNone;
bool g_playedWorld = false;  // this session has been in a world (back in the menu = the session is over)
unsigned g_steps = 0;
unsigned g_offlineSteps = 0;
bool g_leaving = false;  // guest: on the way back to the main menu
std::string g_notice;    // shown on the COOP screen when back in the menu
bool g_wasConnected = false;
bool g_inviteHinted = false;  // host: told about F2 in this world
std::string g_myName;         // shown over our character on the other players' screens; set when a session starts
std::vector<net::PeerId> g_peers;  // linked at the last step
bool g_wasInWorld = true;
std::map<net::PeerId, PlayerStateMsg> g_remotes;  // the other players' last state, shown as puppets

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
    yyc::RValue tool{};
    yyc::At<yyc::ScriptFn>(kScriptToolSelected)(player, player, &tool, 0, nullptr);
    s.tool = static_cast<int32_t>(yyc::ToReal(tool));
    const int id = static_cast<int>(yyc::GetBuiltinReal(player, yyc::va::YYVar_id));
    const std::pair<int, float*> health[] = {{kSlotHp, &s.hp}, {kSlotHpMax, &s.hpMax}};
    for (const auto& [slot, field] : health) {
        yyc::RValue value{};
        yyc::GetVar(id, slot, &value);
        *field = static_cast<float>(yyc::ToReal(value));
        yyc::Free(&value);
    }
    memcpy(s.name, g_myName.data(), (std::min)(g_myName.size(), sizeof(s.name)));
    s.look = look::Read(player);
    yyc::Free(&tool);
    return s;
}

// Coop worlds are Classic only for now (QUESTIONS.md): a host that started another mode goes back to the menu.
bool HostWorldAllowed(yyc::CInstance* localPlayer) {
    const yyc::RValue* mode = yyc::GlobalVar(kSlotGameMode);
    if (mode == nullptr || yyc::ToReal(*mode) == 0) return true;
    Log("session: game mode %.0f is not Classic - coop worlds are Classic only", yyc::ToReal(*mode));
    g_notice = "Coop needs a Classic world";
    g_leaving = true;
    yyc::RValue result{};
    yyc::At<yyc::ScriptFn>(kScriptRestartGame)(localPlayer, localPlayer, &result, 0, nullptr);
    yyc::Free(&result);
    return false;
}

// Guest: the session is over (the host left or is gone); back to the main menu, where it is reset.
void GuestGoToMenu(yyc::CInstance* localPlayer, const char* why) {
    if (g_leaving) return;
    g_leaving = true;
    Log("session: %s; back to the main menu", why);
    yyc::RValue result{};
    yyc::At<yyc::ScriptFn>(kScriptRestartGame)(localPlayer, localPlayer, &result, 0, nullptr);
    yyc::Free(&result);
}

uint32_t OurProtocol();

// Different builds of the patch may go out of sync: both players are warned, the game goes on. A Windows
// message box on its own thread, so the game keeps running behind it.
void WarnVersion(uint32_t theirs) {
    Log("session: the other player has protocol %u, ours is %u - playing on, with a warning", theirs, OurProtocol());
    wchar_t text[256];
    swprintf(text, 256,
             L"The other player has a different version of the coop patch (theirs %u, yours %u).\n\n"
             L"You can play, but things may go out of sync. Best to use the same version.",
             theirs, OurProtocol());
    std::thread([message = std::wstring(text)] {
        const int answer = MessageBoxW(nullptr, message.c_str(), L"Forager coop", MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
        Log("session: version warning closed (%d, error %lu)", answer, GetLastError());
    }).detach();
}

// The protocol version this side speaks. Testing aid: COOP_DEBUG_PROTOCOL=<n> pretends to be another build.
uint32_t OurProtocol() {
    static const uint32_t version = [] {
        char claimed[16] = {};
        return GetEnvironmentVariableA("COOP_DEBUG_PROTOCOL", claimed, sizeof(claimed)) > 0
                   ? static_cast<uint32_t>(atoi(claimed))
                   : kProtocolVersion;
    }();
    return version;
}

// Messages either side sends about the shared world (each module's own type). With more than one guest, the
// host passes a guest's on to the other guests, as if it had sent them itself. (Taking a chest or a station is
// passed on by storage/stations themselves, once the host has allowed it.)
constexpr uint8_t kSharedTypes[] = {
    40,  // storage: contents
    52,  // land: landfill
    53,  // land: dug/watered ground
    70,  // unlocks
    90,  // progress: museum lists
    91,  // quests
    94,  // stations: craft state
    93,  // chests: a treasure chest opened
};

void Relay(const net::Message& message) {
    if (std::find(std::begin(kSharedTypes), std::end(kSharedTypes), message.type) == std::end(kSharedTypes)) return;
    net::SendExcept(message.from, message.type, message.payload.data(), message.payload.size());
}

bool IsLinked(net::PeerId peer) { return std::find(g_peers.begin(), g_peers.end(), peer) != g_peers.end(); }

// Host: a chest or station taken by a guest that has left meanwhile (still in the inbox) would stay taken: its
// things were freed when it left (storage/stations::OnPeerLeft).
bool IsStaleClaim(const net::Message& message) {
    constexpr uint8_t kStorageLock = 41, kStation = 92;
    return g_role == Role::kHost && message.from != net::kHostPeer && !IsLinked(message.from) &&
           (message.type == kStorageLock || message.type == kStation);
}

// A player's own state: shown as its puppet; the host passes a guest's on to the other guests. (A guest that
// has just left may still have a state in the inbox: it would bring its puppet back, so it is dropped.)
void OnPlayerState(net::PeerId from, const PlayerStateMsg& state) {
    if (g_role == Role::kHost && !IsLinked(from)) return;
    g_remotes[from] = state;
    if (g_role != Role::kHost) return;
    const PeerStateMsg relay{from, state};
    for (const net::PeerId peer : g_peers) {
        if (peer != from) net::SendTo(peer, kPeerState, &relay, sizeof(relay));
    }
}

void HandleMessages(yyc::CInstance* localPlayer) {
    std::vector<net::Message> inbox;
    net::Drain(inbox);
    for (const auto& message : inbox) {
        if (IsStaleClaim(message)) continue;
        if (g_role == Role::kHost && message.from != net::kHostPeer) Relay(message);
        if (sync::Handle(message, localPlayer) || mobs::Handle(message, localPlayer) ||
            storage::Handle(message, localPlayer) || land::Handle(message, localPlayer) ||
            clock::Handle(message, localPlayer) ||
            unlocks::Handle(message, localPlayer) || coins::Handle(message, localPlayer) ||
            progress::Handle(message, localPlayer) || quests::Handle(message, localPlayer) ||
            stations::Handle(message, localPlayer) || skills::Handle(message) || traps::Handle(message, localPlayer) ||
            reconcile::Handle(message, localPlayer) || chests::Handle(message, localPlayer)) {
            continue;
        }
        bool guestIntroduced = false;
        if (g_role == Role::kHost && character::HostHandle(message, localPlayer, &guestIntroduced)) {
            // The guest's character answer is out; now the world, so it arrives after it.
            if (guestIntroduced) {
                skills::HostOnGuest(character::HostGuestName(message.from), message.from);
                world::HostRequestSnapshot(localPlayer, message.from);
            }
            continue;
        }
        if (message.type == kBye && g_role == Role::kGuest) {
            GuestGoToMenu(localPlayer, "the host left");
            return;  // the room is going away; the rest does not matter any more
        }
        if (message.type == kHello && message.payload.size() == sizeof(HelloMsg)) {
            HelloMsg hello{};
            memcpy(&hello, message.payload.data(), sizeof(hello));
            Log("session: player %u has protocol %u (ours %u)", message.from, hello.version, OurProtocol());
            if (hello.version != OurProtocol()) WarnVersion(hello.version);
        } else if (message.type == kPlayerState && message.payload.size() == sizeof(PlayerStateMsg)) {
            PlayerStateMsg state{};
            memcpy(&state, message.payload.data(), sizeof(state));
            OnPlayerState(message.from, state);
        } else if (message.type == kPeerState && message.payload.size() == sizeof(PeerStateMsg) &&
                   g_role == Role::kGuest) {
            PeerStateMsg peer{};
            memcpy(&peer, message.payload.data(), sizeof(peer));
            g_remotes[peer.player] = peer.state;
        } else if (message.type == kPeerGone && message.payload.size() == 1 && g_role == Role::kGuest) {
            g_remotes.erase(message.payload[0]);
            puppet::Destroy(localPlayer, message.payload[0]);
        }
    }
}

int MaxGuests();

void Announce(size_t guests) {
    const yyc::RValue* slot = yyc::GlobalVar(yyc::slot::saveFile);
    const int slotNumber = slot != nullptr ? static_cast<int>(yyc::ToReal(*slot)) : 0;
    lan::SetAnnouncement(true, "World " + std::to_string(slotNumber + 1), 1 + static_cast<int>(guests),
                         1 + MaxGuests());
}

// Greets every newly linked player (a guest: the host) and notes who left. The host removes a guest that left
// and tells the others to.
void TrackPeers(yyc::CInstance* localPlayer, const std::vector<net::PeerId>& peers) {
    const auto known = [](const std::vector<net::PeerId>& list, net::PeerId peer) {
        return std::find(list.begin(), list.end(), peer) != list.end();
    };
    for (const net::PeerId peer : peers) {
        if (known(g_peers, peer)) continue;
        Log("session: player %u joined", peer);
        const HelloMsg hello{OurProtocol()};
        net::SendTo(peer, kHello, &hello, sizeof(hello));
        if (g_role == Role::kHost) {
            stations::OnPeerJoined(peer);
            storage::OnPeerJoined(peer);
        }
    }
    for (const net::PeerId peer : g_peers) {
        if (known(peers, peer)) continue;
        Log("session: player %u left", peer);
        g_remotes.erase(peer);
        puppet::Destroy(localPlayer, peer);
        if (g_role != Role::kHost) continue;
        net::Send(kPeerGone, &peer, sizeof(peer));
        storage::OnPeerLeft(localPlayer, peer);
        stations::OnPeerLeft(peer);
    }
    g_peers = peers;
}

// Guests the host takes at once. Testing aid: COOP_DEBUG_MAX_GUESTS=<n> takes fewer (up to 3).
int MaxGuests() {
    static const int guests = [] {
        char value[16] = {};
        const int wanted = GetEnvironmentVariableA("COOP_DEBUG_MAX_GUESTS", value, sizeof(value)) > 0
                               ? atoi(value)
                               : kMaxGuests;
        return (std::min)((std::max)(wanted, 1), kMaxGuests);
    }();
    return guests;
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

// Testing aid: COOP_DEBUG_CHEATS=1 turns the world's cheats on (console /give, /spawn...): a new world has them
// off, and the mode screen's ENABLE CHEATS button does not take clicks sent by the test tools.
void ApplyDebugCheats() {
    constexpr int kSlotCheatsEnabled = 0x101;  // global.cheatsEnabled (decompiled/code/var_slots.tsv)
    static const bool enabled = GetEnvironmentVariableA("COOP_DEBUG_CHEATS", nullptr, 0) > 0;
    if (!enabled) return;
    if (yyc::RValue* cheats = yyc::GlobalVar(kSlotCheatsEnabled)) {
        if (yyc::ToReal(*cheats) != 0 && (cheats->kind & yyc::kKindMask) == yyc::kReal) return;
        if ((cheats->kind & yyc::kKindMask) == yyc::kReal || (cheats->kind & yyc::kKindMask) == 0xFFFFFF) {
            *cheats = yyc::Real(1);
        }
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

// Host with a Steam lobby: F2 opens Steam's "invite friends" dialog; said once per world.
void SteamInvites(yyc::CInstance* localPlayer) {
    if (!steam::HasLobby()) return;
    if (!g_inviteHinted) {
        g_inviteHinted = true;
        stations::Say(localPlayer, "F2: invite Steam friends");
    }
    gui::Begin(localPlayer);
    if (gui::KeyPressed(VK_F2)) steam::OpenInviteDialog();
}

// Runs after the local player's Step_0, i.e. only while in a world.

// Testing aid: COOP_DEBUG_ROOM=<room> goes into that room ~8 s into the session (e.g. 24, the museum) and back to
// the world ~10 s later; COOP_DEBUG_ROOM_AT=<seconds> goes that late instead.
void DebugVisitRoom(yyc::CInstance* localPlayer, bool inWorld) {
    static const int room = [] {
        char value[16] = {};
        return GetEnvironmentVariableA("COOP_DEBUG_ROOM", value, sizeof(value)) > 0 ? atoi(value) : 0;
    }();
    static const unsigned at = [] {
        char value[16] = {};
        return GetEnvironmentVariableA("COOP_DEBUG_ROOM_AT", value, sizeof(value)) > 0 ? atoi(value) * 60u : 480u;
    }();
    static unsigned steps = 0;
    if (room == 0) return;
    ++steps;
    const bool go = inWorld && steps == at;
    const bool back = !inWorld && steps >= at + 600 && steps < at + 610;
    if (!go && !back) return;
    yyc::RValue target = yyc::Real(go ? room : 8);
    yyc::RValue result{};
    yyc::CallBuiltin("room_goto", localPlayer, 1, &target, &result);
    yyc::Free(&result);
    Log("session: debug goes to room %d", go ? room : 8);
}

void Tick(yyc::CInstance* localPlayer) {
    debugport::Tick(localPlayer);
    constexpr int kRoomWorld = 8;  // rmGame
    look::Tick(localPlayer, g_role == Role::kGuest,
               static_cast<int>(yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_room)) == kRoomWorld);
    ApplyDebugGodMode();
    ApplyDebugCheats();
    if (g_role == Role::kNone || g_leaving) return;
    if (!g_playedWorld && g_role == Role::kHost && !HostWorldAllowed(localPlayer)) {
        g_playedWorld = true;  // back in the menu this ends the session
        return;
    }
    if (!g_playedWorld) Log("session: in the world");  // test scripts wait for it (the world has loaded)
    g_playedWorld = true;
    puppet::HoldLocalPlayer(skilltab::IsOpen());  // the player stands while the skill tree is open
    stations::DebugTick(localPlayer);
    const std::vector<net::PeerId> peers = net::Peers();
    const bool connected = !peers.empty();
    if (g_role == Role::kHost && g_steps % kAnnounceEverySteps == 0) Announce(peers.size());
    TrackPeers(localPlayer, peers);
    if (g_role == Role::kHost) world::HostTick();
    if (g_role == Role::kGuest) {
        character::GuestTick(localPlayer);
    }
    if (!connected && g_wasConnected) {
        Log("session: peer gone");
        g_remotes.clear();
        puppet::DestroyAll(localPlayer);
        sync::Reset();  // a guest coming back starts over: new replicas, new drops
        storage::Reset();
        stations::Reset();
        fishing::Reset();
        reconcile::Reset();
    }
    g_wasConnected = connected;
    puppet::SetEnemiesSeeAll(g_role == Role::kHost && connected);
    puppet::SetFamiliarsFollowHost(g_role == Role::kGuest && connected);
    ++g_steps;
    if (!connected) {
        HandleMessages(localPlayer);  // what arrived before the link went, e.g. the host saying goodbye
        if (g_role == Role::kGuest && ++g_offlineSteps == kGuestGiveUpSteps) {
            GuestGoToMenu(localPlayer, "the host is gone");
        }
        return;
    }
    g_offlineSteps = 0;

    // Inside a building (museum, wizard tower, druid tree) the world room sleeps: nothing here is the shared
    // world, so nothing is synced; the peer's news waits in the inbox until we are back. (A census taken in
    // there would make the guest remove its whole surroundings.)
    constexpr int kRoomGame = 8;  // rmGame (decompiled/data/rooms.tsv)
    const bool inWorld = static_cast<int>(yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_room)) == kRoomGame;
    if (inWorld != g_wasInWorld) Log("session: %s the world room", inWorld ? "back in" : "left");
    DebugVisitRoom(localPlayer, inWorld);
    skilltab::DebugTick(localPlayer);
    g_wasInWorld = inWorld;
    if (!inWorld) return;

    if (g_role == Role::kHost) SteamInvites(localPlayer);
    sync::Tick(localPlayer);
    storage::DebugTick(localPlayer);
    land::DebugTick(localPlayer);
    if (g_role == Role::kHost) mobs::HostTick(localPlayer);
    if (g_role == Role::kHost) clock::HostTick(localPlayer);
    unlocks::Tick(localPlayer);
    if (g_role == Role::kHost) reconcile::HostTick(localPlayer);
    reconcile::DebugTick(localPlayer);
    chests::DebugTick(localPlayer);
    if (g_role == Role::kHost) coins::HostTick(localPlayer);
    if (g_role == Role::kGuest) coins::GuestTick(localPlayer);
    progress::Tick(localPlayer);
    quests::Tick(localPlayer);
    stations::Tick(localPlayer, g_role == Role::kGuest);
    fishing::Tick(localPlayer);
    skills::Tick(localPlayer);
    HandleMessages(localPlayer);
    if (g_steps % kSendEverySteps == 0) {
        const PlayerStateMsg state = CaptureState(localPlayer);
        net::Send(kPlayerState, &state, sizeof(state));
    }
    for (const auto& [player, remote] : g_remotes) {
        puppet::State target{remote.x,      remote.y,     remote.imageIndex, remote.xscale, remote.yscale,
                             remote.depth,  remote.sprite, remote.tool,       remote.hp,     remote.hpMax};
        memcpy(target.name, remote.name, sizeof(target.name));
        target.look = remote.look;
        puppet::Update(localPlayer, player, target);
    }
}

}  // namespace

namespace session {

void Host() {
    if (g_role != Role::kNone) return;
    g_role = Role::kHost;
    g_myName = character::OwnName();
    net::StartHost(kGamePort, MaxGuests());
    lan::StartAnnouncing(kGamePort);
    if (steam::Ready()) {
        steam::HostLobby(1 + MaxGuests());
        net::HostSteam();
    }
    Log("session: hosting on port %u", kGamePort);
}

// Guest: what arrives before the game loop takes it (the host's world and our character) and what to say first.
void PrepareGuest() {
    g_role = Role::kGuest;
    g_myName = character::OwnName();
    net::SetReceiveHandler([](const net::Message& m) {
        return world::HandleMessage(m) || character::GuestHandleReceive(m);
    });
    net::SetConnectHandler(&character::GuestOnConnect);
}

void Join(const std::string& address, uint16_t port) {
    if (g_role != Role::kNone) return;
    PrepareGuest();
    Log("session: joining %s:%u", address.c_str(), port);
    net::StartJoin(address, port);
}

void JoinSteam(uint64_t hostSteamId) {
    if (g_role != Role::kNone || !steam::Ready()) return;
    PrepareGuest();
    Log("session: joining Steam account %llu", static_cast<unsigned long long>(hostSteamId));
    net::StartJoinSteam(hostSteamId);
}

void Leave() {
    if (g_role == Role::kNone) return;
    const bool host = g_role == Role::kHost;
    if (host) net::Send(kBye, nullptr, 0);
    net::Stop();
    steam::LeaveLobby();
    skilltab::Reset();
    puppet::HoldLocalPlayer(false);
    g_inviteHinted = false;
    g_role = Role::kNone;
    g_playedWorld = false;
    g_leaving = false;
    g_offlineSteps = 0;
    g_wasConnected = false;
    g_peers.clear();
    g_remotes.clear();
    character::Reset();
    world::Reset();
    sync::Reset();
    mobs::Reset();
    storage::Reset();
    unlocks::Reset();
    coins::Reset();
    progress::Reset();
    quests::Reset();
    stations::Reset();
    fishing::Reset();
    skills::Reset();
    reconcile::Reset();
    look::Reset();
    Log(host ? "session: stopped hosting; can create again" : "session: left the host's world; can join again");
}

bool PlayedWorld() { return g_playedWorld; }

std::string TakeNotice() {
    std::string notice;
    notice.swap(g_notice);
    return notice;
}

Role CurrentRole() { return g_role; }
bool IsConnected() { return net::IsConnected(); }

}  // namespace session

bool SessionInstall() {
    debugport::Start();
    if (!puppet::InstallHooks(&Tick, &RespawnIfDead)) return false;
    if (MH_CreateHook(yyc::At<void*>(kScriptIsMenuOpen), reinterpret_cast<void*>(&IsMenuOpenHook),
                      reinterpret_cast<void**>(&g_origIsMenuOpen)) != MH_OK) {
        Log("session: hook IsMenuOpen failed");
        return false;
    }
    if (!world::InstallHooks([] { return g_role == Role::kGuest; })) return false;
    if (!skilltab::InstallHooks([] { return g_role != Role::kNone; })) return false;
    if (!fishing::InstallHooks([] { return g_role != Role::kNone; })) return false;
    const auto isHost = [] { return g_role == Role::kHost; };
    const auto isGuest = [] { return g_role == Role::kGuest; };
    if (!sync::InstallHooks(isHost, isGuest) || !mobs::InstallHooks(isHost, isGuest) ||
        !character::InstallHooks(isGuest) || !storage::InstallHooks() || !land::InstallHooks(isHost, isGuest) ||
        !progress::InstallHooks() || !stations::InstallHooks() || !chests::InstallHooks() ||
        !traps::InstallHooks(isHost, isGuest)) {
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
