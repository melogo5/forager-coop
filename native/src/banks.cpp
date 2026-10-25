#include "banks.h"

#include <windows.h>

#include <cstring>

#include "MinHook.h"
#include "log.h"
#include "puppet.h"
#include "sync.h"

namespace coop::banks {

namespace {

constexpr uint8_t kBank = 102;      // host -> guest: BankMsg, how many coins a bank holds now
constexpr uint8_t kBankTake = 103;  // guest -> host: BankMsg, "I collect this bank" (coins: what it held here)
constexpr int kObjBank = 125;       // decompiled/data/objects.tsv
constexpr int kSlotCrafting = 0x146;  // crafting[1]: the coins the bank holds (var_slots.tsv)
constexpr int kCoinsIndex = 1;
constexpr uint32_t kScriptHandleBanks = 0x004B3310;  // a bank's earning (ObjectProcess, self = the bank)
constexpr uint32_t kBankCollect = 0x00BFC860;        // objBank Other_11: DropItem(coins, crafting[1]), then 0

#pragma pack(push, 1)
struct BankMsg {
    float x, y;
    int32_t coins;  // what the bank holds now
};
#pragma pack(pop)

bool (*g_isHost)() = nullptr;
bool (*g_isGuest)() = nullptr;
yyc::ScriptFn g_origHandle = nullptr;
yyc::EventFn g_origCollect = nullptr;

bool InSession() { return net::IsConnected() && g_isHost != nullptr && g_isGuest != nullptr; }

int IdOf(yyc::CInstance* instance) { return static_cast<int>(yyc::GetBuiltinReal(instance, yyc::va::YYVar_id)); }

double Coins(int bank) {
    yyc::RValue value{};
    const double coins =
        yyc::GetVarAt(bank, kSlotCrafting, kCoinsIndex, &value) && yyc::IsNumber(value) ? yyc::ToReal(value) : 0;
    yyc::Free(&value);
    return coins;
}

BankMsg Describe(yyc::CInstance* bank, double coins) {
    return {static_cast<float>(yyc::GetBuiltinReal(bank, yyc::va::YYVar_x)),
            static_cast<float>(yyc::GetBuiltinReal(bank, yyc::va::YYVar_y)), static_cast<int32_t>(coins)};
}

// Host: what the bank holds now, to every guest.
void Send(yyc::CInstance* bank, double coins) {
    const BankMsg msg = Describe(bank, coins);
    net::Send(kBank, &msg, sizeof(msg));
}

yyc::RValue* __cdecl HandleHook(yyc::CInstance* self, yyc::CInstance* other, yyc::RValue* result, int argc,
                                yyc::RValue** args) {
    if (!InSession() || self == nullptr) return g_origHandle(self, other, result, argc, args);
    if (g_isGuest()) return result;  // the host's banks earn; what they hold comes from there
    const int id = IdOf(self);
    const double before = Coins(id);
    yyc::RValue* r = g_origHandle(self, other, result, argc, args);
    const double after = Coins(id);
    if (after != before && yyc::FindInstance(id) != nullptr) Send(self, after);
    return r;
}

// The host collects as usual and tells the guests the bank is empty; a guest only asks the host (traps.cpp).
void __cdecl CollectHook(yyc::CInstance* self, yyc::CInstance* other) {
    if (!InSession() || self == nullptr) {
        g_origCollect(self, other);
        return;
    }
    const int id = IdOf(self);
    const double before = Coins(id);
    if (g_isGuest()) {
        if (before <= 0) return;
        const BankMsg msg = Describe(self, before);
        net::Send(kBankTake, &msg, sizeof(msg));
        Log("banks: asked the host for %.0f coins from the bank at (%.0f, %.0f)", before, msg.x, msg.y);
        return;
    }
    g_origCollect(self, other);
    if (before <= 0 || yyc::FindInstance(id) == nullptr || Coins(id) > 0) return;
    Send(self, 0);
    Log("banks: collected %.0f coins from the bank at (%.0f, %.0f)", before,
        yyc::GetBuiltinReal(self, yyc::va::YYVar_x), yyc::GetBuiltinReal(self, yyc::va::YYVar_y));
}

// `woken`: the second try, after waking the banks up (off-screen ones are asleep). 0 if there is none.
template <typename Then>
int FindBank(yyc::CInstance* localPlayer, const BankMsg& msg, bool woken, Then retry) {
    const int bank = sync::Resolve(localPlayer, -1, kObjBank, msg.x, msg.y);
    if (bank == 0 && !woken) {
        sync::WakeThen(localPlayer, kObjBank, retry);
    } else if (bank == 0) {
        Log("banks: no bank at (%.0f, %.0f) here", msg.x, msg.y);
    }
    return bank;
}

// Guest: what the host's bank holds now. Its Alarm_0 writes the number over the bank from crafting[1].
void Apply(yyc::CInstance* localPlayer, const BankMsg& msg, bool woken) {
    const int bank = FindBank(localPlayer, msg, woken, [localPlayer, msg] { Apply(localPlayer, msg, true); });
    yyc::CInstance* instance = bank != 0 ? yyc::FindInstance(bank) : nullptr;
    if (instance == nullptr) return;
    yyc::RValue coins = yyc::Real(msg.coins);
    yyc::SetVarAt(bank, kSlotCrafting, kCoinsIndex, &coins);
    yyc::SetBuiltinRealAt(instance, yyc::va::YYVar_alarm, 0, 1);
}

// Host: guest `from` collects a bank - it gets the coins if the bank still holds some.
void Take(yyc::CInstance* localPlayer, net::PeerId from, const BankMsg& msg, bool woken) {
    const int bank =
        FindBank(localPlayer, msg, woken, [localPlayer, from, msg] { Take(localPlayer, from, msg, true); });
    yyc::CInstance* instance = bank != 0 ? yyc::FindInstance(bank) : nullptr;
    yyc::CInstance* collector = puppet::Instance(from);
    if (instance == nullptr || collector == nullptr) return;
    const double held = Coins(bank);
    if (held <= 0) {
        const BankMsg empty = Describe(instance, 0);
        net::SendTo(from, kBank, &empty, sizeof(empty));
        Log("banks: guest %u came for the bank at (%.0f, %.0f), already empty", from, msg.x, msg.y);
        return;
    }
    puppet::BeginActing(from);  // its drop is the guest's: it goes straight into the guest's inventory
    g_origCollect(instance, collector);
    puppet::EndActing();
    if (yyc::FindInstance(bank) == nullptr || Coins(bank) > 0) return;
    Send(instance, 0);
    Log("banks: guest %u collected %.0f coins from the bank at (%.0f, %.0f)", from, held, msg.x, msg.y);
}

}  // namespace

bool InstallHooks(bool (*isHost)(), bool (*isGuest)()) {
    g_isHost = isHost;
    g_isGuest = isGuest;
    const bool ok = MH_CreateHook(yyc::At<void*>(kScriptHandleBanks), reinterpret_cast<void*>(&HandleHook),
                                  reinterpret_cast<void**>(&g_origHandle)) == MH_OK &&
                    MH_CreateHook(yyc::At<void*>(kBankCollect), reinterpret_cast<void*>(&CollectHook),
                                  reinterpret_cast<void**>(&g_origCollect)) == MH_OK;
    if (!ok) Log("banks: hooks failed");
    return ok;
}

bool Handle(const net::Message& message, yyc::CInstance* localPlayer) {
    if (message.type != kBank && message.type != kBankTake) return false;
    if (message.payload.size() != sizeof(BankMsg)) return true;
    BankMsg msg{};
    memcpy(&msg, message.payload.data(), sizeof(msg));
    if (message.type == kBank && g_isGuest()) Apply(localPlayer, msg, false);
    if (message.type == kBankTake && g_isHost()) Take(localPlayer, message.from, msg, false);
    return true;
}

}  // namespace coop::banks
