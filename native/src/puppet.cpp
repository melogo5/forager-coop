#include "puppet.h"

#include <windows.h>

#include <cmath>
#include <initializer_list>
#include <cstring>

#include "MinHook.h"
#include "log.h"

namespace coop::puppet {

namespace {

constexpr int kObjPlayer = 22;
constexpr double kFollow = 0.35;       // fraction of the remaining distance covered per step
constexpr double kSnapDistance = 256;  // teleport instead of gliding across the map

// objPlayer events that must not run for the puppet (its state comes from the network).
// Create/PreCreate/CleanUp run normally; Draw_0 runs with PlayerFind() pointing at the puppet.
namespace ev {
constexpr uint32_t Step_0 = 0x009A0430;
constexpr uint32_t Step_1 = 0x009D76D0;  // begin step: also where hp < 1 turns into a game over
constexpr uint32_t Draw_0 = 0x009DCB00;
}  // namespace ev
constexpr uint32_t kSkippedEvents[] = {
    0x0099F340,  // Alarm_1
    0x0099F650,  // Alarm_2
    0x009C66A0,  // Step_2
    0x009D8F90,  // Collision with parEntity
    0x009D90A0,  // Mouse_61
    0x009D93D0,  // Mouse_60
    0x009D9700,  // Other_10
    0x009DA130,  // Other_18
    0x009DC8B0,  // Other_4 (room start)
    0x009F7590,  // Draw_73
};
constexpr int kSkippedCount = sizeof(kSkippedEvents) / sizeof(kSkippedEvents[0]);
constexpr uint32_t kScriptDmgPlayer = 0x00727030;
constexpr char kCreateFrame[] = "gml_Object_objPlayer_Create_0";

struct StackFrame {
    StackFrame* prev;
    const char* name;
    int line;
};

LocalStepFn g_onLocalStep = nullptr;
yyc::CInstance* g_puppet = nullptr;  // valid while g_puppetId's instance exists; checked every local step
int g_puppetId = 0;
bool g_creating = false;     // PlayerFind() -> calling instance, while the puppet runs Create_0
int g_actorId = 0;           // PlayerFind() -> this instance id, when non-zero
int g_actingDepth = 0;       // BeginActing nesting
bool g_enemiesSeeBoth = false;     // host with a guest: enemies go for the nearer player (PLAYTEST.md #8)
bool g_familiarsFollowPeer = false;  // guest: familiars are the host's (PLAYTEST.md #14)
constexpr int kParEnemy = 23;        // decompiled/data/objects.tsv
constexpr int kParFamiliar = 74;     // lantern, halo, droids...
constexpr int kMaxObjects = 1024;
int8_t g_kinds[kMaxObjects] = {};  // object index -> 0 unknown, else kEnemy/kFamiliar/kOther
constexpr int8_t kEnemy = 1, kFamiliar = 2, kOther = 3;

yyc::EventFn g_origStep = nullptr;
yyc::EventFn g_origStep1 = nullptr;
LocalStepFn g_beforeLocalStep1 = nullptr;
yyc::EventFn g_origDraw = nullptr;
yyc::EventFn g_origSkipped[kSkippedCount] = {};
yyc::ScriptFn g_origPlayerFind = nullptr;
yyc::ScriptFn g_origDmgPlayer = nullptr;

bool IsPuppet(const yyc::CInstance* instance) { return instance != nullptr && instance == g_puppet; }

const char* CallerName() {
    const StackFrame* top = *yyc::At<StackFrame**>(yyc::va::StackTraceTop);
    return top != nullptr ? top->name : nullptr;
}

bool IsA(yyc::CInstance* self, int object, int parent) {
    yyc::RValue args[2] = {yyc::Real(object), yyc::Real(parent)};
    yyc::RValue result{};
    yyc::CallBuiltin("object_is_ancestor", self, 2, args, &result);
    const bool is = object == parent || yyc::ToReal(result) != 0;
    yyc::Free(&result);
    return is;
}

int8_t KindOf(yyc::CInstance* self) {
    const int object = static_cast<int>(yyc::GetBuiltinReal(self, yyc::va::YYVar_object_index));
    if (object < 0 || object >= kMaxObjects) return kOther;
    int8_t& kind = g_kinds[object];
    if (kind == 0) kind = IsA(self, object, kParEnemy) ? kEnemy : IsA(self, object, kParFamiliar) ? kFamiliar : kOther;
    return kind;
}

double DistanceSquared(yyc::CInstance* a, yyc::CInstance* b) {
    const double dx = yyc::GetBuiltinReal(a, yyc::va::YYVar_x) - yyc::GetBuiltinReal(b, yyc::va::YYVar_x);
    const double dy = yyc::GetBuiltinReal(a, yyc::va::YYVar_y) - yyc::GetBuiltinReal(b, yyc::va::YYVar_y);
    return dx * dx + dy * dy;
}

// Hot path (~350 calls per frame): a single branch when no override is active.
yyc::RValue* __cdecl PlayerFindHook(yyc::CInstance* self, yyc::CInstance* other, yyc::RValue* result, int argc,
                                    yyc::RValue** args) {
    int id = g_actorId;
    if (id == 0 && g_creating) {
        // The puppet's Create_0 edits "the player's" gear: point it at the puppet itself. Create_0's own
        // checks ("am I the main player?" -> create uiToolbar/familiars) must keep seeing the local player.
        const char* caller = CallerName();
        if (caller != nullptr && strcmp(caller, kCreateFrame) != 0) {
            id = static_cast<int>(yyc::GetBuiltinReal(self, yyc::va::YYVar_id));
        }
    }
    if (id == 0 && g_familiarsFollowPeer && g_puppet != nullptr && self != nullptr && KindOf(self) == kFamiliar) {
        id = g_puppetId;
    }
    if (id == 0 && g_enemiesSeeBoth && g_puppet != nullptr && self != nullptr && KindOf(self) == kEnemy) {
        g_origPlayerFind(self, other, result, argc, args);
        yyc::CInstance* local = yyc::FindInstance(static_cast<int>(yyc::ToReal(*result)));
        if (local == nullptr || DistanceSquared(self, g_puppet) >= DistanceSquared(self, local)) return result;
        id = g_puppetId;
        static bool told = false;
        if (!told) Log("puppet: an enemy goes for the guest (nearer than the host)");
        told = true;
    }
    if (id == 0) return g_origPlayerFind(self, other, result, argc, args);
    yyc::Free(result);
    *result = yyc::Real(id);
    return result;
}

void __cdecl StepHook(yyc::CInstance* self, yyc::CInstance* other) {
    if (IsPuppet(self)) return;
    g_origStep(self, other);
    if (g_onLocalStep == nullptr) return;
    __try {
        g_onLocalStep(self);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("puppet: local step callback crashed (code 0x%08lX)", GetExceptionCode());
    }
}

void __cdecl Step1Hook(yyc::CInstance* self, yyc::CInstance* other) {
    if (IsPuppet(self)) return;
    if (g_beforeLocalStep1 != nullptr) {
        __try {
            g_beforeLocalStep1(self);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("puppet: before-Step_1 callback crashed (code 0x%08lX)", GetExceptionCode());
        }
    }
    g_origStep1(self, other);
}

void __cdecl DrawHook(yyc::CInstance* self, yyc::CInstance* other) {
    if (!IsPuppet(self)) {
        g_origDraw(self, other);
        return;
    }
    g_actorId = g_puppetId;
    g_origDraw(self, other);
    g_actorId = 0;
}

// Damage meant for the puppet must not land on the local player (for now it is dropped).
yyc::RValue* __cdecl DmgPlayerHook(yyc::CInstance* self, yyc::CInstance* other, yyc::RValue* result, int argc,
                                   yyc::RValue** args) {
    if (IsPuppet(other)) return result;
    return g_origDmgPlayer(self, other, result, argc, args);
}

template <int N>
void __cdecl SkippedHook(yyc::CInstance* self, yyc::CInstance* other) {
    if (!IsPuppet(self)) g_origSkipped[N](self, other);
}

template <int... N>
struct SkippedTable {
    static constexpr yyc::EventFn kHooks[] = {&SkippedHook<N>...};
};
using Skipped = SkippedTable<0, 1, 2, 3, 4, 5, 6, 7, 8, 9>;
static_assert(sizeof(Skipped::kHooks) / sizeof(Skipped::kHooks[0]) == kSkippedCount, "one hook per skipped event");

bool Hook(uint32_t va, void* detour, void* original) {
    const MH_STATUS status = MH_CreateHook(yyc::At<void*>(va), detour, static_cast<void**>(original));
    if (status != MH_OK) Log("puppet: hook @ VA %08X failed: %s", va, MH_StatusToString(status));
    return status == MH_OK;
}

yyc::CInstance* Create(yyc::CInstance* localPlayer, const State& at) {
    yyc::RValue args[4] = {yyc::Real(at.x), yyc::Real(at.y), yyc::Real(at.depth), yyc::Real(kObjPlayer)};
    yyc::RValue result{};
    g_creating = true;
    const bool called = yyc::CallBuiltin("instance_create_depth", localPlayer, 4, args, &result);
    g_creating = false;
    if (!called) return nullptr;
    g_puppetId = static_cast<int>(yyc::ToReal(result));
    yyc::Free(&result);
    g_puppet = yyc::FindInstance(g_puppetId);
    if (g_puppet != nullptr) {
        yyc::SetBuiltinReal(g_puppet, yyc::va::YYVar_image_speed, 0);
        Log("puppet: objPlayer %d created", g_puppetId);
    }
    return g_puppet;
}

void Approach(uint32_t yyvar, double target) {
    const double current = yyc::GetBuiltinReal(g_puppet, yyvar);
    yyc::SetBuiltinReal(g_puppet, yyvar, current + (target - current) * kFollow);
}

}  // namespace

bool InstallHooks(LocalStepFn onLocalStep, LocalStepFn beforeLocalStep1) {
    g_onLocalStep = onLocalStep;
    g_beforeLocalStep1 = beforeLocalStep1;
    bool ok = Hook(yyc::va::Script_PlayerFind, reinterpret_cast<void*>(&PlayerFindHook), &g_origPlayerFind);
    ok = Hook(ev::Step_0, reinterpret_cast<void*>(&StepHook), &g_origStep) && ok;
    ok = Hook(ev::Step_1, reinterpret_cast<void*>(&Step1Hook), &g_origStep1) && ok;
    ok = Hook(ev::Draw_0, reinterpret_cast<void*>(&DrawHook), &g_origDraw) && ok;
    ok = Hook(kScriptDmgPlayer, reinterpret_cast<void*>(&DmgPlayerHook), &g_origDmgPlayer) && ok;
    for (int i = 0; i < kSkippedCount; ++i) {
        ok = Hook(kSkippedEvents[i], reinterpret_cast<void*>(Skipped::kHooks[i]), &g_origSkipped[i]) && ok;
    }
    return ok;
}

void SetEnemiesSeeBoth(bool on) { g_enemiesSeeBoth = on; }

void SetFamiliarsFollowPeer(bool on) { g_familiarsFollowPeer = on; }

yyc::CInstance* Instance() {
    if (g_puppet != nullptr && yyc::FindInstance(g_puppetId) != g_puppet) {
        g_puppet = nullptr;
        g_puppetId = 0;
    }
    return g_puppet;
}

void BeginActing() {
    if (++g_actingDepth == 1 && Instance() != nullptr) g_actorId = g_puppetId;
}

void EndActing() {
    if (g_actingDepth > 0 && --g_actingDepth == 0) g_actorId = 0;
}

bool IsActing() { return g_actorId != 0 && g_actorId == g_puppetId; }

int PauseActing() {
    const int depth = g_actingDepth;
    g_actingDepth = 0;
    g_actorId = 0;
    return depth;
}

void ResumeActing(int depth) {
    g_actingDepth = depth;
    if (depth > 0 && Instance() != nullptr) g_actorId = g_puppetId;
}

namespace {

// The puppet draws the tool in its own toolbar (Draw_0 -> ToolSelected() with PlayerFind() = the puppet). It
// gets a list of its own holding just the peer's tool, so the local player's toolbar is never touched.
constexpr int kSlotToolbar = 0x897;  // decompiled/code/var_slots.tsv
constexpr int kSlotToolSelected = 0x89C;
int g_toolbarOf = 0;  // the puppet instance the list below belongs to
double g_toolbarList = -1;
int g_shownTool = -5;

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

void ShowTool(yyc::CInstance* localPlayer, int tool) {
    if (g_toolbarOf != g_puppetId) {
        g_toolbarList = CallReal(localPlayer, "ds_list_create", {});
        g_toolbarOf = g_puppetId;
        g_shownTool = -5;
        yyc::RValue list = yyc::Real(g_toolbarList);
        yyc::SetVar(g_puppetId, kSlotToolbar, &list);
    }
    if (tool == g_shownTool) return;
    g_shownTool = tool;
    CallReal(localPlayer, "ds_list_clear", {g_toolbarList});
    if (tool >= 0) CallReal(localPlayer, "ds_list_add", {g_toolbarList, double(tool)});
    yyc::RValue selected = yyc::Real(0);
    yyc::SetVar(g_puppetId, kSlotToolSelected, &selected);
}

}  // namespace

void Update(yyc::CInstance* localPlayer, const State& target) {
    if (g_puppetId != 0 && yyc::FindInstance(g_puppetId) != g_puppet) {
        g_puppet = nullptr;  // destroyed by the game (room change)
        g_puppetId = 0;
    }
    if (g_puppet == nullptr && Create(localPlayer, target) == nullptr) return;

    const double dx = target.x - yyc::GetBuiltinReal(g_puppet, yyc::va::YYVar_x);
    const double dy = target.y - yyc::GetBuiltinReal(g_puppet, yyc::va::YYVar_y);
    if (std::sqrt(dx * dx + dy * dy) > kSnapDistance) {
        yyc::SetBuiltinReal(g_puppet, yyc::va::YYVar_x, target.x);
        yyc::SetBuiltinReal(g_puppet, yyc::va::YYVar_y, target.y);
    } else {
        Approach(yyc::va::YYVar_x, target.x);
        Approach(yyc::va::YYVar_y, target.y);
    }
    yyc::SetBuiltinReal(g_puppet, yyc::va::YYVar_sprite_index, target.sprite);
    yyc::SetBuiltinReal(g_puppet, yyc::va::YYVar_image_index, target.imageIndex);
    yyc::SetBuiltinReal(g_puppet, yyc::va::YYVar_image_xscale, target.xscale);
    yyc::SetBuiltinReal(g_puppet, yyc::va::YYVar_image_yscale, target.yscale);
    yyc::SetBuiltinReal(g_puppet, yyc::va::YYVar_depth, target.depth);
    ShowTool(localPlayer, target.tool);
}

void Destroy(yyc::CInstance* localPlayer) {
    if (g_puppet != nullptr && yyc::FindInstance(g_puppetId) == g_puppet) {
        yyc::RValue id = yyc::Real(g_puppetId);
        yyc::RValue result{};
        yyc::CallBuiltin("instance_destroy", localPlayer, 1, &id, &result);
        yyc::Free(&result);
    }
    g_puppet = nullptr;
    g_puppetId = 0;
}

}  // namespace coop::puppet
