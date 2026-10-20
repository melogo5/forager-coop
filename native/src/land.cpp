#include "land.h"

#include <windows.h>

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "MinHook.h"
#include "log.h"
#include "puppet.h"
#include "sync.h"

namespace coop::land {

namespace {

constexpr uint8_t kLand = 50;      // host -> guest: the host bought an island
constexpr uint8_t kLandfill = 52;  // either way: LandfillMsg
constexpr uint32_t kScriptSpawnLand = 0x00763750;  // SpawnLand(x, y, biome): x, y of the sale sign
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
    double x, y, biome;  // SpawnLand's arguments, in order
    double seed;  // random_set_seed before SpawnLand on both sides: the same island shape
};
#pragma pack(pop)

constexpr uint32_t kPurchaseSignStep = 0x00E1F0F0;  // objPurchaseLand Step_0: buying happens here

yyc::ScriptFn g_origSpawnLand = nullptr;

// Landfill turns water into land (PlaceLandfill) and the obliterator land into water (RemoveLandfill): tiles,
// not objects, so the peer repeats the call. It runs as the puppet there, so the energy it costs (EnergyDmg on
// PlayerFind()) is not taken from the peer's own player.
constexpr uint32_t kScriptPlaceLandfill = 0x007758C0;   // PlaceLandfill(x, y)
constexpr uint32_t kScriptRemoveLandfill = 0x00777B00;  // RemoveLandfill(x, y)
constexpr uint32_t kScriptWaterCollision = 0x004EF950;  // WaterCollision(x, y): water there
yyc::ScriptFn g_origPlaceLandfill = nullptr;
yyc::ScriptFn g_origRemoveLandfill = nullptr;

struct LandfillMsg {
    double x, y;
    double remove;  // 1: RemoveLandfill
};
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

// The sale sign standing exactly at (x, y), or nullptr (the buyer's own sign removes itself).
yyc::CInstance* SignAt(yyc::CInstance* localPlayer, double x, double y, int* id) {
    *id = static_cast<int>(CallReal(localPlayer, "instance_nearest", {x, y, kObjPurchaseLand}));
    yyc::CInstance* sign = *id == kNoOne ? nullptr : yyc::FindInstance(*id);
    if (sign == nullptr || yyc::GetBuiltinReal(sign, yyc::va::YYVar_x) != x ||
        yyc::GetBuiltinReal(sign, yyc::va::YYVar_y) != y) {
        return nullptr;
    }
    return sign;
}

void Apply(yyc::CInstance* localPlayer, const LandMsg& msg) {
    // The sign for that island, if it is still here: SpawnLand runs in its context, then it goes.
    int signId = kNoOne;
    yyc::CInstance* sign = SignAt(localPlayer, msg.x, msg.y, &signId);
    yyc::CInstance* self = sign != nullptr ? sign : localPlayer;
    yyc::RValue values[3] = {yyc::Real(msg.x), yyc::Real(msg.y), yyc::Real(msg.biome)};
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

// Host: builds the island at (x, y) from a fresh seed and sends it to the guest with that seed (testing aid).
void BuildAndShare(yyc::CInstance* localPlayer, LandMsg msg, const char* buyer) {
    int signId = kNoOne;
    yyc::CInstance* sign = SignAt(localPlayer, msg.x, msg.y, &signId);
    yyc::CInstance* self = sign != nullptr ? sign : localPlayer;
    msg.seed = static_cast<double>(GetTickCount() % 1000000000);
    yyc::RValue values[3] = {yyc::Real(msg.x), yyc::Real(msg.y), yyc::Real(msg.biome)};
    yyc::RValue* args[3] = {&values[0], &values[1], &values[2]};
    yyc::RValue result{};
    ++g_applying;
    CallReal(localPlayer, "random_set_seed", {msg.seed});
    g_origSpawnLand(self, self, &result, 3, args);
    CallReal(localPlayer, "randomize", {});
    --g_applying;
    yyc::Free(&result);
    if (sign != nullptr) CallReal(localPlayer, "instance_destroy", {double(signId)});
    net::Send(kLand, &msg, sizeof(msg));
    Log("land: %s bought the island at (%.0f, %.0f), biome %.0f", buyer, msg.x, msg.y, msg.biome);
}

}  // namespace

yyc::RValue* ShareLandfill(yyc::ScriptFn original, bool remove, yyc::CInstance* self, yyc::CInstance* other,
                           yyc::RValue* result, int argc, yyc::RValue** args) {
    yyc::RValue* r = original(self, other, result, argc, args);
    if (g_applying == 0 && argc >= 2 && net::IsConnected()) {
        const LandfillMsg msg{yyc::ToReal(*args[0]), yyc::ToReal(*args[1]), remove ? 1.0 : 0.0};
        net::Send(kLandfill, &msg, sizeof(msg));
        Log("land: %s at (%.0f, %.0f) sent", remove ? "removed land" : "landfill", msg.x, msg.y);
    }
    return r;
}

yyc::RValue* __cdecl PlaceLandfillHook(yyc::CInstance* self, yyc::CInstance* other, yyc::RValue* result, int argc,
                                       yyc::RValue** args) {
    return ShareLandfill(g_origPlaceLandfill, false, self, other, result, argc, args);
}

yyc::RValue* __cdecl RemoveLandfillHook(yyc::CInstance* self, yyc::CInstance* other, yyc::RValue* result, int argc,
                                        yyc::RValue** args) {
    return ShareLandfill(g_origRemoveLandfill, true, self, other, result, argc, args);
}

bool IsWater(yyc::CInstance* self, double x, double y) {
    yyc::RValue values[2] = {yyc::Real(x), yyc::Real(y)};
    yyc::RValue* args[2] = {&values[0], &values[1]};
    yyc::RValue result{};
    yyc::At<yyc::ScriptFn>(kScriptWaterCollision)(self, self, &result, 2, args);
    const bool water = yyc::ToReal(result) != 0;
    yyc::Free(&result);
    return water;
}

void ApplyLandfill(yyc::CInstance* localPlayer, net::PeerId from, const LandfillMsg& msg) {
    yyc::RValue values[2] = {yyc::Real(msg.x), yyc::Real(msg.y)};
    yyc::RValue* args[2] = {&values[0], &values[1]};
    yyc::RValue result{};
    ++g_applying;
    puppet::BeginActing(from);
    (msg.remove != 0 ? g_origRemoveLandfill : g_origPlaceLandfill)(localPlayer, localPlayer, &result, 2, args);
    puppet::EndActing();
    --g_applying;
    yyc::Free(&result);
    Log("land: the peer's %s at (%.0f, %.0f) applied; water there now: %d", msg.remove != 0 ? "removed land" : "landfill",
        msg.x, msg.y, IsWater(localPlayer, msg.x, msg.y));
}


// Testing aid: COOP_DEBUG_LANDFILL=1 fills the nearest water tile (16 px grid, up to 10 tiles away) ~5 s in.
void DebugLandfill(yyc::CInstance* localPlayer) {
    static const bool enabled = GetEnvironmentVariableA("COOP_DEBUG_LANDFILL", nullptr, 0) > 0;
    static unsigned steps = 0;
    if (!enabled || ++steps != 300) return;
    const double px = yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_x);
    const double py = yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_y);
    for (int r = 1; r <= 10; ++r) {
        for (int dx = -r; dx <= r; ++dx) {
            for (int dy = -r; dy <= r; ++dy) {
                if (std::abs(dx) != r && std::abs(dy) != r) continue;
                const double x = std::floor(px / 16) * 16 + 8 + dx * 16;
                const double y = std::floor(py / 16) * 16 + 8 + dy * 16;
                if (!IsWater(localPlayer, x, y)) continue;
                yyc::RValue values[2] = {yyc::Real(x), yyc::Real(y)};
                yyc::RValue* args[2] = {&values[0], &values[1]};
                yyc::RValue result{};
                PlaceLandfillHook(localPlayer, localPlayer, &result, 2, args);
                yyc::Free(&result);
                Log("land: debug landfill at (%.0f, %.0f); water there now: %d", x, y, IsWater(localPlayer, x, y));
                return;
            }
        }
    }
    Log("land: debug landfill: no water nearby");
}

// Only the host buys islands (docs/coop/QUESTIONS.md): on the guest the sale signs don't react.
void __cdecl SignStepHook(yyc::CInstance* self, yyc::CInstance* other) {
    if (g_isGuest != nullptr && g_isGuest() && net::IsConnected()) return;
    g_origSignStep(self, other);
}

void DebugBuy(yyc::CInstance* localPlayer);

// Testing aid: COOP_DEBUG_BUYLAND=1 (host) buys the island of the nearest sale sign ~7 s into the world;
// =<seconds> does it that late. Sale signs exist only while the BUY LAND tab of the player menu is open.
void DebugTick(yyc::CInstance* localPlayer) {
    DebugLandfill(localPlayer);
    static const unsigned atStep = [] {
        char value[16] = {};
        if (GetEnvironmentVariableA("COOP_DEBUG_BUYLAND", value, sizeof(value)) == 0) return 0u;
        const int seconds = atoi(value);
        return seconds > 1 ? static_cast<unsigned>(seconds) * 60 : 420u;
    }();
    static unsigned steps = 0;
    if (atStep == 0 || g_isHost == nullptr || !g_isHost() || ++steps != atStep) return;
    sync::WakeThen(localPlayer, kObjPurchaseLand, [localPlayer] { DebugBuy(localPlayer); });
}

void DebugBuy(yyc::CInstance* localPlayer) {
    constexpr int kSlotBiome = 0xB4;
    const double px = yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_x);
    const double py = yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_y);
    const int signId = static_cast<int>(CallReal(localPlayer, "instance_nearest", {px, py, kObjPurchaseLand}));
    yyc::CInstance* sign = signId == kNoOne ? nullptr : yyc::FindInstance(signId);
    if (sign == nullptr) {
        Log("land: debug purchase: no sale sign found");
        return;
    }
    yyc::RValue biome{};
    yyc::GetVar(signId, kSlotBiome, &biome);
    const LandMsg msg{yyc::GetBuiltinReal(sign, yyc::va::YYVar_x), yyc::GetBuiltinReal(sign, yyc::va::YYVar_y),
                      yyc::ToReal(biome), 0};
    yyc::Free(&biome);
    BuildAndShare(localPlayer, msg, "debug");
}

bool InstallHooks(bool (*isHost)(), bool (*isGuest)()) {
    g_isHost = isHost;
    g_isGuest = isGuest;
    const bool ok = MH_CreateHook(yyc::At<void*>(kScriptSpawnLand), reinterpret_cast<void*>(&SpawnLandHook),
                                  reinterpret_cast<void**>(&g_origSpawnLand)) == MH_OK &&
                    MH_CreateHook(yyc::At<void*>(kPurchaseSignStep), reinterpret_cast<void*>(&SignStepHook),
                                  reinterpret_cast<void**>(&g_origSignStep)) == MH_OK &&
                    MH_CreateHook(yyc::At<void*>(kScriptPlaceLandfill), reinterpret_cast<void*>(&PlaceLandfillHook),
                                  reinterpret_cast<void**>(&g_origPlaceLandfill)) == MH_OK &&
                    MH_CreateHook(yyc::At<void*>(kScriptRemoveLandfill), reinterpret_cast<void*>(&RemoveLandfillHook),
                                  reinterpret_cast<void**>(&g_origRemoveLandfill)) == MH_OK;
    if (!ok) Log("land: hooks failed");
    return ok;
}

bool Handle(const net::Message& message, yyc::CInstance* localPlayer) {
    if (message.type == kLandfill) {
        if (message.payload.size() == sizeof(LandfillMsg)) {
            LandfillMsg msg{};
            memcpy(&msg, message.payload.data(), sizeof(msg));
            ApplyLandfill(localPlayer, message.from, msg);
        }
        return true;
    }
    if (message.type != kLand) return false;
    if (message.payload.size() != sizeof(LandMsg)) return true;
    LandMsg msg{};
    memcpy(&msg, message.payload.data(), sizeof(msg));
    // The sale sign may be off-screen, i.e. deactivated: wake the signs up first.
    sync::WakeThen(localPlayer, kObjPurchaseLand, [localPlayer, msg] { Apply(localPlayer, msg); });
    return true;
}

}  // namespace coop::land
