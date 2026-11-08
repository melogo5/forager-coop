#include "shots.h"

#include <cmath>
#include <cstring>
#include <unordered_set>
#include <vector>

#include "MinHook.h"
#include "bombs.h"
#include "log.h"
#include "puppet.h"

namespace coop::shots {

namespace {

constexpr uint8_t kShot = 58;   // either way (the host passes a guest's on): ShotMsg
constexpr uint8_t kBlast = 59;  // either way: BlastMsg, a fireball went off (ExplosionCreate in its collision)
constexpr int kObjArrow = 303, kObjFireball = 301;  // decompiled/data/objects.tsv
// The ice rod's shot: the others saw no shot, only what it hit freeze (puzzles.cpp shares the frozen chests'
// `freeze`). Enemies' ice bolts (the ice wizard's) are theirs, not a player's: FiredByLocalPlayer keeps them out.
constexpr int kObjIceBolt = 308;
// decompiled/code/yyvar_map.tsv
constexpr uint32_t kYYVarDirection = 0x01AD87A0, kYYVarSpeed = 0x01AD8C10, kYYVarImageAngle = 0x01AD8728;
// objFireball.owner: UseSword sets it to the player; Create_0 leaves noone. Its Step_0 starts with
// `if (!instance_exists(owner)) { instance_destroy(); exit }` - a copy (no owner) was gone at its first step, so the
// sword's fireballs were never seen by the other players.
constexpr int kSlotFireballOwner = 0x6CA;
// ExplosionCreate(x, y, range, dmgPlayer) makes the objExplosion of a fireball's hit (its collision events call it;
// bombs make theirs themselves, bombs.cpp) and returns nothing. A copy hits nothing, so the other players saw no blast.
constexpr uint32_t kScriptExplosionCreate = 0x0086D1E0;
constexpr int kBlastArgs = 4;
constexpr int kObjExplosion = 295;   // decompiled/data/objects.tsv
constexpr int kSlotRange = 0x744;    // objExplosion.range: ExplosionCreate's third argument
constexpr float kBlastMatchDistance = 48;  // a copy flies a little behind its fireball: the one that went off

// What a copy must not do (decompiled/code/index.tsv): hit (the collision events deal the damage) and, for an arrow,
// Destroy_0 (it drops the arrow item where it lands).
constexpr uint32_t kHarmful[] = {
    0x01052DB0, 0x01055320, 0x01057130, 0x010573A0,  // objArrow Collision_80, _23, _271, _64
    0x01051960,                                      // objArrow Destroy_0
    0x01049800, 0x0104A020, 0x0104B5A0,              // objFireball Collision_80, _20, _271
    0x01067D50, 0x01069130, 0x01069890, 0x01069930,  // objIceBolt's five collisions (they freeze and hurt)
    0x0106A2C0,
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
struct BlastMsg {
    float args[kBlastArgs];  // ExplosionCreate's: x, y, range, dmgPlayer
    float depth;
};
#pragma pack(pop)

std::vector<int> g_fired;              // ours, made since the last step
std::unordered_set<int> g_copies;      // other players' shots shown here
int g_showing = 0;                     // inside our own instance_create of a copy
yyc::EventFn g_orig[kHookCount] = {};
yyc::ScriptFn g_origExplosionCreate = nullptr;

int IdOf(yyc::CInstance* instance) { return static_cast<int>(yyc::GetBuiltinReal(instance, yyc::va::YYVar_id)); }

bool IsShot(int object) { return object == kObjArrow || object == kObjFireball || object == kObjIceBolt; }

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
using Hooks = HookTable<0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12>;
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

void Show(yyc::CInstance* self, net::PeerId from, const ShotMsg& msg) {
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
    if (msg.object == kObjFireball) {
        // The shooter's puppet owns it (a guest hears every shot from the host: then the host's); any player does.
        yyc::CInstance* shooter = puppet::Instance(from);
        yyc::RValue owner = yyc::Real(IdOf(shooter != nullptr ? shooter : self));
        yyc::SetVar(id, kSlotFireballOwner, &owner);
    }
    // Shots end by themselves (lifetime, outside the room): forget the ids that are gone.
    for (auto it = g_copies.begin(); it != g_copies.end();) {
        it = yyc::FindInstance(*it) == nullptr ? g_copies.erase(it) : std::next(it);
    }
}

// One of our own fireballs went off: the others see its blast too (and lose their copy of it there).
yyc::RValue* __cdecl ExplosionCreateHook(yyc::CInstance* self, yyc::CInstance* other, yyc::RValue* result, int argc,
                                         yyc::RValue** args) {
    yyc::RValue* r = g_origExplosionCreate(self, other, result, argc, args);
    if (self == nullptr || !net::IsConnected() || argc < 2 || args == nullptr) return r;
    if (static_cast<int>(yyc::GetBuiltinReal(self, yyc::va::YYVar_object_index)) != kObjFireball) return r;
    if (g_copies.count(IdOf(self)) > 0) return r;  // a copy never collides; to be safe all the same
    BlastMsg msg{};
    for (int i = 0; i < kBlastArgs; ++i) {
        const yyc::RValue* a = i < argc ? args[i] : nullptr;
        msg.args[i] = a != nullptr && yyc::IsNumber(*a) ? static_cast<float>(yyc::ToReal(*a)) : 0.f;
    }
    msg.depth = static_cast<float>(yyc::GetBuiltinReal(self, yyc::va::YYVar_depth));
    net::Send(kBlast, &msg, sizeof(msg));
    return r;
}

// Another player's fireball went off: its copy here (the nearest) goes, and the same blast shows - harmless: it is
// the shooter's blast that breaks and hurts, at the shooter's side (bombs.cpp makes it so).
void ShowBlast(yyc::CInstance* self, const BlastMsg& msg) {
    const float x = msg.args[0], y = msg.args[1];
    int nearest = 0;
    double best = double(kBlastMatchDistance) * kBlastMatchDistance;
    for (auto it = g_copies.begin(); it != g_copies.end();) {
        yyc::CInstance* copy = yyc::FindInstance(*it);
        if (copy == nullptr) {
            it = g_copies.erase(it);
            continue;
        }
        const double dx = yyc::GetBuiltinReal(copy, yyc::va::YYVar_x) - x;
        const double dy = yyc::GetBuiltinReal(copy, yyc::va::YYVar_y) - y;
        if (static_cast<int>(yyc::GetBuiltinReal(copy, yyc::va::YYVar_object_index)) == kObjFireball &&
            dx * dx + dy * dy <= best) {
            best = dx * dx + dy * dy;
            nearest = *it;
        }
        ++it;
    }
    if (nearest != 0) {
        g_copies.erase(nearest);
        yyc::CallReal(self, "instance_destroy", {double(nearest), 0});
    }
    // Made as ExplosionCreate makes it, but here: ExplosionCreate returns nothing, so its blast could not be told
    // apart and made harmless - the copy hurt objects, mobs and players a second time (review of PR #4). As
    // bombs::Blast does: the explosion directly, its range set, harmless before its first step.
    ++g_showing;
    const int blast = static_cast<int>(
        yyc::CallReal(self, "instance_create_depth", {double(x), double(y), double(msg.depth), double(kObjExplosion)}));
    --g_showing;
    if (yyc::FindInstance(blast) == nullptr) return;
    bombs::MakeHarmless(blast);
    yyc::RValue range = yyc::Real(msg.args[2]);
    yyc::SetVar(blast, kSlotRange, &range);  // dmgPlayer is not needed: the harmless step zeroes dmg and dmgPlayer
}

}  // namespace

bool InstallHooks() {
    bool ok = true;
    for (size_t i = 0; i < kHookCount; ++i) {
        ok = MH_CreateHook(yyc::At<void*>(kHarmful[i]), reinterpret_cast<void*>(Hooks::kHooks[i]),
                           reinterpret_cast<void**>(&g_orig[i])) == MH_OK && ok;
    }
    ok = MH_CreateHook(yyc::At<void*>(kScriptExplosionCreate), reinterpret_cast<void*>(&ExplosionCreateHook),
                       reinterpret_cast<void**>(&g_origExplosionCreate)) == MH_OK && ok;
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
    if (message.type == kBlast) {
        if (message.payload.size() != sizeof(BlastMsg)) return true;
        BlastMsg msg{};
        memcpy(&msg, message.payload.data(), sizeof(msg));
        for (const float v : msg.args) {
            if (!std::isfinite(v)) return true;
        }
        if (!std::isfinite(msg.depth)) return true;
        ShowBlast(localPlayer, msg);
        return true;
    }
    if (message.type != kShot) return false;
    if (message.payload.size() != sizeof(ShotMsg)) return true;
    ShotMsg msg{};
    memcpy(&msg, message.payload.data(), sizeof(msg));
    if (!IsShot(msg.object) || !IsSane(msg)) return true;
    Show(localPlayer, message.from, msg);
    return true;
}

void Reset() {
    g_fired.clear();
    g_copies.clear();
    g_showing = 0;
}

}  // namespace coop::shots
