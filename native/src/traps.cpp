#include "traps.h"

#include <windows.h>

#include <cstring>

#include "MinHook.h"
#include "log.h"
#include "sync.h"

namespace coop::traps {

namespace {

constexpr uint8_t kTrap = 96;            // either way: TrapMsg
constexpr int kObjFishTrap = 112;        // decompiled/data/objects.tsv
constexpr int kSlotCaught = 0xF8;        // caught[0]: the item caught, -4 when empty; caught[10]: its pop-up timer
constexpr int kCaughtTimer = 10;
constexpr double kEmpty = -4;
constexpr uint32_t kScriptHandleFishTraps = 0x004B1970;  // a trap's catch roll (ObjectProcess, self = the trap)
constexpr uint32_t kTrapCollect = 0x00BC2B90;             // objFishTrap Other_11: DropItem(caught[0]), caught[0] = -4

#pragma pack(push, 1)
struct TrapMsg {
    float x, y;
    int32_t item;  // what the trap holds now; -4: emptied
};
#pragma pack(pop)

bool (*g_isHost)() = nullptr;
bool (*g_isGuest)() = nullptr;
yyc::ScriptFn g_origHandle = nullptr;
yyc::EventFn g_origCollect = nullptr;

bool InSession() { return net::IsConnected() && g_isHost != nullptr && g_isGuest != nullptr; }

int IdOf(yyc::CInstance* instance) { return static_cast<int>(yyc::GetBuiltinReal(instance, yyc::va::YYVar_id)); }

double Caught(int trap) {
    yyc::RValue value{};
    const double item = yyc::GetVarAt(trap, kSlotCaught, 0, &value) && yyc::IsNumber(value) ? yyc::ToReal(value) : kEmpty;
    yyc::Free(&value);
    return item;
}

// To everyone but `except` (a guest whose news it is).
void Send(yyc::CInstance* trap, double item, int except = -1) {
    const TrapMsg msg{static_cast<float>(yyc::GetBuiltinReal(trap, yyc::va::YYVar_x)),
                      static_cast<float>(yyc::GetBuiltinReal(trap, yyc::va::YYVar_y)), static_cast<int32_t>(item)};
    if (except >= 0) {
        net::SendExcept(static_cast<net::PeerId>(except), kTrap, &msg, sizeof(msg));
    } else {
        net::Send(kTrap, &msg, sizeof(msg));
    }
}

yyc::RValue* __cdecl HandleHook(yyc::CInstance* self, yyc::CInstance* other, yyc::RValue* result, int argc,
                                yyc::RValue** args) {
    if (!InSession() || self == nullptr) return g_origHandle(self, other, result, argc, args);
    if (g_isGuest()) return result;  // the host's traps catch; what they hold comes from there
    const int id = IdOf(self);
    const double before = Caught(id);
    yyc::RValue* r = g_origHandle(self, other, result, argc, args);
    const double after = Caught(id);
    if (after != before && yyc::FindInstance(id) != nullptr) Send(self, after);
    return r;
}

// Collecting: the catch drops for the collector as usual; the others' traps go empty without a drop.
void __cdecl CollectHook(yyc::CInstance* self, yyc::CInstance* other) {
    if (!InSession() || self == nullptr) {
        g_origCollect(self, other);
        return;
    }
    const int id = IdOf(self);
    const double before = Caught(id);
    g_origCollect(self, other);
    if (before == kEmpty || yyc::FindInstance(id) == nullptr || Caught(id) != kEmpty) return;
    Send(self, kEmpty);
    Log("traps: collected item %.0f from the trap at (%.0f, %.0f)", before, yyc::GetBuiltinReal(self, yyc::va::YYVar_x),
        yyc::GetBuiltinReal(self, yyc::va::YYVar_y));
}

// `woken`: the second try, after waking the traps up (off-screen ones are asleep).
void Apply(yyc::CInstance* localPlayer, const net::Message& message, const TrapMsg& msg, bool woken) {
    const int trap = sync::Resolve(localPlayer, -1, kObjFishTrap, msg.x, msg.y);
    if (trap == 0 && !woken) {
        sync::WakeThen(localPlayer, kObjFishTrap, [localPlayer, message, msg] { Apply(localPlayer, message, msg, true); });
        return;
    }
    if (trap == 0) {
        Log("traps: no trap at (%.0f, %.0f) here", msg.x, msg.y);
        return;
    }
    yyc::RValue item = yyc::Real(msg.item);
    yyc::SetVarAt(trap, kSlotCaught, 0, &item);
    if (msg.item != kEmpty) {
        yyc::RValue restart = yyc::Real(0);  // the "caught!" pop-up plays again
        yyc::SetVarAt(trap, kSlotCaught, kCaughtTimer, &restart);
    }
    // Host: a guest emptied its copy - the other guests' copies go empty too.
    yyc::CInstance* instance = yyc::FindInstance(trap);
    if (g_isHost() && message.from != net::kHostPeer && instance != nullptr) {
        Send(instance, msg.item, static_cast<int>(message.from));
    }
}

}  // namespace

bool InstallHooks(bool (*isHost)(), bool (*isGuest)()) {
    g_isHost = isHost;
    g_isGuest = isGuest;
    const bool ok = MH_CreateHook(yyc::At<void*>(kScriptHandleFishTraps), reinterpret_cast<void*>(&HandleHook),
                                  reinterpret_cast<void**>(&g_origHandle)) == MH_OK &&
                    MH_CreateHook(yyc::At<void*>(kTrapCollect), reinterpret_cast<void*>(&CollectHook),
                                  reinterpret_cast<void**>(&g_origCollect)) == MH_OK;
    if (!ok) Log("traps: hooks failed");
    return ok;
}

bool Handle(const net::Message& message, yyc::CInstance* localPlayer) {
    if (message.type != kTrap) return false;
    if (message.payload.size() != sizeof(TrapMsg)) return true;
    TrapMsg msg{};
    memcpy(&msg, message.payload.data(), sizeof(msg));
    Apply(localPlayer, message, msg, false);
    return true;
}

}  // namespace coop::traps
