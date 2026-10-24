#include "traps.h"

#include <windows.h>

#include <cstring>

#include "MinHook.h"
#include "log.h"
#include "puppet.h"
#include "sync.h"

namespace coop::traps {

namespace {

constexpr uint8_t kTrap = 96;            // host -> guest: TrapMsg, what a trap holds now
constexpr uint8_t kTrapTake = 101;       // guest -> host: TrapMsg, "I collect this trap" (item: what it held here)
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

TrapMsg Describe(yyc::CInstance* trap, double item) {
    return {static_cast<float>(yyc::GetBuiltinReal(trap, yyc::va::YYVar_x)),
            static_cast<float>(yyc::GetBuiltinReal(trap, yyc::va::YYVar_y)), static_cast<int32_t>(item)};
}

// Host: what the trap holds now, to every guest.
void Send(yyc::CInstance* trap, double item) {
    const TrapMsg msg = Describe(trap, item);
    net::Send(kTrap, &msg, sizeof(msg));
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

// Collecting. The host decides who gets a catch: two players collecting the same trap within a ping both got it
// (REVIEW.md). The host collects as usual and tells the guests the trap is empty; a guest only asks the host, which
// collects it for that guest (the drop goes to the guest like any drop it earns) - or, if someone was quicker,
// just says it is empty.
void __cdecl CollectHook(yyc::CInstance* self, yyc::CInstance* other) {
    if (!InSession() || self == nullptr) {
        g_origCollect(self, other);
        return;
    }
    const int id = IdOf(self);
    const double before = Caught(id);
    if (g_isGuest()) {
        if (before == kEmpty) return;
        const TrapMsg msg = Describe(self, before);
        net::Send(kTrapTake, &msg, sizeof(msg));
        Log("traps: asked the host for item %.0f from the trap at (%.0f, %.0f)", before, msg.x, msg.y);
        return;
    }
    g_origCollect(self, other);
    if (before == kEmpty || yyc::FindInstance(id) == nullptr || Caught(id) != kEmpty) return;
    Send(self, kEmpty);
    Log("traps: collected item %.0f from the trap at (%.0f, %.0f)", before, yyc::GetBuiltinReal(self, yyc::va::YYVar_x),
        yyc::GetBuiltinReal(self, yyc::va::YYVar_y));
}

// `woken`: the second try, after waking the traps up (off-screen ones are asleep). 0 if there is none.
template <typename Then>
int FindTrap(yyc::CInstance* localPlayer, const TrapMsg& msg, bool woken, Then retry) {
    const int trap = sync::Resolve(localPlayer, -1, kObjFishTrap, msg.x, msg.y);
    if (trap == 0 && !woken) {
        sync::WakeThen(localPlayer, kObjFishTrap, retry);
    } else if (trap == 0) {
        Log("traps: no trap at (%.0f, %.0f) here", msg.x, msg.y);
    }
    return trap;
}

// Guest: what the host's trap holds now.
void Apply(yyc::CInstance* localPlayer, const TrapMsg& msg, bool woken) {
    const int trap = FindTrap(localPlayer, msg, woken, [localPlayer, msg] { Apply(localPlayer, msg, true); });
    if (trap == 0) return;
    yyc::RValue item = yyc::Real(msg.item);
    yyc::SetVarAt(trap, kSlotCaught, 0, &item);
    if (msg.item != kEmpty) {
        yyc::RValue restart = yyc::Real(0);  // the "caught!" pop-up plays again
        yyc::SetVarAt(trap, kSlotCaught, kCaughtTimer, &restart);
    }
}

// Host: guest `from` collects a trap - it gets the catch if the trap still holds one.
void Take(yyc::CInstance* localPlayer, net::PeerId from, const TrapMsg& msg, bool woken) {
    const int trap =
        FindTrap(localPlayer, msg, woken, [localPlayer, from, msg] { Take(localPlayer, from, msg, true); });
    yyc::CInstance* instance = trap != 0 ? yyc::FindInstance(trap) : nullptr;
    yyc::CInstance* collector = puppet::Instance(from);
    if (instance == nullptr || collector == nullptr) return;
    const double held = Caught(trap);
    if (held == kEmpty) {
        const TrapMsg empty = Describe(instance, kEmpty);
        net::SendTo(from, kTrap, &empty, sizeof(empty));
        Log("traps: guest %u came for the trap at (%.0f, %.0f), already empty", from, msg.x, msg.y);
        return;
    }
    puppet::BeginActing(from);  // its drop is the guest's: it goes straight into the guest's inventory
    g_origCollect(instance, collector);
    puppet::EndActing();
    if (yyc::FindInstance(trap) == nullptr || Caught(trap) != kEmpty) return;
    Send(instance, kEmpty);
    Log("traps: guest %u collected item %.0f from the trap at (%.0f, %.0f)", from, held, msg.x, msg.y);
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
    if (message.type != kTrap && message.type != kTrapTake) return false;
    if (message.payload.size() != sizeof(TrapMsg)) return true;
    TrapMsg msg{};
    memcpy(&msg, message.payload.data(), sizeof(msg));
    if (message.type == kTrap && g_isGuest()) Apply(localPlayer, msg, false);
    if (message.type == kTrapTake && g_isHost()) Take(localPlayer, message.from, msg, false);
    return true;
}

}  // namespace coop::traps
