#include "fishing.h"

#include <windows.h>

#include <cmath>
#include <map>

#include "MinHook.h"
#include "log.h"

namespace coop::fishing {

namespace {

constexpr int kObjBobber = 159;  // decompiled/data/objects.tsv
namespace ev {                   // decompiled/code/index.tsv
constexpr uint32_t Create = 0x00C7B1F0;
constexpr uint32_t Step2 = 0x00C7BE90;   // end step: cast, bite, timeout, reel
constexpr uint32_t Reel = 0x00C81A90;    // Other_10: bobberCastState = 1 (reel in)
constexpr uint32_t Collect = 0x00C81CA0;  // Other_11: DropItem(caught), instance_destroy
constexpr uint32_t Miss = 0x00C83230;    // Other_12: caught = -4, new timers
}  // namespace ev
// decompiled/code/var_slots.tsv
constexpr int kSlotCastState = 0xBD, kSlotCastTimer = 0xBE, kSlotLanded = 0xBF, kSlotWater = 0xC2;
constexpr int kSlotCatchTime = 0xF2, kSlotCatchTimeout = 0xF3, kSlotCatchTimer = 0xF4, kSlotCaught = 0xF8;
constexpr int kSlotMiss = 0x646;
constexpr unsigned kHeartbeatSteps = 180;  // ~3 s: timers of every bobber, changed or not
constexpr unsigned kTickEverySteps = 10;

using yyc::StackFrame;

struct Seen {
    int castState, water, caught, miss;  // what a change is noticed by
    unsigned steps;                      // its end steps since the last line
    bool active;
};

bool (*g_inSession)() = nullptr;
yyc::EventFn g_origCreate = nullptr, g_origStep2 = nullptr, g_origReel = nullptr, g_origCollect = nullptr,
             g_origMiss = nullptr;
std::map<int, Seen> g_bobbers;  // by instance id
unsigned g_steps = 0;

bool On() { return g_inSession != nullptr && g_inSession(); }

const char* Caller() {
    const StackFrame* top = yyc::TopFrame();
    return top != nullptr && top->name != nullptr ? top->name : "?";
}

int IdOf(yyc::CInstance* self) { return static_cast<int>(yyc::GetBuiltinReal(self, yyc::va::YYVar_id)); }

double Var(int id, int slot) {
    yyc::RValue value{};
    const bool number = yyc::GetVar(id, slot, &value) && yyc::IsNumber(value);
    const double v = number ? yyc::ToReal(value) : -99;
    yyc::Free(&value);
    return v;
}

void LogState(const char* why, int id, yyc::CInstance* localPlayer) {
    yyc::CInstance* bobber = yyc::FindInstance(id);
    if (bobber == nullptr) return;
    const double x = yyc::GetBuiltinReal(bobber, yyc::va::YYVar_x), y = yyc::GetBuiltinReal(bobber, yyc::va::YYVar_y);
    double distance = -1;
    if (localPlayer != nullptr) {
        distance = std::hypot(x - yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_x),
                              y - yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_y));
    }
    const Seen& seen = g_bobbers[id];
    Log("fishing: %s bobber %d at (%.0f, %.0f), %.0f px from us: cast %g timer %g landed %g water %g | caught %g "
        "catch %g/%g timeout %g miss %g | %u end steps",
        why, id, x, y, distance, Var(id, kSlotCastState), Var(id, kSlotCastTimer), Var(id, kSlotLanded),
        Var(id, kSlotWater), Var(id, kSlotCaught), Var(id, kSlotCatchTimer), Var(id, kSlotCatchTime),
        Var(id, kSlotCatchTimeout), Var(id, kSlotMiss), seen.steps);
}

void __cdecl CreateHook(yyc::CInstance* self, yyc::CInstance* other) {
    const char* caller = Caller();
    g_origCreate(self, other);
    if (!On() || self == nullptr) return;
    const int id = IdOf(self);
    g_bobbers[id] = Seen{-99, -99, -99, -99, 0, true};
    Log("fishing: bobber %d cast (from %s)", id, caller);
}

void __cdecl Step2Hook(yyc::CInstance* self, yyc::CInstance* other) {
    if (On() && self != nullptr) ++g_bobbers[IdOf(self)].steps;
    g_origStep2(self, other);
}

void __cdecl ReelHook(yyc::CInstance* self, yyc::CInstance* other) {
    if (On() && self != nullptr) Log("fishing: bobber %d reeled in (from %s)", IdOf(self), Caller());
    g_origReel(self, other);
}

void __cdecl CollectHook(yyc::CInstance* self, yyc::CInstance* other) {
    if (!On() || self == nullptr) return g_origCollect(self, other);
    const int id = IdOf(self);
    Log("fishing: bobber %d collects item %g (from %s)", id, Var(id, kSlotCaught), Caller());
    g_origCollect(self, other);
    Log("fishing: bobber %d %s after collecting", id, yyc::FindInstance(id) != nullptr ? "STILL THERE" : "gone");
}

void __cdecl MissHook(yyc::CInstance* self, yyc::CInstance* other) {
    if (On() && self != nullptr) Log("fishing: bobber %d missed the fish (from %s)", IdOf(self), Caller());
    g_origMiss(self, other);
}

bool Hook(uint32_t va, void* detour, yyc::EventFn* original) {
    const MH_STATUS status = MH_CreateHook(yyc::At<void*>(va), detour, reinterpret_cast<void**>(original));
    if (status != MH_OK) Log("fishing: hook @ VA %08X failed: %s", va, MH_StatusToString(status));
    return status == MH_OK;
}

}  // namespace

bool InstallHooks(bool (*inSession)()) {
    g_inSession = inSession;
    bool ok = Hook(ev::Create, reinterpret_cast<void*>(&CreateHook), &g_origCreate);
    ok = Hook(ev::Step2, reinterpret_cast<void*>(&Step2Hook), &g_origStep2) && ok;
    ok = Hook(ev::Reel, reinterpret_cast<void*>(&ReelHook), &g_origReel) && ok;
    ok = Hook(ev::Collect, reinterpret_cast<void*>(&CollectHook), &g_origCollect) && ok;
    ok = Hook(ev::Miss, reinterpret_cast<void*>(&MissHook), &g_origMiss) && ok;
    return ok;
}

void Tick(yyc::CInstance* localPlayer) {
    ++g_steps;
    if (g_bobbers.empty() || g_steps % kTickEverySteps != 0) return;
    const bool heartbeat = g_steps % kHeartbeatSteps == 0;
    for (auto it = g_bobbers.begin(); it != g_bobbers.end();) {
        const int id = it->first;
        Seen& seen = it->second;
        if (yyc::FindInstance(id) == nullptr) {
            Log("fishing: bobber %d is gone", id);
            it = g_bobbers.erase(it);
            continue;
        }
        const Seen now{static_cast<int>(Var(id, kSlotCastState)), static_cast<int>(Var(id, kSlotWater)),
                       static_cast<int>(Var(id, kSlotCaught)), static_cast<int>(Var(id, kSlotMiss)), seen.steps,
                       true};
        const bool changed = now.castState != seen.castState || now.water != seen.water ||
                             now.caught != seen.caught || now.miss != seen.miss;
        if (changed || heartbeat) {
            LogState(changed ? "changed:" : "still:", id, localPlayer);
            seen.castState = now.castState;
            seen.water = now.water;
            seen.caught = now.caught;
            seen.miss = now.miss;
            seen.steps = 0;  // a bobber whose end step stops shows "0 end steps" on its next line
        }
        ++it;
    }
}

void Reset() {
    g_bobbers.clear();
    g_steps = 0;
}

}  // namespace coop::fishing
