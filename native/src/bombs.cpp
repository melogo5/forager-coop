#include "bombs.h"

#include <cmath>
#include <cstring>
#include <unordered_set>

#include "MinHook.h"
#include "log.h"

namespace coop::bombs {

namespace {

constexpr uint8_t kBombPlaced = 35;  // either way: BombMsg, a bomb placed (the host passes a guest's on)
constexpr uint8_t kBombBlast = 36;   // either way: BombMsg, that bomb went off
constexpr int kObjBomb = 294;        // decompiled/data/objects.tsv
constexpr int kObjExplosion = 295;
constexpr double kSndBombExplode = 148;  // decompiled/data/sounds.tsv
constexpr double kMatchDistance = 12;    // the decoy and the owner's bomb sit on the same spot
constexpr char kPlacer[] = "gml_Script_UseBomb";
constexpr uint32_t kBombAlarm = 0x010314F0;      // objBomb Alarm_0: the fuse runs out -> instance_destroy
constexpr uint32_t kBombDestroy = 0x0102F1A0;    // objBomb Destroy_0: the blast (ExplosionCreate, damage)
constexpr uint32_t kExplosionStep = 0x01034190;  // objExplosion Step_0: hurts what it touches (once: dealtDmg)
constexpr int kSlotDealtDmg = 0x16D;
constexpr int kSlotDmg = 0x197;
constexpr int kSlotDmgPlayer = 0x198;

#pragma pack(push, 1)
struct BombMsg {
    float x, y, depth;
};
#pragma pack(pop)

std::unordered_set<int> g_decoys;       // bombs of other players: never go off here
std::unordered_set<int> g_harmless;     // blasts made for a decoy: their step (the damage) is skipped
int g_placing = 0;                      // inside our own instance_create of a decoy or a blast
yyc::EventFn g_origAlarm = nullptr;
yyc::EventFn g_origDestroy = nullptr;
yyc::EventFn g_origExplosionStep = nullptr;

int IdOf(yyc::CInstance* instance) { return static_cast<int>(yyc::GetBuiltinReal(instance, yyc::va::YYVar_id)); }

bool PlacedByPlayer() {
    for (const yyc::StackFrame* frame = yyc::TopFrame(); frame != nullptr; frame = frame->prev) {
        if (frame->name != nullptr && strcmp(frame->name, kPlacer) == 0) return true;
    }
    return false;
}

BombMsg Describe(yyc::CInstance* bomb) {
    return BombMsg{static_cast<float>(yyc::GetBuiltinReal(bomb, yyc::va::YYVar_x)),
                   static_cast<float>(yyc::GetBuiltinReal(bomb, yyc::va::YYVar_y)),
                   static_cast<float>(yyc::GetBuiltinReal(bomb, yyc::va::YYVar_depth))};
}

void __cdecl AlarmHook(yyc::CInstance* self, yyc::CInstance* other) {
    if (g_decoys.count(IdOf(self)) > 0) return;  // waits for the owner's blast
    g_origAlarm(self, other);
}

void __cdecl DestroyHook(yyc::CInstance* self, yyc::CInstance* other) {
    if (g_decoys.erase(IdOf(self)) > 0) return;  // a decoy set off here (another blast): the owner's decides
    if (net::IsConnected()) {
        const BombMsg msg = Describe(self);
        net::Send(kBombBlast, &msg, sizeof(msg));
    }
    g_origDestroy(self, other);
}

// The step also grows, fades and ends the blast: it runs, with the damage marked as dealt already.
void __cdecl ExplosionStepHook(yyc::CInstance* self, yyc::CInstance* other) {
    const int id = IdOf(self);
    if (g_harmless.count(id) > 0) {
        yyc::RValue dealt = yyc::Real(1), none = yyc::Real(0), none2 = yyc::Real(0);
        yyc::SetVar(id, kSlotDealtDmg, &dealt);
        yyc::SetVar(id, kSlotDmg, &none);
        yyc::SetVar(id, kSlotDmgPlayer, &none2);
    }
    g_origExplosionStep(self, other);
}

void Place(yyc::CInstance* self, const BombMsg& msg) {
    ++g_placing;
    const int id = static_cast<int>(yyc::CallReal(self, "instance_create_depth", {msg.x, msg.y, msg.depth, double(kObjBomb)}));
    --g_placing;
    if (yyc::FindInstance(id) != nullptr) g_decoys.insert(id);
    Log("bombs: the other player's bomb at (%.0f, %.0f) shown", msg.x, msg.y);
}

void Blast(yyc::CInstance* self, const BombMsg& msg) {
    for (auto it = g_decoys.begin(); it != g_decoys.end();) {
        yyc::CInstance* decoy = yyc::FindInstance(*it);
        if (decoy == nullptr) {
            it = g_decoys.erase(it);
            continue;
        }
        const double dx = yyc::GetBuiltinReal(decoy, yyc::va::YYVar_x) - msg.x;
        const double dy = yyc::GetBuiltinReal(decoy, yyc::va::YYVar_y) - msg.y;
        if (std::sqrt(dx * dx + dy * dy) > kMatchDistance) {
            ++it;
            continue;
        }
        yyc::CallReal(self, "instance_destroy", {double(*it), 0});  // no Destroy_0: that is the real blast
        it = g_decoys.erase(it);
    }
    ++g_placing;
    const int id =
        static_cast<int>(yyc::CallReal(self, "instance_create_depth", {msg.x, msg.y, msg.depth, double(kObjExplosion)}));
    --g_placing;
    if (yyc::FindInstance(id) != nullptr) g_harmless.insert(id);
    yyc::CallReal(self, "audio_play_sound", {kSndBombExplode, 1, 0});
    // Blasts end by themselves (Other_7): forget the ids that are gone.
    for (auto it = g_harmless.begin(); it != g_harmless.end();) {
        it = yyc::FindInstance(*it) == nullptr ? g_harmless.erase(it) : std::next(it);
    }
}

bool Hook(uint32_t va, void* detour, void* original) {
    return MH_CreateHook(yyc::At<void*>(va), detour, static_cast<void**>(original)) == MH_OK;
}

}  // namespace

bool InstallHooks() {
    const bool ok = Hook(kBombAlarm, reinterpret_cast<void*>(&AlarmHook), &g_origAlarm) &&
                    Hook(kBombDestroy, reinterpret_cast<void*>(&DestroyHook), &g_origDestroy) &&
                    Hook(kExplosionStep, reinterpret_cast<void*>(&ExplosionStepHook), &g_origExplosionStep);
    if (!ok) Log("bombs: hooks failed");
    return ok;
}

void OnCreated(yyc::CInstance* self, int createdId, int object) {
    if (object != kObjBomb || g_placing > 0 || !PlacedByPlayer()) return;
    yyc::CInstance* bomb = yyc::FindInstance(createdId);
    if (bomb == nullptr) return;
    const BombMsg msg = Describe(bomb);
    net::Send(kBombPlaced, &msg, sizeof(msg));
}

bool Handle(const net::Message& message, yyc::CInstance* localPlayer) {
    if (message.type != kBombPlaced && message.type != kBombBlast) return false;
    if (message.payload.size() != sizeof(BombMsg)) return true;
    BombMsg msg{};
    memcpy(&msg, message.payload.data(), sizeof(msg));
    if (!std::isfinite(msg.x) || !std::isfinite(msg.y) || !std::isfinite(msg.depth)) return true;
    if (message.type == kBombPlaced) {
        Place(localPlayer, msg);
    } else {
        Blast(localPlayer, msg);
    }
    return true;
}

void Reset() {
    g_decoys.clear();
    g_harmless.clear();
    g_placing = 0;
}

}  // namespace coop::bombs
