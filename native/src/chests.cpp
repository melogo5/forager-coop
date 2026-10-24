#include "chests.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "MinHook.h"
#include "log.h"
#include "mutant.h"
#include "stations.h"
#include "sync.h"

namespace coop::chests {

namespace {

constexpr uint8_t kChestOpened = 93;   // ChestMsg, any -> host -> the rest: a keyless chest opened; open yours
constexpr uint8_t kChestMoved = 97;    // host -> guest: ChestMoveMsg, where a new chest came to rest
constexpr uint8_t kChestKey = 98;      // guest -> host: ChestMsg, the guest put a key in this chest
constexpr uint8_t kChestKeys = 99;     // host -> guests: KeysMsg, keys in it so far
constexpr uint8_t kChestReward = 100;  // host -> guests: RewardMsg, every key is in: open it, the reward is this item
// The chests' Other_11 (opening: loot, animation; the chest destroys itself afterwards).
constexpr uint32_t kOpenEvents[] = {
    0x00C3CC20,  // objTreasureChest: needs a key (item 110), gives a gear item (objItemGet)
    0x00C47C10,  // objTreasureChestSmall
    0x00C53AA0,  // objTreasureChestVoid
};
constexpr int kOpenEventCount = sizeof(kOpenEvents) / sizeof(kOpenEvents[0]);
constexpr int kFirstChest = 150, kLastChest = 153;  // objTreasureChest .. objTreasureChestSmallFrozen
constexpr int kObjKeyChest = 150;
constexpr int kObjItemGet = 289;      // the reward the key chest makes: its `item` is what the player gets
constexpr double kKeyItem = 110;      // what objTreasureChest Other_11 counts and removes (ItemCount/RemoveItem)
constexpr uint32_t kScriptItemCount = 0x006AB090;
constexpr uint32_t kScriptRemoveItem = 0x006B6FC0;
constexpr uint32_t kScriptGainItem = 0x006AC690;
constexpr int kSlotUsable = 0x8CA;    // decompiled/code/var_slots.tsv: false once the chest has opened
constexpr int kSlotGrounded = 0x54D;  // true once the chest has stopped sliding off what it landed on
constexpr int kSlotItem = 0x5B8;
constexpr unsigned kEverySteps = 60;
constexpr double kSameSpot = 4;

#pragma pack(push, 1)
struct ChestMsg {
    int32_t hostId;  // -1 when the sender does not know it
    int32_t object;
    float x, y;
};
struct ChestMoveMsg {
    int32_t hostId;
    int32_t object;
    float fromX, fromY;  // where the guest's copy was spawned
    float x, y;
};
struct KeysMsg {
    ChestMsg chest;
    uint8_t have, need;
};
struct RewardMsg {
    ChestMsg chest;
    float item;
};
#pragma pack(pop)

yyc::EventFn g_orig[kOpenEventCount] = {};
bool (*g_isHost)() = nullptr;
unsigned g_steps = 0;
bool g_opening = false;                          // we run a chest's own opening: not news
std::set<int> g_keyGiven;                        // chests (local ids) this player put a key in
std::map<int, std::set<net::PeerId>> g_keys;     // host: chest -> the players whose key is in it
std::map<int, ChestMsg> g_keyChests;             // host: the same chests as the guests know them
// Host: where each treasure chest was when the guests last heard of it.
struct Spot {
    double x, y;
    bool sent;  // its resting place went out (or it was there from the start)
};
std::map<int, Spot> g_spots;

bool IsHost() { return g_isHost != nullptr && g_isHost(); }

double ReadNumber(int id, int slot) {
    yyc::RValue value{};
    const double number = yyc::GetVar(id, slot, &value) && yyc::IsNumber(value) ? yyc::ToReal(value) : 0;
    yyc::Free(&value);
    return number;
}

double CallReal(yyc::CInstance* self, const char* name, std::vector<double> values) {
    std::vector<yyc::RValue> args;
    for (const double v : values) args.push_back(yyc::Real(v));
    yyc::RValue result{};
    yyc::CallBuiltin(name, self, static_cast<int>(args.size()), args.data(), &result);
    const double number = yyc::ToReal(result);
    yyc::Free(&result);
    return number;
}

double CallScript(uint32_t va, yyc::CInstance* player, double arg) {
    yyc::RValue value = yyc::Real(arg);
    yyc::RValue* args[1] = {&value};
    yyc::RValue result{};
    yyc::At<yyc::ScriptFn>(va)(player, player, &result, 1, args);
    const double number = yyc::ToReal(result);
    yyc::Free(&result);
    return number;
}

ChestMsg Describe(yyc::CInstance* chest, int id) {
    return {sync::HostIdOf(id), static_cast<int32_t>(yyc::GetBuiltinReal(chest, yyc::va::YYVar_object_index)),
            static_cast<float>(yyc::GetBuiltinReal(chest, yyc::va::YYVar_x)),
            static_cast<float>(yyc::GetBuiltinReal(chest, yyc::va::YYVar_y))};
}

// kOpenEvents' index for a hooked chest kind (150, 151, 152; the frozen small chest is not hooked).
int EventIndex(int object) { return object == kObjKeyChest ? 0 : object == 151 ? 1 : 2; }

bool Opened(int id) { return yyc::FindInstance(id) == nullptr || ReadNumber(id, kSlotUsable) == 0; }

// The chest's own opening, run here for `player` (as if it pressed E).
void OpenHere(yyc::CInstance* chest, yyc::CInstance* player) {
    const int object = static_cast<int>(yyc::GetBuiltinReal(chest, yyc::va::YYVar_object_index));
    g_opening = true;
    g_orig[EventIndex(object)](chest, player);
    g_opening = false;
}

// The newest objItemGet (the one an opening just made: ids only grow), or 0.
int NewestReward(yyc::CInstance* self) {
    int newest = 0;
    const int count = static_cast<int>(CallReal(self, "instance_number", {double(kObjItemGet)}));
    for (int i = 0; i < count; ++i) {
        newest = (std::max)(newest, static_cast<int>(CallReal(self, "instance_find", {double(kObjItemGet), double(i)})));
    }
    return newest;
}

// The key chest's opening for `player`, and the reward it made (0 if none).
int OpenForReward(yyc::CInstance* chest, yyc::CInstance* player) {
    const int before = NewestReward(player);
    OpenHere(chest, player);
    const int after = NewestReward(player);
    return after > before ? after : 0;
}

void SayKeys(yyc::CInstance* player, int have, int need) {
    const std::string text = "Keys in the chest: " + std::to_string(have) + "/" + std::to_string(need);
    stations::Say(player, text.c_str());
}

// Host: every player's key is in - the host's chest opens with the game's own code (for that it gets back the key
// it put in), and the guests open theirs with the same reward.
void OpenForEveryone(yyc::CInstance* localPlayer, int chestId, const ChestMsg& msg) {
    yyc::CInstance* chest = yyc::FindInstance(chestId);
    g_keys.erase(chestId);
    g_keyChests.erase(chestId);
    g_keyGiven.erase(chestId);
    if (chest == nullptr) return;
    CallScript(kScriptGainItem, localPlayer, kKeyItem);
    const int reward = OpenForReward(chest, localPlayer);
    const RewardMsg out{msg, static_cast<float>(reward != 0 ? ReadNumber(reward, kSlotItem) : -1)};
    net::Send(kChestReward, &out, sizeof(out));
    Log("chests: every key is in chest %d at (%.0f, %.0f) - opened for everyone, reward item %.0f", msg.object, msg.x,
        msg.y, out.item);
}

// Host: `from` put a key in the chest.
void AddKey(yyc::CInstance* localPlayer, int chestId, const ChestMsg& msg, net::PeerId from) {
    std::set<net::PeerId>& keys = g_keys[chestId];
    keys.insert(from);
    g_keyChests[chestId] = msg;
    const int need = 1 + static_cast<int>(net::Peers().size());
    const int have = static_cast<int>(keys.size());
    const KeysMsg out{msg, static_cast<uint8_t>(have), static_cast<uint8_t>(need)};
    net::Send(kChestKeys, &out, sizeof(out));
    SayKeys(localPlayer, have, need);
    Log("chests: player %u put a key in chest %d at (%.0f, %.0f): %d/%d", from, msg.object, msg.x, msg.y, have, need);
    if (have >= need) OpenForEveryone(localPlayer, chestId, msg);
}

// A key chest: every player puts a key in, and then all get the same reward at once (playtest 5: with each player's
// own chest, a rejoin brought back the host's world, where the host had opened it). Without a session the game's own.
void OpenKeyChest(yyc::CInstance* self, yyc::CInstance* player, int id) {
    if (Opened(id)) return;
    if (g_keyGiven.count(id) > 0) {
        stations::Say(player, "Waiting for the other players' keys");
        return;
    }
    if (CallScript(kScriptItemCount, player, kKeyItem) < 1) {
        g_orig[0](self, player);  // the game says a key is needed
        return;
    }
    CallScript(kScriptRemoveItem, player, kKeyItem);
    g_keyGiven.insert(id);
    const ChestMsg msg = Describe(self, id);
    if (IsHost()) {
        AddKey(player, id, msg, net::kHostPeer);
    } else {
        net::Send(kChestKey, &msg, sizeof(msg));
        Log("chests: put a key in chest %d at (%.0f, %.0f)", msg.object, msg.x, msg.y);
    }
}

template <int N>
void __cdecl OpenHook(yyc::CInstance* self, yyc::CInstance* other) {
    if (!net::IsConnected() || self == nullptr || g_opening || Mutant("chests")) {
        g_orig[N](self, other);
        return;
    }
    const int id = static_cast<int>(yyc::GetBuiltinReal(self, yyc::va::YYVar_id));
    if (N == 0) {
        OpenKeyChest(self, other, id);
        return;
    }
    // Keyless: opened here, then at every other player too (each gets its own loot).
    const ChestMsg msg = Describe(self, id);
    const bool wasUsable = !Opened(id);
    g_orig[N](self, other);
    if (!wasUsable || !Opened(id)) return;
    net::Send(kChestOpened, &msg, sizeof(msg));
    Log("chests: opened chest %d at (%.0f, %.0f) here - it opens for everyone", msg.object, msg.x, msg.y);
}

constexpr yyc::EventFn kHooks[] = {&OpenHook<0>, &OpenHook<1>, &OpenHook<2>};
static_assert(sizeof(kHooks) / sizeof(kHooks[0]) == kOpenEventCount, "one hook per chest");

// The local copy of a chest a message is about (woken first if asleep), then `action` with it.
template <typename Action>
void WithChest(yyc::CInstance* self, const ChestMsg& msg, Action action, bool woken = false) {
    const int local = sync::Resolve(self, msg.hostId, msg.object, msg.x, msg.y);
    if (local == 0 && !woken) {
        sync::WakeThen(self, msg.object, [self, msg, action] { WithChest(self, msg, action, true); });
        return;
    }
    if (local == 0) {
        Log("chests: chest %d at (%.0f, %.0f) is not here", msg.object, msg.x, msg.y);
        return;
    }
    action(local);
}

// Calls `visit(instance, id)` for every active treasure chest.
template <typename Visit>
void ForEachChest(yyc::CInstance* self, Visit visit) {
    for (int object = kFirstChest; object <= kLastChest; ++object) {
        const int count = static_cast<int>(CallReal(self, "instance_number", {double(object)}));
        for (int i = 0; i < count; ++i) {
            const int id = static_cast<int>(CallReal(self, "instance_find", {double(object), double(i)}));
            if (yyc::CInstance* chest = yyc::FindInstance(id)) visit(chest, id);
        }
    }
}

// Host: a new chest slides off whatever it landed on (its Collision events) before it is `grounded`; the guests
// only heard where it was spawned, so they get where it came to rest (playtest 4, bug 2).
void SendRestingPlaces(yyc::CInstance* self) {
    std::map<int, Spot> seen;
    ForEachChest(self, [&seen](yyc::CInstance* chest, int id) {
        const double x = yyc::GetBuiltinReal(chest, yyc::va::YYVar_x);
        const double y = yyc::GetBuiltinReal(chest, yyc::va::YYVar_y);
        const auto known = g_spots.find(id);
        Spot spot = known != g_spots.end() ? known->second : Spot{x, y, false};
        if (!spot.sent && ReadNumber(id, kSlotGrounded) != 0) {
            if (std::hypot(x - spot.x, y - spot.y) > kSameSpot) {
                const ChestMoveMsg msg{id, static_cast<int32_t>(yyc::GetBuiltinReal(chest, yyc::va::YYVar_object_index)),
                                       static_cast<float>(spot.x), static_cast<float>(spot.y), static_cast<float>(x),
                                       static_cast<float>(y)};
                net::Send(kChestMoved, &msg, sizeof(msg));
                Log("chests: chest %d came to rest at (%.0f, %.0f), spawned at (%.0f, %.0f)", msg.object, x, y, spot.x,
                    spot.y);
            }
            spot = {x, y, true};
        }
        seen[id] = spot;
    });
    for (const auto& [id, spot] : g_spots) {
        if (seen.count(id) == 0 && yyc::FindInstance(id) != nullptr) seen[id] = spot;  // asleep, not gone
    }
    g_spots = std::move(seen);
}

void ApplyMove(yyc::CInstance* self, const ChestMoveMsg& msg) {
    const int local = sync::Resolve(self, msg.hostId, msg.object, msg.fromX, msg.fromY);
    yyc::CInstance* chest = local != 0 ? yyc::FindInstance(local) : nullptr;
    if (chest == nullptr) return;
    yyc::SetBuiltinReal(chest, yyc::va::YYVar_x, msg.x);
    yyc::SetBuiltinReal(chest, yyc::va::YYVar_y, msg.y);
    yyc::RValue grounded = yyc::Real(1);
    yyc::SetVar(local, kSlotGrounded, &grounded);
    Log("chests: chest %d moved to (%.0f, %.0f) as at the host", msg.object, msg.x, msg.y);
}

template <typename T>
bool Read(const net::Message& message, T* out) {
    if (message.payload.size() != sizeof(T)) return false;
    memcpy(out, message.payload.data(), sizeof(T));
    return true;
}

}  // namespace

bool IsTreasureChest(int object) { return object >= kFirstChest && object <= kLastChest; }

void Tick(yyc::CInstance* localPlayer) {
    if (!net::IsConnected() || ++g_steps % kEverySteps != 0) return;
    if (IsHost()) SendRestingPlaces(localPlayer);
}

void DebugTick(yyc::CInstance* localPlayer) {
    constexpr int kObjTreasureChest = 150;
    static const bool enabled = GetEnvironmentVariableA("COOP_DEBUG_CHEST", nullptr, 0) > 0;
    static unsigned steps = 0;
    if (!enabled || ++steps != 600) return;
    yyc::RValue args[3] = {yyc::Real(yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_x)),
                           yyc::Real(yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_y)), yyc::Real(kObjTreasureChest)};
    yyc::RValue result{};
    yyc::CallBuiltin("instance_nearest", localPlayer, 3, args, &result);
    yyc::CInstance* chest = yyc::FindInstance(static_cast<int>(yyc::ToReal(result)));
    yyc::Free(&result);
    if (chest == nullptr) {
        Log("chests: debug: no chest here");
        return;
    }
    yyc::PerformEvent(chest, localPlayer, yyc::kEvOther, yyc::kEvUser0 + 1);
}

bool InstallHooks(bool (*isHost)()) {
    g_isHost = isHost;
    bool ok = true;
    for (int i = 0; i < kOpenEventCount; ++i) {
        ok = MH_CreateHook(yyc::At<void*>(kOpenEvents[i]), reinterpret_cast<void*>(kHooks[i]),
                           reinterpret_cast<void**>(&g_orig[i])) == MH_OK &&
             ok;
    }
    if (!ok) Log("chests: hooks failed");
    return ok;
}

bool Handle(const net::Message& message, yyc::CInstance* localPlayer) {
    switch (message.type) {
        case kChestMoved: {
            ChestMoveMsg msg{};
            if (Read(message, &msg) && !IsHost()) ApplyMove(localPlayer, msg);
            return true;
        }
        case kChestOpened: {  // a keyless chest someone opened: ours opens too
            ChestMsg msg{};
            if (!Read(message, &msg)) return true;
            if (IsHost()) net::SendExcept(message.from, kChestOpened, &msg, sizeof(msg));
            WithChest(localPlayer, msg, [localPlayer, msg](int local) {
                if (Opened(local)) return;
                OpenHere(yyc::FindInstance(local), localPlayer);
                Log("chests: the other player opened chest %d at (%.0f, %.0f) - opened here too", msg.object, msg.x,
                    msg.y);
            });
            return true;
        }
        case kChestKey: {  // host: a guest's key
            ChestMsg msg{};
            if (!Read(message, &msg) || !IsHost()) return true;
            const net::PeerId from = message.from;
            WithChest(localPlayer, msg, [localPlayer, msg, from](int local) { AddKey(localPlayer, local, msg, from); });
            return true;
        }
        case kChestKeys: {
            KeysMsg msg{};
            if (Read(message, &msg)) SayKeys(localPlayer, msg.have, msg.need);
            return true;
        }
        case kChestReward: {  // every key is in: open ours, and its reward is the host's
            RewardMsg msg{};
            if (!Read(message, &msg)) return true;
            WithChest(localPlayer, msg.chest, [localPlayer, msg](int local) {
                g_keyGiven.erase(local);
                if (Opened(local)) return;
                CallScript(kScriptGainItem, localPlayer, kKeyItem);  // the key its opening takes
                const int reward = OpenForReward(yyc::FindInstance(local), localPlayer);
                if (reward != 0 && msg.item >= 0) {
                    yyc::RValue item = yyc::Real(msg.item);
                    yyc::SetVar(reward, kSlotItem, &item);
                }
                Log("chests: chest %d at (%.0f, %.0f) opened for everyone - reward item %.0f", msg.chest.object,
                    msg.chest.x, msg.chest.y, msg.item);
            });
            return true;
        }
        default:
            return false;
    }
}

void OnPeerLeft(yyc::CInstance* localPlayer, net::PeerId peer) {
    // Fewer players now need fewer keys: a chest that waited only for the one who left opens (and a key the host
    // put in is not stuck there when the last guest goes).
    const int need = 1 + static_cast<int>(net::Peers().size());
    std::vector<int> ready;
    for (auto& [chestId, keys] : g_keys) {
        keys.erase(peer);
        if (!keys.empty() && static_cast<int>(keys.size()) >= need) ready.push_back(chestId);
    }
    for (const int chestId : ready) {
        const ChestMsg msg = g_keyChests[chestId];
        Log("chests: player %u left - the keys in chest %d are enough now", peer, chestId);
        OpenForEveryone(localPlayer, chestId, msg);
    }
}

void Reset() {
    g_keyGiven.clear();
    g_keys.clear();
    g_keyChests.clear();
    g_spots.clear();
    g_steps = 0;
    g_opening = false;
}

}  // namespace coop::chests
