#include "chests.h"

#include <windows.h>

#include <cstring>

#include "MinHook.h"
#include "log.h"
#include "sync.h"

namespace coop::chests {

namespace {

constexpr uint8_t kChestOpened = 93;  // either way: ChestMsg
// The chests' Other_11 (opening: loot, animation; the chest destroys itself afterwards).
constexpr uint32_t kOpenEvents[] = {
    0x00C3CC20,  // objTreasureChest
    0x00C47C10,  // objTreasureChestSmall
    0x00C53AA0,  // objTreasureChestVoid
};
constexpr int kOpenEventCount = sizeof(kOpenEvents) / sizeof(kOpenEvents[0]);

#pragma pack(push, 1)
struct ChestMsg {
    int32_t hostId;  // -1 when the sender does not know it
    int32_t object;
    float x, y;
};
#pragma pack(pop)

yyc::EventFn g_orig[kOpenEventCount] = {};

template <int N>
void __cdecl OpenHook(yyc::CInstance* self, yyc::CInstance* other) {
    if (net::IsConnected() && self != nullptr) {
        const int id = static_cast<int>(yyc::GetBuiltinReal(self, yyc::va::YYVar_id));
        const ChestMsg msg{sync::HostIdOf(id), static_cast<int32_t>(yyc::GetBuiltinReal(self, yyc::va::YYVar_object_index)),
                           static_cast<float>(yyc::GetBuiltinReal(self, yyc::va::YYVar_x)),
                           static_cast<float>(yyc::GetBuiltinReal(self, yyc::va::YYVar_y))};
        net::Send(kChestOpened, &msg, sizeof(msg));
        Log("chests: opened chest %d at (%.0f, %.0f) here", msg.object, msg.x, msg.y);
    }
    g_orig[N](self, other);
}

constexpr yyc::EventFn kHooks[] = {&OpenHook<0>, &OpenHook<1>, &OpenHook<2>};
static_assert(sizeof(kHooks) / sizeof(kHooks[0]) == kOpenEventCount, "one hook per chest");

// `woken`: the second try, after waking the chest's kind up (off-screen chests are asleep).
void Remove(yyc::CInstance* self, const ChestMsg& msg, bool woken) {
    const int local = sync::Resolve(self, msg.hostId, msg.object, msg.x, msg.y);
    if (local == 0 && !woken) {
        sync::WakeThen(self, msg.object, [self, msg] { Remove(self, msg, true); });
        return;
    }
    if (local != 0) sync::RemoveLocal(self, local);
    Log("chests: the other player opened chest %d at (%.0f, %.0f)%s", msg.object, msg.x, msg.y,
        local != 0 ? "; removed here" : " - not here");
}

}  // namespace

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

bool InstallHooks() {
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
    if (message.type != kChestOpened) return false;
    if (message.payload.size() != sizeof(ChestMsg)) return true;
    ChestMsg msg{};
    memcpy(&msg, message.payload.data(), sizeof(msg));
    Remove(localPlayer, msg, false);
    return true;
}

}  // namespace coop::chests
