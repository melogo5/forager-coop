#include "land.h"

#include <windows.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "MinHook.h"
#include "log.h"
#include "puppet.h"
#include "sync.h"

namespace coop::land {

namespace {

constexpr uint8_t kLand = 50;      // host -> guest: the host bought an island
constexpr uint8_t kLandfill = 52;  // either way: LandfillMsg
constexpr uint8_t kDirt = 53;      // either way: x, y (f64), then CreateDirt's third argument (see SendDirt)
constexpr uint32_t kScriptSpawnLand = 0x00763750;  // SpawnLand(x, y, biome): x, y of the sale sign
constexpr int kObjPurchaseLand = 229;              // the sign standing on an island for sale
constexpr int kNoOne = yyc::kNoOne;
constexpr char kPurchaseFrame[] = "gml_Object_objPurchaseLand_Step_0";

using yyc::StackFrame;

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
constexpr uint32_t kScriptWaterCollision = yyc::script::WaterCollision;
yyc::ScriptFn g_origPlaceLandfill = nullptr;
yyc::ScriptFn g_origRemoveLandfill = nullptr;

struct LandfillMsg {
    double x, y;
    double remove;  // 1: RemoveLandfill
};
// Dug (shovel) and watered (bottled water) ground is dirt: a sprite on a layer and an entry in the dirt list,
// not an object, so the peer repeats CreateDirt (PLAYTEST3.md #5). Dirt read from the save is not news.
constexpr uint32_t kScriptCreateDirt = 0x0077C0E0;  // CreateDirt(x, y, kind)
constexpr const char* kDirtCallers[] = {"gml_Script_UseShovel", "gml_Script_UseBottledWater"};
yyc::ScriptFn g_origCreateDirt = nullptr;
yyc::EventFn g_origSignStep = nullptr;
bool (*g_isHost)() = nullptr;
bool (*g_isGuest)() = nullptr;
int g_applying = 0;

bool CalledFromPurchase() {
    const StackFrame* top = yyc::TopFrame();
    return top != nullptr && top->name != nullptr && strcmp(top->name, kPurchaseFrame) == 0;
}

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

using yyc::CallReal;

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

bool DugByPlayer() {
    const StackFrame* top = yyc::TopFrame();
    if (top == nullptr || top->name == nullptr) return false;
    for (const char* caller : kDirtCallers) {
        if (strcmp(top->name, caller) == 0) return true;
    }
    return false;
}

// Payload: x, y (f64), u8 kind of the third argument (0 none, 1 number, 2 string), then f64 or the string's bytes.
void SendDirt(int argc, yyc::RValue** args) {
    std::vector<uint8_t> out(2 * sizeof(double));
    const double xy[2] = {yyc::ToReal(*args[0]), yyc::ToReal(*args[1])};
    memcpy(out.data(), xy, sizeof(xy));
    if (argc < 3) {
        out.push_back(0);
    } else if (yyc::IsNumber(*args[2])) {
        out.push_back(1);
        const double v = yyc::ToReal(*args[2]);
        const auto* bytes = reinterpret_cast<const uint8_t*>(&v);
        out.insert(out.end(), bytes, bytes + sizeof(v));
    } else {
        out.push_back(2);
        const std::string text = yyc::ToString(*args[2]);
        out.insert(out.end(), text.begin(), text.end());
    }
    net::Send(kDirt, out.data(), out.size());
}

yyc::RValue* __cdecl CreateDirtHook(yyc::CInstance* self, yyc::CInstance* other, yyc::RValue* result, int argc,
                                    yyc::RValue** args) {
    const bool share = g_applying == 0 && argc >= 2 && net::IsConnected() && DugByPlayer();
    yyc::RValue* r = g_origCreateDirt(self, other, result, argc, args);
    if (share) SendDirt(argc, args);
    return r;
}

void ApplyDirt(yyc::CInstance* localPlayer, net::PeerId from, const std::vector<uint8_t>& p) {
    if (p.size() < 2 * sizeof(double) + 1) return;
    double xy[2] = {};
    memcpy(xy, p.data(), sizeof(xy));
    constexpr size_t kMaxDirtText = 64;
    if (!std::isfinite(xy[0]) || !std::isfinite(xy[1]) || p.size() > 2 * sizeof(double) + 1 + kMaxDirtText) return;
    const uint8_t kind = p[2 * sizeof(double)];
    const size_t at = 2 * sizeof(double) + 1;
    yyc::RValue values[3] = {yyc::Real(xy[0]), yyc::Real(xy[1]), {}};
    int argc = 2;
    if (kind == 1 && p.size() == at + sizeof(double)) {
        double v = 0;
        memcpy(&v, p.data() + at, sizeof(v));
        values[2] = yyc::Real(v);
        argc = 3;
    } else if (kind == 2) {
        values[2] = yyc::String(std::string(p.begin() + at, p.end()).c_str());
        argc = 3;
    }
    yyc::RValue* args[3] = {&values[0], &values[1], &values[2]};
    yyc::RValue result{};
    ++g_applying;
    puppet::BeginActing(from);
    g_origCreateDirt(localPlayer, localPlayer, &result, argc, args);
    puppet::EndActing();
    --g_applying;
    yyc::Free(&result);
    if (argc == 3) yyc::Free(&values[2]);
    Log("land: the peer's dirt at (%.0f, %.0f) applied", xy[0], xy[1]);
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
                                  reinterpret_cast<void**>(&g_origRemoveLandfill)) == MH_OK &&
                    MH_CreateHook(yyc::At<void*>(kScriptCreateDirt), reinterpret_cast<void*>(&CreateDirtHook),
                                  reinterpret_cast<void**>(&g_origCreateDirt)) == MH_OK;
    if (!ok) Log("land: hooks failed");
    return ok;
}

bool Handle(const net::Message& message, yyc::CInstance* localPlayer) {
    if (message.type == kDirt) {
        ApplyDirt(localPlayer, message.from, message.payload);
        return true;
    }
    if (message.type == kLandfill) {
        if (message.payload.size() == sizeof(LandfillMsg)) {
            LandfillMsg msg{};
            memcpy(&msg, message.payload.data(), sizeof(msg));
            if (std::isfinite(msg.x) && std::isfinite(msg.y)) ApplyLandfill(localPlayer, message.from, msg);
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
