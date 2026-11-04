#include "shots.h"

#include <cmath>
#include <cstring>
#include <unordered_set>
#include <vector>

#include "MinHook.h"
#include "log.h"
#include "puppet.h"

namespace coop::shots {

namespace {

constexpr uint8_t kShot = 58;  // either way (the host passes a guest's on): ShotMsg
constexpr int kObjArrow = 303, kObjFireball = 301;  // decompiled/data/objects.tsv
// decompiled/code/yyvar_map.tsv
constexpr uint32_t kYYVarDirection = 0x01AD87A0, kYYVarSpeed = 0x01AD8C10, kYYVarImageAngle = 0x01AD8728;

// What a copy must not do (decompiled/code/index.tsv): hit (the collision events deal the damage) and, for an arrow,
// Destroy_0 (it drops the arrow item where it lands).
constexpr uint32_t kHarmful[] = {
    0x01052DB0, 0x01055320, 0x01057130, 0x010573A0,  // objArrow Collision_80, _23, _271, _64
    0x01051960,                                      // objArrow Destroy_0
    0x01049800, 0x0104A020, 0x0104B5A0,              // objFireball Collision_80, _20, _271
};
constexpr size_t kHookCount = sizeof(kHarmful) / sizeof(kHarmful[0]);

#pragma pack(push, 1)
struct ShotMsg {
    int32_t object;
    float x, y, depth;
    float direction, speed, angle;
    float xscale, yscale;
    float sprite;
};
#pragma pack(pop)

std::vector<int> g_fired;              // ours, made since the last step
std::unordered_set<int> g_copies;      // other players' shots shown here
int g_showing = 0;                     // inside our own instance_create of a copy
yyc::EventFn g_orig[kHookCount] = {};

int IdOf(yyc::CInstance* instance) { return static_cast<int>(yyc::GetBuiltinReal(instance, yyc::va::YYVar_id)); }

bool IsShot(int object) { return object == kObjArrow || object == kObjFireball; }

// The local player fired it: made in its own step or Use script (an enemy's fireball is made by the enemy).
bool FiredByLocalPlayer(yyc::CInstance* self) {
    return self != nullptr && static_cast<int>(yyc::GetBuiltinReal(self, yyc::va::YYVar_object_index)) ==
                                  yyc::obj::player &&
           !puppet::IsPuppet(self);
}

template <size_t N>
void __cdecl HarmfulHook(yyc::CInstance* self, yyc::CInstance* other) {
    if (!g_copies.empty() && g_copies.count(IdOf(self)) > 0) return;
    g_orig[N](self, other);
}

template <size_t... N>
struct HookTable {
    static constexpr yyc::EventFn kHooks[] = {&HarmfulHook<N>...};
};
using Hooks = HookTable<0, 1, 2, 3, 4, 5, 6, 7>;
static_assert(sizeof(Hooks::kHooks) / sizeof(Hooks::kHooks[0]) == kHookCount, "one hook per harmful event");

ShotMsg Describe(yyc::CInstance* shot, int object) {
    auto get = [shot](uint32_t var) { return static_cast<float>(yyc::GetBuiltinReal(shot, var)); };
    return ShotMsg{object,
                   get(yyc::va::YYVar_x),
                   get(yyc::va::YYVar_y),
                   get(yyc::va::YYVar_depth),
                   get(kYYVarDirection),
                   get(kYYVarSpeed),
                   get(kYYVarImageAngle),
                   get(yyc::va::YYVar_image_xscale),
                   get(yyc::va::YYVar_image_yscale),
                   get(yyc::va::YYVar_sprite_index)};
}

// A shot from the network: finite, and nothing the drawing could fail on (a sprite index past the last sprite) or
// that would swamp the screen.
bool IsSane(const ShotMsg& m) {
    for (const float v : {m.x, m.y, m.depth, m.direction, m.speed, m.angle, m.xscale, m.yscale, m.sprite}) {
        if (!std::isfinite(v)) return false;
    }
    constexpr float kSpriteCount = 1388, kMaxSpeed = 64, kMaxScale = 8;  // decompiled/data/sprites.tsv
    return m.sprite < kSpriteCount && std::fabs(m.speed) <= kMaxSpeed && std::fabs(m.xscale) <= kMaxScale &&
           std::fabs(m.yscale) <= kMaxScale;
}

void Show(yyc::CInstance* self, const ShotMsg& msg) {
    ++g_showing;
    const int id = static_cast<int>(
        yyc::CallReal(self, "instance_create_depth", {msg.x, msg.y, msg.depth, double(msg.object)}));
    --g_showing;
    yyc::CInstance* copy = yyc::FindInstance(id);
    if (copy == nullptr) return;
    g_copies.insert(id);
    // Its Create aimed it from our player: it flies as the shooter's did.
    yyc::SetBuiltinReal(copy, kYYVarDirection, msg.direction);
    yyc::SetBuiltinReal(copy, kYYVarSpeed, msg.speed);
    yyc::SetBuiltinReal(copy, kYYVarImageAngle, msg.angle);
    yyc::SetBuiltinReal(copy, yyc::va::YYVar_image_xscale, msg.xscale);
    yyc::SetBuiltinReal(copy, yyc::va::YYVar_image_yscale, msg.yscale);
    if (msg.sprite >= 0) yyc::SetBuiltinReal(copy, yyc::va::YYVar_sprite_index, msg.sprite);
    // Shots end by themselves (lifetime, outside the room): forget the ids that are gone.
    for (auto it = g_copies.begin(); it != g_copies.end();) {
        it = yyc::FindInstance(*it) == nullptr ? g_copies.erase(it) : std::next(it);
    }
}

}  // namespace

bool InstallHooks() {
    bool ok = true;
    for (size_t i = 0; i < kHookCount; ++i) {
        ok = MH_CreateHook(yyc::At<void*>(kHarmful[i]), reinterpret_cast<void*>(Hooks::kHooks[i]),
                           reinterpret_cast<void**>(&g_orig[i])) == MH_OK && ok;
    }
    if (!ok) Log("shots: hooks failed");
    return ok;
}

void OnCreated(yyc::CInstance* self, int createdId, int object) {
    if (!IsShot(object) || g_showing > 0 || !net::IsConnected() || !FiredByLocalPlayer(self)) return;
    g_fired.push_back(createdId);
}

void Tick() {
    if (g_fired.empty()) return;
    for (const int id : g_fired) {
        yyc::CInstance* shot = yyc::FindInstance(id);
        if (shot == nullptr) continue;  // gone within its first step (hit at point-blank range)
        const int object = static_cast<int>(yyc::GetBuiltinReal(shot, yyc::va::YYVar_object_index));
        if (!IsShot(object)) continue;
        const ShotMsg msg = Describe(shot, object);
        net::Send(kShot, &msg, sizeof(msg));
    }
    g_fired.clear();
}

bool Handle(const net::Message& message, yyc::CInstance* localPlayer) {
    if (message.type != kShot) return false;
    if (message.payload.size() != sizeof(ShotMsg)) return true;
    ShotMsg msg{};
    memcpy(&msg, message.payload.data(), sizeof(msg));
    if (!IsShot(msg.object) || !IsSane(msg)) return true;
    Show(localPlayer, msg);
    return true;
}

void Reset() {
    g_fired.clear();
    g_copies.clear();
    g_showing = 0;
}

}  // namespace coop::shots
