#include "puppet.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <cstring>
#include <string>

#include "MinHook.h"
#include "gui.h"
#include "log.h"

namespace coop::puppet {

namespace {

constexpr int kObjPlayer = 22;
constexpr double kFollow = 0.35;       // fraction of the remaining distance covered per step
constexpr double kSnapDistance = 256;  // teleport instead of gliding across the map

// objPlayer events that must not run for a puppet (its state comes from the network).
// Create/PreCreate/CleanUp run normally; Draw_0 runs with PlayerFind() pointing at the puppet.
namespace ev {
constexpr uint32_t Step_0 = 0x009A0430;
constexpr uint32_t Step_1 = 0x009D76D0;  // begin step: also where hp < 1 turns into a game over
constexpr uint32_t Draw_0 = 0x009DCB00;
constexpr uint32_t Draw_73 = 0x009F7590;  // draw end: after every instance's Draw - the labels go on top here
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
};
constexpr int kSkippedCount = sizeof(kSkippedEvents) / sizeof(kSkippedEvents[0]);
constexpr uint32_t kScriptDmgPlayer = 0x00727030;
constexpr char kCreateFrame[] = "gml_Object_objPlayer_Create_0";

struct StackFrame {
    StackFrame* prev;
    const char* name;
    int line;
};

struct Puppet {
    net::PeerId player = net::kHostPeer;
    int id = 0;
    yyc::CInstance* instance = nullptr;  // valid while `id`'s instance exists; checked by Prune every local step
    // Its toolbar (see ShowTool).
    double toolbarList = -1;
    int shownTool = -5;
    // Its label: the player's name and hearts.
    std::string name;
    float hp = 0, hpMax = 0;
};

LocalStepFn g_onLocalStep = nullptr;
std::vector<Puppet> g_puppets;  // in the order they were created
bool g_creating = false;     // PlayerFind() -> calling instance, while a puppet runs Create_0
int g_actorId = 0;           // PlayerFind() -> this instance id, when non-zero
int g_actingDepth = 0;       // BeginActing nesting
bool g_enemiesSeeAll = false;       // host with guests: enemies go for the nearest player (PLAYTEST.md #8)
bool g_familiarsFollowHost = false;  // guest: familiars are the host's (PLAYTEST.md #14)
constexpr int kParEnemy = 23;        // decompiled/data/objects.tsv
constexpr int kParFamiliar = 74;     // lantern, halo, droids...
constexpr int kMaxObjects = 1024;
int8_t g_kinds[kMaxObjects] = {};  // object index -> 0 unknown, else kEnemy/kFamiliar/kOther
constexpr int8_t kEnemy = 1, kFamiliar = 2, kOther = 3;

yyc::EventFn g_origStep = nullptr;
yyc::EventFn g_origStep1 = nullptr;
LocalStepFn g_beforeLocalStep1 = nullptr;
yyc::EventFn g_origDraw = nullptr;
yyc::EventFn g_origDrawEnd = nullptr;
yyc::EventFn g_origSkipped[kSkippedCount] = {};
yyc::ScriptFn g_origPlayerFind = nullptr;
yyc::ScriptFn g_origDmgPlayer = nullptr;

Puppet* Find(net::PeerId player) {
    for (auto& puppet : g_puppets) {
        if (puppet.player == player) return &puppet;
    }
    return nullptr;
}

// Forgets puppets the game destroyed (a room change).
void Prune() {
    g_puppets.erase(std::remove_if(g_puppets.begin(), g_puppets.end(),
                                   [](const Puppet& p) { return yyc::FindInstance(p.id) != p.instance; }),
                    g_puppets.end());
}

bool IsPuppetInstance(const yyc::CInstance* instance) {
    if (instance == nullptr) return false;
    for (const auto& puppet : g_puppets) {
        if (puppet.instance == instance) return true;
    }
    return false;
}

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

// The puppet nearer to `enemy` than the local player (`localId`) and every other puppet, or 0.
int NearestPuppetId(yyc::CInstance* enemy, int localId) {
    yyc::CInstance* local = yyc::FindInstance(localId);
    double best = local != nullptr ? DistanceSquared(enemy, local) : 1e300;
    int bestId = 0;
    for (const auto& puppet : g_puppets) {
        if (yyc::FindInstance(puppet.id) != puppet.instance) continue;  // gone with a room change, not pruned yet
        const double d = DistanceSquared(enemy, puppet.instance);
        if (d < best) {
            best = d;
            bestId = puppet.id;
        }
    }
    return bestId;
}

// Hot path (~350 calls per frame): a single branch when no override is active.
yyc::RValue* __cdecl PlayerFindHook(yyc::CInstance* self, yyc::CInstance* other, yyc::RValue* result, int argc,
                                    yyc::RValue** args) {
    int id = g_actorId;
    if (id == 0 && g_creating) {
        // A puppet's Create_0 edits "the player's" gear: point it at the puppet itself. Create_0's own
        // checks ("am I the main player?" -> create uiToolbar/familiars) must keep seeing the local player.
        const char* caller = CallerName();
        if (caller != nullptr && strcmp(caller, kCreateFrame) != 0) {
            id = static_cast<int>(yyc::GetBuiltinReal(self, yyc::va::YYVar_id));
        }
    }
    if (id == 0 && g_familiarsFollowHost && !g_puppets.empty() && self != nullptr && KindOf(self) == kFamiliar) {
        if (const Puppet* host = Find(net::kHostPeer)) id = host->id;
    }
    if (id == 0 && g_enemiesSeeAll && !g_puppets.empty() && self != nullptr && KindOf(self) == kEnemy) {
        g_origPlayerFind(self, other, result, argc, args);
        id = NearestPuppetId(self, static_cast<int>(yyc::ToReal(*result)));
        if (id == 0) return result;
        static bool told = false;
        if (!told) Log("puppet: an enemy goes for a guest (nearer than the host)");
        told = true;
    }
    if (id == 0) return g_origPlayerFind(self, other, result, argc, args);
    yyc::Free(result);
    *result = yyc::Real(id);
    return result;
}

void __cdecl StepHook(yyc::CInstance* self, yyc::CInstance* other) {
    if (IsPuppetInstance(self)) return;
    g_origStep(self, other);
    if (g_onLocalStep == nullptr) return;
    __try {
        g_onLocalStep(self);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("puppet: local step callback crashed (code 0x%08lX)", GetExceptionCode());
    }
}

void __cdecl Step1Hook(yyc::CInstance* self, yyc::CInstance* other) {
    if (IsPuppetInstance(self)) return;
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
    if (!IsPuppetInstance(self)) {
        g_origDraw(self, other);
        return;
    }
    const int previous = g_actorId;
    g_actorId = static_cast<int>(yyc::GetBuiltinReal(self, yyc::va::YYVar_id));
    g_origDraw(self, other);
    g_actorId = previous;
}

// Damage meant for a puppet must not land on the local player (for now it is dropped).
yyc::RValue* __cdecl DmgPlayerHook(yyc::CInstance* self, yyc::CInstance* other, yyc::RValue* result, int argc,
                                   yyc::RValue** args) {
    if (IsPuppetInstance(other)) return result;
    return g_origDmgPlayer(self, other, result, argc, args);
}

// Over each puppet: its player's name and hearts (sprHeart: 0 full, 1 empty), in room pixels.
constexpr int kSprHeart = 1269;
constexpr double kHeartScale = 0.22;   // 36 px -> ~8 room px
constexpr double kHeartStep = 8;       // room px between hearts
constexpr double kHeartsAbove = 21;    // room px above the puppet's origin (its feet): just over the head
constexpr double kNameAbove = 30;
constexpr double kNameScale = 0.35;
constexpr int kMaxHearts = 20;
constexpr int kNameColor = gui::Rgb(255, 255, 255);

void DrawLabel(const Puppet& puppet) {
    const double x = yyc::GetBuiltinReal(puppet.instance, yyc::va::YYVar_x);
    const double y = yyc::GetBuiltinReal(puppet.instance, yyc::va::YYVar_y);
    if (!puppet.name.empty()) gui::Text(x, y - kNameAbove, puppet.name, kNameScale, kNameColor, gui::kAlignCenter);
    const int hearts = static_cast<int>((std::min)(std::ceil(puppet.hpMax - 0.01), double(kMaxHearts)));
    if (hearts <= 0) return;
    const double size = 36 * kHeartScale;
    double hx = x - (hearts - 1) * kHeartStep / 2;
    for (int i = 0; i < hearts; ++i, hx += kHeartStep) {
        gui::Call("draw_sprite_ext", {double(kSprHeart), 1, hx, y - kHeartsAbove, kHeartScale, kHeartScale, 0,
                                      gui::kWhite, 1});
        // This heart's share of the health: 1 full, 0 empty, in between a part of it.
        const double fill = (std::min)(1.0, (std::max)(0.0, double(puppet.hp) - i));
        if (fill <= 0) continue;
        // The full heart's left part (its origin is its centre, 18,18).
        gui::SpritePart(kSprHeart, 0, 0, 0, 36 * fill, 36, hx - size / 2, y - kHeartsAbove - size / 2, kHeartScale,
                        kHeartScale);
    }
}

// The local player's draw end: then every other player's label, over everything drawn this frame.
void __cdecl DrawEndHook(yyc::CInstance* self, yyc::CInstance* other) {
    if (IsPuppetInstance(self)) return;
    g_origDrawEnd(self, other);
    if (g_puppets.empty()) return;
    __try {
        gui::Begin(self);
        for (const auto& puppet : g_puppets) {
            if (yyc::FindInstance(puppet.id) == puppet.instance) DrawLabel(puppet);
        }
        gui::End();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("puppet: drawing labels crashed (code 0x%08lX)", GetExceptionCode());
    }
}

template <int N>
void __cdecl SkippedHook(yyc::CInstance* self, yyc::CInstance* other) {
    if (!IsPuppetInstance(self)) g_origSkipped[N](self, other);
}

template <int... N>
struct SkippedTable {
    static constexpr yyc::EventFn kHooks[] = {&SkippedHook<N>...};
};
using Skipped = SkippedTable<0, 1, 2, 3, 4, 5, 6, 7, 8>;
static_assert(sizeof(Skipped::kHooks) / sizeof(Skipped::kHooks[0]) == kSkippedCount, "one hook per skipped event");

bool Hook(uint32_t va, void* detour, void* original) {
    const MH_STATUS status = MH_CreateHook(yyc::At<void*>(va), detour, static_cast<void**>(original));
    if (status != MH_OK) Log("puppet: hook @ VA %08X failed: %s", va, MH_StatusToString(status));
    return status == MH_OK;
}

Puppet* Create(yyc::CInstance* localPlayer, net::PeerId player, const State& at) {
    yyc::RValue args[4] = {yyc::Real(at.x), yyc::Real(at.y), yyc::Real(at.depth), yyc::Real(kObjPlayer)};
    yyc::RValue result{};
    g_creating = true;
    const bool called = yyc::CallBuiltin("instance_create_depth", localPlayer, 4, args, &result);
    g_creating = false;
    if (!called) return nullptr;
    Puppet puppet;
    puppet.player = player;
    puppet.id = static_cast<int>(yyc::ToReal(result));
    yyc::Free(&result);
    puppet.instance = yyc::FindInstance(puppet.id);
    if (puppet.instance == nullptr) return nullptr;
    yyc::SetBuiltinReal(puppet.instance, yyc::va::YYVar_image_speed, 0);
    Log("puppet: objPlayer %d created for player %u", puppet.id, player);
    g_puppets.push_back(puppet);
    return &g_puppets.back();
}

void Approach(yyc::CInstance* instance, uint32_t yyvar, double target) {
    const double current = yyc::GetBuiltinReal(instance, yyvar);
    yyc::SetBuiltinReal(instance, yyvar, current + (target - current) * kFollow);
}

// The puppet draws the tool in its own toolbar (Draw_0 -> ToolSelected() with PlayerFind() = the puppet). It
// gets a list of its own holding just its player's tool, so the local player's toolbar is never touched.
constexpr int kSlotToolbar = 0x897;  // decompiled/code/var_slots.tsv
constexpr int kSlotToolSelected = 0x89C;

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

void ShowTool(yyc::CInstance* localPlayer, Puppet& puppet, int tool) {
    if (puppet.toolbarList < 0) {
        puppet.toolbarList = CallReal(localPlayer, "ds_list_create", {});
        yyc::RValue list = yyc::Real(puppet.toolbarList);
        yyc::SetVar(puppet.id, kSlotToolbar, &list);
    }
    if (tool == puppet.shownTool) return;
    puppet.shownTool = tool;
    CallReal(localPlayer, "ds_list_clear", {puppet.toolbarList});
    if (tool >= 0) CallReal(localPlayer, "ds_list_add", {puppet.toolbarList, double(tool)});
    yyc::RValue selected = yyc::Real(0);
    yyc::SetVar(puppet.id, kSlotToolSelected, &selected);
}

void DestroyInstance(yyc::CInstance* localPlayer, const Puppet& puppet) {
    if (yyc::FindInstance(puppet.id) != puppet.instance) return;
    yyc::RValue id = yyc::Real(puppet.id);
    yyc::RValue result{};
    yyc::CallBuiltin("instance_destroy", localPlayer, 1, &id, &result);
    yyc::Free(&result);
}

}  // namespace

bool InstallHooks(LocalStepFn onLocalStep, LocalStepFn beforeLocalStep1) {
    g_onLocalStep = onLocalStep;
    g_beforeLocalStep1 = beforeLocalStep1;
    bool ok = Hook(yyc::va::Script_PlayerFind, reinterpret_cast<void*>(&PlayerFindHook), &g_origPlayerFind);
    ok = Hook(ev::Step_0, reinterpret_cast<void*>(&StepHook), &g_origStep) && ok;
    ok = Hook(ev::Step_1, reinterpret_cast<void*>(&Step1Hook), &g_origStep1) && ok;
    ok = Hook(ev::Draw_0, reinterpret_cast<void*>(&DrawHook), &g_origDraw) && ok;
    ok = Hook(ev::Draw_73, reinterpret_cast<void*>(&DrawEndHook), &g_origDrawEnd) && ok;
    ok = Hook(kScriptDmgPlayer, reinterpret_cast<void*>(&DmgPlayerHook), &g_origDmgPlayer) && ok;
    for (int i = 0; i < kSkippedCount; ++i) {
        ok = Hook(kSkippedEvents[i], reinterpret_cast<void*>(Skipped::kHooks[i]), &g_origSkipped[i]) && ok;
    }
    return ok;
}

void SetEnemiesSeeAll(bool on) { g_enemiesSeeAll = on; }

void SetFamiliarsFollowHost(bool on) { g_familiarsFollowHost = on; }

yyc::CInstance* Instance(net::PeerId player) {
    Prune();
    const Puppet* puppet = Find(player);
    return puppet != nullptr ? puppet->instance : nullptr;
}

std::vector<yyc::CInstance*> Instances() {
    Prune();
    std::vector<yyc::CInstance*> instances;
    for (const auto& puppet : g_puppets) instances.push_back(puppet.instance);
    return instances;
}

bool IsPuppet(const yyc::CInstance* instance) { return IsPuppetInstance(instance); }

void BeginActing(net::PeerId player) {
    if (++g_actingDepth != 1) return;
    g_actorId = Instance(player) != nullptr ? Find(player)->id : 0;
}

void EndActing() {
    if (g_actingDepth > 0 && --g_actingDepth == 0) g_actorId = 0;
}

bool IsActing() { return g_actorId != 0; }

net::PeerId ActingPlayer() {
    for (const auto& puppet : g_puppets) {
        if (puppet.id == g_actorId) return puppet.player;
    }
    return net::kHostPeer;
}

Acting PauseActing() {
    const Acting acting{g_actingDepth, g_actorId};
    g_actingDepth = 0;
    g_actorId = 0;
    return acting;
}

void ResumeActing(const Acting& acting) {
    g_actingDepth = acting.depth;
    g_actorId = acting.depth > 0 && yyc::FindInstance(acting.actorId) != nullptr ? acting.actorId : 0;
}

void Update(yyc::CInstance* localPlayer, net::PeerId player, const State& target) {
    Prune();
    Puppet* puppet = Find(player);
    if (puppet == nullptr) puppet = Create(localPlayer, player, target);
    if (puppet == nullptr) return;
    yyc::CInstance* instance = puppet->instance;

    const double dx = target.x - yyc::GetBuiltinReal(instance, yyc::va::YYVar_x);
    const double dy = target.y - yyc::GetBuiltinReal(instance, yyc::va::YYVar_y);
    if (std::sqrt(dx * dx + dy * dy) > kSnapDistance) {
        yyc::SetBuiltinReal(instance, yyc::va::YYVar_x, target.x);
        yyc::SetBuiltinReal(instance, yyc::va::YYVar_y, target.y);
    } else {
        Approach(instance, yyc::va::YYVar_x, target.x);
        Approach(instance, yyc::va::YYVar_y, target.y);
    }
    yyc::SetBuiltinReal(instance, yyc::va::YYVar_sprite_index, target.sprite);
    yyc::SetBuiltinReal(instance, yyc::va::YYVar_image_index, target.imageIndex);
    yyc::SetBuiltinReal(instance, yyc::va::YYVar_image_xscale, target.xscale);
    yyc::SetBuiltinReal(instance, yyc::va::YYVar_image_yscale, target.yscale);
    yyc::SetBuiltinReal(instance, yyc::va::YYVar_depth, target.depth);
    ShowTool(localPlayer, *puppet, target.tool);
    puppet->name.assign(target.name, strnlen(target.name, sizeof(target.name)));
    puppet->hp = target.hp;
    puppet->hpMax = target.hpMax;
}

void Destroy(yyc::CInstance* localPlayer, net::PeerId player) {
    const auto found = std::find_if(g_puppets.begin(), g_puppets.end(),
                                    [player](const Puppet& p) { return p.player == player; });
    if (found == g_puppets.end()) return;
    DestroyInstance(localPlayer, *found);
    Log("puppet: player %u's puppet removed", player);
    g_puppets.erase(found);
}

void DestroyAll(yyc::CInstance* localPlayer) {
    for (const auto& puppet : g_puppets) DestroyInstance(localPlayer, puppet);
    g_puppets.clear();
}

}  // namespace coop::puppet
