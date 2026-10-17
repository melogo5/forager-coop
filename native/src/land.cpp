#include "land.h"

#include <windows.h>

#include <cstring>

#include "MinHook.h"
#include "log.h"
#include "sync.h"

namespace coop::land {

namespace {

constexpr uint8_t kLand = 50;
constexpr uint32_t kScriptSpawnLand = 0x00763750;  // SpawnLand(biome, x, y)
constexpr int kObjPurchaseLand = 229;              // the sign standing on an island for sale
constexpr int kNoOne = -4;
constexpr char kPurchaseFrame[] = "gml_Object_objPurchaseLand_Step_0";

struct StackFrame {
    StackFrame* prev;
    const char* name;
    int line;
};

#pragma pack(push, 1)
struct LandMsg {
    double biome, x, y;
    double seed;  // random_set_seed before SpawnLand on both sides: the same island shape
};
#pragma pack(pop)

constexpr uint32_t kPurchaseSignStep = 0x00E1F0F0;  // objPurchaseLand Step_0: buying happens here

yyc::ScriptFn g_origSpawnLand = nullptr;
yyc::EventFn g_origSignStep = nullptr;
bool (*g_isHost)() = nullptr;
bool (*g_isGuest)() = nullptr;
int g_applying = 0;

bool CalledFromPurchase() {
    const StackFrame* top = *yyc::At<StackFrame**>(yyc::va::StackTraceTop);
    return top != nullptr && top->name != nullptr && strcmp(top->name, kPurchaseFrame) == 0;
}

double CallReal(yyc::CInstance* self, const char* builtin, std::initializer_list<double> values);

// Host: a purchase is generated from a fresh seed that the guest then reuses.
yyc::RValue* __cdecl SpawnLandHook(yyc::CInstance* self, yyc::CInstance* other, yyc::RValue* result, int argc,
                                   yyc::RValue** args) {
    const bool share = g_applying == 0 && argc >= 3 && net::IsConnected() && g_isHost != nullptr && g_isHost() &&
                       CalledFromPurchase();
    if (!share) return g_origSpawnLand(self, other, result, argc, args);
    const LandMsg msg{yyc::ToReal(*args[0]), yyc::ToReal(*args[1]), yyc::ToReal(*args[2]),
                      static_cast<double>(GetTickCount() % 1000000000)};
    CallReal(self, "random_set_seed", {msg.seed});
    yyc::RValue* r = g_origSpawnLand(self, other, result, argc, args);
    CallReal(self, "randomize", {});
    net::Send(kLand, &msg, sizeof(msg));
    Log("land: bought island at (%.0f, %.0f), biome %.0f", msg.x, msg.y, msg.biome);
    return r;
}

// Only the host buys islands: on the guest the sale signs don't react.
void __cdecl SignStepHook(yyc::CInstance* self, yyc::CInstance* other) {
    if (g_isGuest != nullptr && g_isGuest() && net::IsConnected()) return;
    g_origSignStep(self, other);
}

double CallReal(yyc::CInstance* self, const char* builtin, std::initializer_list<double> values) {
    yyc::RValue args[4] = {};
    int argc = 0;
    for (const double v : values) args[argc++] = yyc::Real(v);
    yyc::RValue result{};
    yyc::CallBuiltin(builtin, self, argc, args, &result);
    const double value = yyc::ToReal(result);
    yyc::Free(&result);
    return value;
}

void Apply(yyc::CInstance* localPlayer, const LandMsg& msg) {
    // The sign for that island, if it is still here: SpawnLand runs in its context, then it goes.
    const int signId = static_cast<int>(CallReal(localPlayer, "instance_nearest", {msg.x, msg.y, kObjPurchaseLand}));
    yyc::CInstance* sign = signId == kNoOne ? nullptr : yyc::FindInstance(signId);
    yyc::CInstance* self = sign != nullptr ? sign : localPlayer;
    yyc::RValue values[3] = {yyc::Real(msg.biome), yyc::Real(msg.x), yyc::Real(msg.y)};
    yyc::RValue* args[3] = {&values[0], &values[1], &values[2]};
    yyc::RValue result{};
    ++g_applying;
    CallReal(localPlayer, "random_set_seed", {msg.seed});
    sync::BeginSuppressSpawns();  // resources/enemies come from the host, not from our own roll
    g_origSpawnLand(self, self, &result, 3, args);
    sync::EndSuppressSpawns(localPlayer);
    CallReal(localPlayer, "randomize", {});
    --g_applying;
    yyc::Free(&result);
    if (sign != nullptr) CallReal(localPlayer, "instance_destroy", {double(signId)});
    Log("land: the peer bought the island at (%.0f, %.0f)", msg.x, msg.y);
}

}  // namespace

// Testing aid: COOP_DEBUG_BUYLAND=1 buys the island of the nearest sign ~7 s into the world.
void DebugTick(yyc::CInstance* localPlayer) {
    static const bool enabled = GetEnvironmentVariableA("COOP_DEBUG_BUYLAND", nullptr, 0) > 0;
    static unsigned steps = 0;
    constexpr unsigned kAtStep = 420;
    constexpr int kSlotBiome = 0xB4;
    if (!enabled || ++steps != kAtStep) return;
    const double px = yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_x);
    const double py = yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_y);
    const int signId = static_cast<int>(CallReal(localPlayer, "instance_nearest", {px, py, kObjPurchaseLand}));
    yyc::CInstance* sign = signId == kNoOne ? nullptr : yyc::FindInstance(signId);
    if (sign == nullptr) return;
    yyc::RValue biome{};
    yyc::GetVar(signId, kSlotBiome, &biome);
    const LandMsg msg{yyc::ToReal(biome), yyc::GetBuiltinReal(sign, yyc::va::YYVar_x),
                      yyc::GetBuiltinReal(sign, yyc::va::YYVar_y), 0};
    yyc::Free(&biome);
    ++g_applying;  // reported below, not by the hook
    yyc::RValue values[3] = {yyc::Real(msg.biome), yyc::Real(msg.x), yyc::Real(msg.y)};
    yyc::RValue* args[3] = {&values[0], &values[1], &values[2]};
    yyc::RValue result{};
    g_origSpawnLand(sign, sign, &result, 3, args);
    yyc::Free(&result);
    --g_applying;
    CallReal(localPlayer, "instance_destroy", {double(signId)});
    net::Send(kLand, &msg, sizeof(msg));
    Log("land: debug bought the island at (%.0f, %.0f), biome %.0f", msg.x, msg.y, msg.biome);
}

bool InstallHooks(bool (*isHost)(), bool (*isGuest)()) {
    g_isHost = isHost;
    g_isGuest = isGuest;
    const bool ok = MH_CreateHook(yyc::At<void*>(kScriptSpawnLand), reinterpret_cast<void*>(&SpawnLandHook),
                                  reinterpret_cast<void**>(&g_origSpawnLand)) == MH_OK &&
                    MH_CreateHook(yyc::At<void*>(kPurchaseSignStep), reinterpret_cast<void*>(&SignStepHook),
                                  reinterpret_cast<void**>(&g_origSignStep)) == MH_OK;
    if (!ok) Log("land: hooks failed");
    return ok;
}

bool Handle(const net::Message& message, yyc::CInstance* localPlayer) {
    if (message.type != kLand) return false;
    if (message.payload.size() == sizeof(LandMsg)) {
        LandMsg msg{};
        memcpy(&msg, message.payload.data(), sizeof(msg));
        Apply(localPlayer, msg);
    }
    return true;
}

}  // namespace coop::land
