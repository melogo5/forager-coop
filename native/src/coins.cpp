#include "coins.h"

#include <windows.h>

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "log.h"

namespace coop::coins {

namespace {

enum MessageType : uint8_t {
    kCoinsTotal = 80,   // host -> guest: double, the shared coins
    kCoinsChange = 81,  // guest -> host: double, what the guest earned (+) or spent (-)
};
constexpr int kSlotCoins = 0x10C;  // the player's `coins` (GainItem: PlayerFind().coins; var_slots.tsv)
constexpr unsigned kEverySteps = 30;

unsigned g_steps = 0;
bool g_known = false;  // guest: g_lastSeen holds the value the host last set
double g_lastSeen = 0;

int PlayerId(yyc::CInstance* player) { return static_cast<int>(yyc::GetBuiltinReal(player, yyc::va::YYVar_id)); }

bool ReadCoins(yyc::CInstance* player, double* value) {
    yyc::RValue coins{};
    if (!yyc::GetVar(PlayerId(player), kSlotCoins, &coins)) return false;
    const bool number = yyc::IsNumber(coins);
    if (number) *value = yyc::ToReal(coins);
    yyc::Free(&coins);
    return number;
}

void WriteCoins(yyc::CInstance* player, double value) {
    yyc::RValue coins = yyc::Real(value);
    yyc::SetVar(PlayerId(player), kSlotCoins, &coins);
}

// Testing aid: COOP_DEBUG_COINS=<amount> adds that many coins to this side ~5 s in, as if earned.
void DebugEarn(yyc::CInstance* player) {
    static const double amount = [] {
        char value[16] = {};
        return GetEnvironmentVariableA("COOP_DEBUG_COINS", value, sizeof(value)) > 0 ? atof(value) : 0.0;
    }();
    static unsigned steps = 0;
    double coins = 0;
    if (amount == 0 || ++steps != 300 || !ReadCoins(player, &coins)) return;
    WriteCoins(player, coins + amount);
    Log("coins: debug earned %.2f", amount);
}

}  // namespace

void HostTick(yyc::CInstance* localPlayer) {
    DebugEarn(localPlayer);
    if (++g_steps % kEverySteps != 0) return;
    double total = 0;
    if (ReadCoins(localPlayer, &total)) net::Send(kCoinsTotal, &total, sizeof(total));
}

void GuestTick(yyc::CInstance* localPlayer) {
    DebugEarn(localPlayer);
    double now = 0;
    if (!g_known || !ReadCoins(localPlayer, &now) || now == g_lastSeen) return;
    const double change = now - g_lastSeen;
    g_lastSeen = now;
    net::Send(kCoinsChange, &change, sizeof(change));
}

bool Handle(const net::Message& message, yyc::CInstance* localPlayer) {
    if (message.type != kCoinsTotal && message.type != kCoinsChange) return false;
    double value = 0;
    if (message.payload.size() != sizeof(value)) return true;
    memcpy(&value, message.payload.data(), sizeof(value));
    constexpr double kMostCoins = 1e9;
    if (!std::isfinite(value) || std::fabs(value) > kMostCoins) return true;  // would spoil the shared coins and the save
    double current = 0;
    if (!ReadCoins(localPlayer, &current)) return true;
    if (message.type == kCoinsChange) {  // host
        WriteCoins(localPlayer, current + value);
        Log("coins: the guest %s %.2f coins", value >= 0 ? "earned" : "spent", value >= 0 ? value : -value);
    } else {  // guest: what the host has is what there is
        if (!g_known) Log("coins: shared coins %.2f", value);
        WriteCoins(localPlayer, value);
        g_lastSeen = value;
        g_known = true;
    }
    return true;
}

void Reset() {
    g_steps = 0;
    g_known = false;
    g_lastSeen = 0;
}

}  // namespace coop::coins
