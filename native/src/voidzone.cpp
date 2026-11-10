#include "voidzone.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

#include "MinHook.h"
#include "dungeons.h"
#include "land.h"
#include "log.h"
#include "mutant.h"
#include "sync.h"

namespace coop::voidzone {

namespace {

constexpr uint8_t kVoidSeed = 108;   // host -> guests: SeedMsg, the seed of the level the host is about to enter
constexpr uint8_t kVoidState = 109;  // host -> guests: StateMsg, the level's timer and whether it is complete

// decompiled/code/index.tsv and the decompiled events' headers.
constexpr uint32_t kScriptVoidLoad = 0x0073D4F0;    // gameModifier + 1 (0 first, from the world), ZoneGoTo(1)
constexpr uint32_t kScriptZoneGoTo = 0x0073A1A0;    // SaveGame, then a fade that loads the zone
constexpr uint32_t kScriptIsVoidZone = 0x00834630;
constexpr uint32_t kVoidControlStep0 = 0x00943D80;  // the timer; no enemy left: level complete, a portal on
constexpr uint32_t kVoidControlStep1 = 0x0093C250;  // the first one (init false) generates the level
constexpr int kZoneWorld = 0, kZoneVoid = 1;        // ZoneGoTo's argument: the leaderboard's, VoidLoad's
constexpr int kObjVoidControl = 4, kObjVoidLeaderboard = 268, kObjTransition = 287, kObjZoneLoader = 324;
constexpr int kSlotGameModifier = 0x528, kSlotKillVoidPortals = 0x5E6;  // globals (decompiled/code/var_slots.tsv)
// VoidControl's. `init` is true until the level is generated: its Step_1 generates while it is (waiting for the
// zone's load first, a step or more), its Step_0 does nothing meanwhile.
constexpr int kSlotInit = 0x596, kSlotLevelIsComplete = 0x604, kSlotTimerCurrent = 0x888, kSlotPortalPosition = 0x707;
constexpr unsigned kStateEverySteps = 30;
constexpr double kLeaderboardDepth = -10000;

#pragma pack(push, 1)
struct SeedMsg {
    int32_t level;
    double seed;
};
struct StateMsg {
    int32_t level;
    double timer;  // VoidControl.timerCurrent: steps left (-1 once the leaderboard is up)
    int32_t complete;
    double portalX, portalY;  // VoidControl.portalPosition once complete: its Draw_73 points the way there
};
#pragma pack(pop)

bool (*g_isHost)() = nullptr;
bool (*g_isGuest)() = nullptr;
yyc::ScriptFn g_origVoidLoad = nullptr;
yyc::ScriptFn g_origZoneGoTo = nullptr;
yyc::EventFn g_origStep0 = nullptr;
yyc::EventFn g_origStep1 = nullptr;

std::map<int, double> g_seeds;    // level -> the seed it is generated from (the host's)
StateMsg g_state{-1, 0, 0, 0, 0};  // guest: the host's level as last told (timer counted down here in between)
int g_inVoidLoad = 0;             // inside VoidLoad (its ZoneGoTo(1) is not a way out)
bool g_going = false;             // guest: a move the host ordered (let through the hooks)
bool g_leaderboardUp = false;     // guest: the leaderboard of the current level is shown
StateMsg g_lastSent{-1, 0, -1, 0, 0};  // host
unsigned g_steps = 0;
int g_generated = 0;   // the level just generated (its start is taken at the next Tick), 0 if none
int g_startPlace = -1;  // the place whose start is known, and where the generation put the local player
double g_startX = 0, g_startY = 0;

bool IsHost() { return g_isHost != nullptr && g_isHost(); }
bool IsGuest() { return g_isGuest != nullptr && g_isGuest(); }
// The mutant twin (void.ps1 -Mutant void): the Void as before this module - nobody follows, nothing is held back.
bool Off() { return Mutant("void"); }

bool Connected() { return !Off() && net::IsConnected() && (IsHost() || IsGuest()); }

double GlobalReal(int slot) {
    const yyc::RValue* value = yyc::GlobalVar(slot);
    return value != nullptr && yyc::IsNumber(*value) ? yyc::ToReal(*value) : 0;
}

void SetGlobalReal(int slot, double value) {
    if (yyc::RValue* global = yyc::GlobalVar(slot)) {
        yyc::Free(global);
        *global = yyc::Real(value);
    }
}

double VarReal(int id, int slot) {
    yyc::RValue value{};
    const double real = yyc::GetVar(id, slot, &value) && yyc::IsNumber(value) ? yyc::ToReal(value) : 0;
    yyc::Free(&value);
    return real;
}

void SetVarReal(int id, int slot, double value) {
    yyc::RValue real = yyc::Real(value);
    yyc::SetVar(id, slot, &real);
}

int Level() { return static_cast<int>(GlobalReal(kSlotGameModifier)); }

int IdOf(yyc::CInstance* instance) { return static_cast<int>(yyc::GetBuiltinReal(instance, yyc::va::YYVar_id)); }

int VoidControl(yyc::CInstance* self) {
    const int id = static_cast<int>(yyc::CallReal(self, "instance_find", {double(kObjVoidControl), 0}));
    return yyc::FindInstance(id) != nullptr ? id : 0;
}

bool Exists(yyc::CInstance* self, int object) { return yyc::CallReal(self, "instance_number", {double(object)}) > 0; }

// A fade or a zone load is under way: going again would load twice (and raise the level twice).
bool OnTheWay(yyc::CInstance* self) { return Exists(self, kObjTransition) || Exists(self, kObjZoneLoader); }

// The level a VoidLoad from here goes to: the next one in the Void, the first one from the world.
int NextLevel(yyc::CInstance* self) { return InVoid(self) ? Level() + 1 : 1; }

yyc::RValue* CallZoneGoTo(yyc::ScriptFn fn, yyc::CInstance* self, int zone, yyc::RValue* result) {
    yyc::RValue target = yyc::Real(zone);
    yyc::RValue* args[1] = {&target};
    return fn(self, self, result, 1, args);
}

// VoidLoad: the Void portal's way into the Void, or on to the next level.
// Host: the level is generated from a seed every player gets first; then everyone follows. Guest: asks the host.
yyc::RValue* __cdecl VoidLoadHook(yyc::CInstance* self, yyc::CInstance* other, yyc::RValue* result, int argc,
                                  yyc::RValue** args) {
    if (!Connected() || self == nullptr) return g_origVoidLoad(self, other, result, argc, args);
    if (IsGuest()) {
        if (g_going) return g_origVoidLoad(self, other, result, argc, args);
        const int level = NextLevel(self);
        dungeons::GuestAsk(self, kPlaceVoid + level);
        Log("void: asked the host to go to level %d", level);
        return result;
    }
    if (OnTheWay(self)) return result;  // a second portal press (or a guest's ask) while the first one fades
    const int expected = NextLevel(self);
    const SeedMsg seed{expected, static_cast<double>(GetTickCount() % 1000000000)};
    g_seeds[expected] = seed.seed;
    net::Send(kVoidSeed, &seed, sizeof(seed));
    ++g_inVoidLoad;
    yyc::RValue* r = g_origVoidLoad(self, other, result, argc, args);
    --g_inVoidLoad;
    const int level = Level();
    if (level != expected) {  // not as the decompile reads: the seed goes out again under the level it is
        const SeedMsg again{level, seed.seed};
        g_seeds[level] = seed.seed;
        net::Send(kVoidSeed, &again, sizeof(again));
    }
    g_lastSent = StateMsg{-1, 0, -1, 0, 0};
    dungeons::HostAnnounce(self, kPlaceVoid + level);
    Log("void: the host goes to level %d (seed %.0f) - the guests follow", level, seed.seed);
    return r;
}

// ZoneGoTo out of the Void (the leaderboard's button): the host goes and everyone follows; a guest asks.
yyc::RValue* __cdecl ZoneGoToHook(yyc::CInstance* self, yyc::CInstance* other, yyc::RValue* result, int argc,
                                  yyc::RValue** args) {
    const bool leaving = Connected() && self != nullptr && g_inVoidLoad == 0 && argc >= 1 &&
                         yyc::ToReal(*args[0]) != kZoneVoid && InVoid(self);
    if (!leaving) return g_origZoneGoTo(self, other, result, argc, args);
    if (IsGuest()) {
        if (g_going) return g_origZoneGoTo(self, other, result, argc, args);
        dungeons::GuestAsk(self, yyc::kRoomGame);
        Log("void: asked the host to go back to the world");
        return result;
    }
    if (OnTheWay(self)) return result;
    yyc::RValue* r = g_origZoneGoTo(self, other, result, argc, args);
    dungeons::HostAnnounce(self, yyc::kRoomGame);
    Log("void: the host goes back to the world - the guests follow");
    return r;
}

// VoidControl's begin step while `init` holds: the one that generates the level - from the host's seed, so the
// islands come out alike everywhere (the seed is set again each such step: the one that generates starts from it);
// at a guest, what the generation spawns (resources, enemies) goes again: the host's arrive.
void __cdecl Step1Hook(yyc::CInstance* self, yyc::CInstance* other) {
    const int id = self != nullptr ? IdOf(self) : 0;
    if (!Connected() || self == nullptr || VarReal(id, kSlotInit) == 0) {
        g_origStep1(self, other);
        return;
    }
    const int level = Level();
    const auto seed = g_seeds.find(level);
    if (seed != g_seeds.end()) yyc::CallReal(self, "random_set_seed", {seed->second});
    if (IsGuest()) sync::BeginSuppressSpawns();
    g_origStep1(self, other);
    if (IsGuest()) sync::EndSuppressSpawns(self);
    if (seed != g_seeds.end()) yyc::CallReal(self, "randomize", {});
    if (VarReal(id, kSlotInit) != 0) return;  // still loading the zone: generated in a later step
    if (IsHost()) land::HostSendMapSoon();  // the guests' islands, put right where they came out otherwise
    g_leaderboardUp = false;
    g_generated = level;  // the generation put the player at the level's start: taken at the next Tick
    Log("void: level %d generated %s", level, seed != g_seeds.end() ? "from the host's seed" : "without a seed");
}

// VoidControl's step. At a guest the timer and the level's end are the host's: its own would count on its own and
// open a portal of its own as soon as no enemy is here (the host's arrive a step or more later) - the level was
// skipped over and over (playtest 11). Run with "complete" set, the step keeps only its looks.
void __cdecl Step0Hook(yyc::CInstance* self, yyc::CInstance* other) {
    if (!Connected() || !IsGuest() || self == nullptr) {
        g_origStep0(self, other);
        return;
    }
    const int id = IdOf(self);
    const bool known = g_state.level == Level();
    SetVarReal(id, kSlotLevelIsComplete, 1);
    g_origStep0(self, other);
    if (known && g_state.complete == 0 && g_state.timer > 0) g_state.timer -= 1;  // as the host's does each step
    const bool complete = known && g_state.complete != 0;
    if (complete) {  // where the host's portal on stands (its Draw_73 reads it once the level is complete)
        yyc::RValue x = yyc::Real(g_state.portalX), y = yyc::Real(g_state.portalY);
        yyc::SetVarAt(id, kSlotPortalPosition, 0, &x);
        yyc::SetVarAt(id, kSlotPortalPosition, 1, &y);
    }
    SetVarReal(id, kSlotLevelIsComplete, complete ? 1 : 0);
    if (known) SetVarReal(id, kSlotTimerCurrent, (std::max)(0.0, g_state.timer));
}

// Element `index` of an instance's array variable (0 if it is not one).
double VarAt(int id, int slot, int index) {
    yyc::RValue value{};
    if (!yyc::GetVar(id, slot, &value)) return 0;
    double real = 0;
    if ((value.kind & yyc::kKindMask) == yyc::kArray) {
        const yyc::RValue element = yyc::ArrayElement(&value, index);
        if (yyc::IsNumber(element)) real = yyc::ToReal(element);
    }
    yyc::Free(&value);
    return real;
}

void HostTick(yyc::CInstance* localPlayer) {
    const int control = VoidControl(localPlayer);
    // On the way to the next level the level number is the new one, the VoidControl still the old (complete) one;
    // until the new one has generated, there is no state to tell.
    if (control == 0 || OnTheWay(localPlayer) || VarReal(control, kSlotInit) != 0) return;
    const StateMsg state{Level(), VarReal(control, kSlotTimerCurrent),
                         VarReal(control, kSlotLevelIsComplete) != 0 ? 1 : 0, VarAt(control, kSlotPortalPosition, 0),
                         VarAt(control, kSlotPortalPosition, 1)};
    const bool news = state.complete != g_lastSent.complete || state.level != g_lastSent.level ||
                      (state.timer <= 0) != (g_lastSent.timer <= 0);
    if (!news && g_steps % kStateEverySteps != 0) return;
    net::Send(kVoidState, &state, sizeof(state));
    if (state.complete != g_lastSent.complete && state.complete != 0) Log("void: level %d complete", state.level);
    g_lastSent = state;
}

// Guest: the host's timer ran out - the run is over here too: the leaderboard, as the game's own step shows it.
void GuestTick(yyc::CInstance* localPlayer) {
    if (g_state.level != Level() || g_state.complete != 0 || g_state.timer > 0 || g_leaderboardUp) return;
    g_leaderboardUp = true;
    if (Exists(localPlayer, kObjVoidLeaderboard)) return;
    const double x = yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_x), y = yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_y);
    yyc::CallReal(localPlayer, "instance_create_depth", {x, y, kLeaderboardDepth, double(kObjVoidLeaderboard)});
    SetGlobalReal(kSlotKillVoidPortals, 1);
    Log("void: the host's timer ran out at level %d - the leaderboard", g_state.level);
}

}  // namespace

bool IsVoidPlace(int place) { return place > kPlaceVoid; }

bool InVoid(yyc::CInstance* any) {
    if (any == nullptr) return false;
    yyc::RValue result{};
    yyc::At<yyc::ScriptFn>(kScriptIsVoidZone)(any, any, &result, 0, nullptr);
    const bool inVoid = yyc::IsNumber(result) && yyc::ToReal(result) != 0;
    yyc::Free(&result);
    return inVoid;
}

int Place(yyc::CInstance* any) {
    const int room = static_cast<int>(yyc::GetBuiltinReal(any, yyc::va::YYVar_room));
    return room == yyc::kRoomGame && !Off() && InVoid(any) ? kPlaceVoid + (std::max)(1, Level()) : room;
}

bool InstallHooks(bool (*isHost)(), bool (*isGuest)()) {
    g_isHost = isHost;
    g_isGuest = isGuest;
    const bool ok = MH_CreateHook(yyc::At<void*>(kScriptVoidLoad), reinterpret_cast<void*>(&VoidLoadHook),
                                  reinterpret_cast<void**>(&g_origVoidLoad)) == MH_OK &&
                    MH_CreateHook(yyc::At<void*>(kScriptZoneGoTo), reinterpret_cast<void*>(&ZoneGoToHook),
                                  reinterpret_cast<void**>(&g_origZoneGoTo)) == MH_OK &&
                    MH_CreateHook(yyc::At<void*>(kVoidControlStep0), reinterpret_cast<void*>(&Step0Hook),
                                  reinterpret_cast<void**>(&g_origStep0)) == MH_OK &&
                    MH_CreateHook(yyc::At<void*>(kVoidControlStep1), reinterpret_cast<void*>(&Step1Hook),
                                  reinterpret_cast<void**>(&g_origStep1)) == MH_OK;
    if (!ok) Log("void: hooks failed");
    return ok;
}

bool GuestGo(yyc::CInstance* player, int place) {
    const bool intoLevel = IsVoidPlace(place);
    const bool outOfVoid = place == yyc::kRoomGame && InVoid(player);
    if (!intoLevel && !outOfVoid) return false;
    if (OnTheWay(player)) return true;
    yyc::RValue result{};
    g_going = true;
    if (intoLevel) {
        g_origVoidLoad(player, player, &result, 0, nullptr);
        SetGlobalReal(kSlotGameModifier, place - kPlaceVoid);  // the host's level, whatever ours counted
        Log("void: following the host to level %d", place - kPlaceVoid);
    } else {
        CallZoneGoTo(g_origZoneGoTo, player, kZoneWorld, &result);
        Log("void: following the host back to the world");
    }
    g_going = false;
    yyc::Free(&result);
    return true;
}

bool HostGo(yyc::CInstance* player, int place) {
    const bool intoLevel = IsVoidPlace(place);
    const bool outOfVoid = place == yyc::kRoomGame && InVoid(player);
    if (!intoLevel && !outOfVoid) return false;
    yyc::RValue result{};
    if (intoLevel && place != kPlaceVoid + NextLevel(player)) {
        Log("void: a guest asked for level %d, but the next one is %d - not going", place - kPlaceVoid,
            NextLevel(player));
    } else if (intoLevel) {
        yyc::At<yyc::ScriptFn>(kScriptVoidLoad)(player, player, &result, 0, nullptr);  // through the hook: seed, news
    } else {
        CallZoneGoTo(yyc::At<yyc::ScriptFn>(kScriptZoneGoTo), player, kZoneWorld, &result);
    }
    yyc::Free(&result);
    return true;
}

bool Handle(const net::Message& message, yyc::CInstance* localPlayer) {
    if (message.type == kVoidSeed) {
        SeedMsg msg{};
        if (message.payload.size() == sizeof(msg) && IsGuest()) {
            memcpy(&msg, message.payload.data(), sizeof(msg));
            if (std::isfinite(msg.seed) && msg.level > 0) g_seeds[msg.level] = msg.seed;
        }
        return true;
    }
    if (message.type == kVoidState) {
        StateMsg msg{};
        if (message.payload.size() == sizeof(msg) && IsGuest()) {
            memcpy(&msg, message.payload.data(), sizeof(msg));
            if (std::isfinite(msg.timer) && std::isfinite(msg.portalX) && std::isfinite(msg.portalY) && msg.level > 0) {
                if (msg.level != g_state.level) g_leaderboardUp = false;
                g_state = msg;
            }
        }
        return true;
    }
    (void)localPlayer;
    return false;
}

void Tick(yyc::CInstance* localPlayer) {
    ++g_steps;
    if (Off() || !InVoid(localPlayer)) return;
    if (g_generated != 0) {
        g_startPlace = kPlaceVoid + g_generated;
        g_startX = yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_x);
        g_startY = yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_y);
        g_generated = 0;
    }
    if (IsHost()) HostTick(localPlayer);
    if (IsGuest()) GuestTick(localPlayer);
}

bool StartPoint(int place, double* x, double* y) {
    if (place != g_startPlace) return false;
    *x = g_startX;
    *y = g_startY;
    return true;
}

void Reset() {
    g_generated = 0;
    g_startPlace = -1;
    g_seeds.clear();
    g_state = StateMsg{-1, 0, 0, 0, 0};
    g_lastSent = StateMsg{-1, 0, -1, 0, 0};
    g_inVoidLoad = 0;
    g_going = false;
    g_leaderboardUp = false;
}

}  // namespace coop::voidzone
