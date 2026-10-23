#include "chests.h"

#include <windows.h>

#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "MinHook.h"
#include "character.h"
#include "log.h"
#include "mutant.h"
#include "sync.h"

namespace coop::chests {

namespace {

constexpr uint8_t kChestOpened = 93;  // guest -> host: the guest opened it; host -> guest: one it opened before
constexpr uint8_t kChestMoved = 97;   // host -> guest: ChestMoveMsg, where a new chest came to rest
// The chests' Other_11 (opening: loot, animation; the chest destroys itself afterwards).
constexpr uint32_t kOpenEvents[] = {
    0x00C3CC20,  // objTreasureChest
    0x00C47C10,  // objTreasureChestSmall
    0x00C53AA0,  // objTreasureChestVoid
};
constexpr int kOpenEventCount = sizeof(kOpenEvents) / sizeof(kOpenEvents[0]);
constexpr int kFirstChest = 150, kLastChest = 153;  // objTreasureChest .. objTreasureChestSmallFrozen
constexpr int kSlotUsable = 0x8CA;    // decompiled/code/var_slots.tsv: false once the chest has opened
constexpr int kSlotGrounded = 0x54D;  // true once the chest has stopped sliding off what it landed on
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
#pragma pack(pop)

yyc::EventFn g_orig[kOpenEventCount] = {};
bool (*g_isHost)() = nullptr;
unsigned g_steps = 0;
// Guest: the chests this player has opened (here or in an earlier session): they stay gone here.
std::vector<ChestMsg> g_openedHere;
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

ChestMsg Describe(yyc::CInstance* chest, int id) {
    return {sync::HostIdOf(id), static_cast<int32_t>(yyc::GetBuiltinReal(chest, yyc::va::YYVar_object_index)),
            static_cast<float>(yyc::GetBuiltinReal(chest, yyc::va::YYVar_x)),
            static_cast<float>(yyc::GetBuiltinReal(chest, yyc::va::YYVar_y))};
}

std::wstring OpenedFile(net::PeerId guest) { return character::GuestDataFile(character::HostGuestName(guest), ".chests"); }

// Each player opens every chest once (playtest 4, bug 9): the guest's opening stays the guest's - the host keeps
// its chest, and remembers the guest's for its next visit. Only a chest that really opens counts: one that needs
// a key the player lacks stays (PLAYTEST3.md #3).
template <int N>
void __cdecl OpenHook(yyc::CInstance* self, yyc::CInstance* other) {
    if (!net::IsConnected() || self == nullptr || IsHost()) {
        g_orig[N](self, other);
        return;
    }
    const int id = static_cast<int>(yyc::GetBuiltinReal(self, yyc::va::YYVar_id));
    const ChestMsg msg = Describe(self, id);
    const bool wasUsable = yyc::FindInstance(id) != nullptr && ReadNumber(id, kSlotUsable) != 0;
    g_orig[N](self, other);
    if (!wasUsable || (yyc::FindInstance(id) != nullptr && ReadNumber(id, kSlotUsable) != 0)) return;  // still shut
    g_openedHere.push_back(msg);
    net::Send(kChestOpened, &msg, sizeof(msg));
    Log("chests: opened chest %d at (%.0f, %.0f) here", msg.object, msg.x, msg.y);
}

constexpr yyc::EventFn kHooks[] = {&OpenHook<0>, &OpenHook<1>, &OpenHook<2>};
static_assert(sizeof(kHooks) / sizeof(kHooks[0]) == kOpenEventCount, "one hook per chest");

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

// Guest: chests this player opened before stay gone, also when the world snapshot or a spawn brings them back.
void RemoveOpened(yyc::CInstance* self) {
    if (g_openedHere.empty()) return;
    ForEachChest(self, [self](yyc::CInstance* chest, int id) {
        const ChestMsg here = Describe(chest, id);
        for (const ChestMsg& opened : g_openedHere) {
            if (opened.object != here.object || std::hypot(opened.x - here.x, opened.y - here.y) > kSameSpot) continue;
            sync::RemoveLocal(self, id);
            Log("chests: chest %d at (%.0f, %.0f) was opened by this player before - removed", here.object, here.x,
                here.y);
            return;
        }
    });
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

// Host: the guest opened a chest - kept on its file for the guest's next visit.
void RememberOpened(net::PeerId guest, const ChestMsg& msg) {
    std::ofstream(OpenedFile(guest), std::ios::binary | std::ios::app)
        .write(reinterpret_cast<const char*>(&msg), sizeof(msg));
    Log("chests: guest %u opened chest %d at (%.0f, %.0f); the host keeps its own", guest, msg.object, msg.x, msg.y);
}

}  // namespace

bool IsTreasureChest(int object) { return object >= kFirstChest && object <= kLastChest; }

void Tick(yyc::CInstance* localPlayer) {
    if (!net::IsConnected() || ++g_steps % kEverySteps != 0) return;
    if (IsHost()) {
        SendRestingPlaces(localPlayer);
    } else {
        RemoveOpened(localPlayer);
    }
}

void HostOnGuest(net::PeerId peer) {
    std::ifstream in(OpenedFile(peer), std::ios::binary);
    ChestMsg msg{};
    int sent = 0;
    while (in.read(reinterpret_cast<char*>(&msg), sizeof(msg))) {
        net::SendTo(peer, kChestOpened, &msg, sizeof(msg));
        ++sent;
    }
    if (sent > 0) Log("chests: guest %u had opened %d chests before", peer, sent);
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
    if (message.type == kChestMoved) {
        ChestMoveMsg msg{};
        if (message.payload.size() == sizeof(msg) && !IsHost()) {
            memcpy(&msg, message.payload.data(), sizeof(msg));
            ApplyMove(localPlayer, msg);
        }
        return true;
    }
    if (message.type != kChestOpened) return false;
    if (message.payload.size() != sizeof(ChestMsg)) return true;
    ChestMsg msg{};
    memcpy(&msg, message.payload.data(), sizeof(msg));
    if (IsHost() && Mutant("chests")) {  // the old rule: opened by anyone, gone for everyone
        const int local = sync::Resolve(localPlayer, msg.hostId, msg.object, msg.x, msg.y);
        if (local != 0) sync::RemoveLocal(localPlayer, local);
    } else if (IsHost()) {
        RememberOpened(message.from, msg);
    } else {
        g_openedHere.push_back(msg);  // removed by the next Tick, once the world is here
    }
    return true;
}

void Reset() {
    g_openedHere.clear();
    g_spots.clear();
    g_steps = 0;
}

}  // namespace coop::chests
