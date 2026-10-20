#include "skilltab.h"

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <utility>
#include <vector>

#include "MinHook.h"
#include "gui.h"
#include "log.h"
#include "puppet.h"

namespace coop::skilltab {

namespace {

constexpr int kRoomGame = 8;               // rmGame (decompiled/data/rooms.tsv)
constexpr int kRoomSkillGrid = 9;          // rmSkillGrid: 1280x720, what the tree is laid out for
constexpr double kGridWidth = 1280, kGridHeight = 720;
constexpr int kObjSkillGridControl = 231;  // decompiled/data/objects.tsv
constexpr int kObjSkill = 232;
constexpr int kObjPlayer = 22;
constexpr uint32_t kYYVarVisible = 0x01AD8C80;
constexpr double kDepth = -15000;
constexpr double kMatrixWorld = 2;         // matrix_world
constexpr uint32_t kVariableGetBuiltin = 0x016CA140;
// In its room the tree loads the player back from the save (LoadAsync -> LoadPlayer, which also sets the control's
// skillDisplay: until then it stays behind a black fade). In the world the player is all there: the load is
// skipped (it would bring back the saved state over the live one) and skillDisplay is set right away.
constexpr uint32_t kScriptLoadAsync = 0x00519BD0;
constexpr int kSlotSkillDisplay = 0x7B8;
constexpr uint32_t kYYVarMouseX = 0x01AD8B20, kYYVarMouseY = 0x01AD8B28;  // decompiled/code/yyvar_map.tsv
constexpr uint32_t kYYVarRoomWidth = 0x01AD8748, kYYVarRoomHeight = 0x01AD88E8;
namespace ev {  // decompiled/code/index.tsv
constexpr uint32_t ControlDraw = 0x00E39370;
constexpr uint32_t ControlDrawGui = 0x00E35ED0;
constexpr uint32_t SkillDraw = 0x00E47650;
}  // namespace ev

using GetBuiltinFn = int(__cdecl*)(yyc::CInstance* self, int varId, int arrayIndex, yyc::RValue* out);

bool (*g_inSession)() = nullptr;
bool g_open = false;
bool g_gridRoom = false;  // the tree's code runs now (made or drawn): it sees rmSkillGrid's size, as in its room
int g_control = 0;
std::vector<int> g_avatars;  // the tree's own objPlayer(s), kept inert
int g_mouseX = -1, g_mouseY = -1, g_roomWidth = -1, g_roomHeight = -1;  // builtin variable ids

GetBuiltinFn g_origGetBuiltin = nullptr;
yyc::ScriptFn g_origLoadAsync = nullptr;
yyc::EventFn g_origControlDraw = nullptr;
yyc::EventFn g_origControlDrawGui = nullptr;
yyc::EventFn g_origSkillDraw = nullptr;

int VarId(uint32_t yyvarAddress) { return yyc::At<yyc::YYVar*>(yyvarAddress)->id; }

// GUI pixels per tree pixel: the tree is laid out for 1280x720.
double GuiScale() { return gui::Call("display_get_gui_height", {}) / kGridHeight; }

// Hot path (every builtin variable read): one branch unless the tree is being made or is open.
int __cdecl GetBuiltinHook(yyc::CInstance* self, int varId, int arrayIndex, yyc::RValue* out) {
    if (!g_open && !g_gridRoom) return g_origGetBuiltin(self, varId, arrayIndex, out);
    if (g_gridRoom && (varId == g_roomWidth || varId == g_roomHeight)) {
        yyc::Free(out);
        *out = yyc::Real(varId == g_roomWidth ? kGridWidth : kGridHeight);
        return 1;
    }
    if (g_open && (varId == g_mouseX || varId == g_mouseY)) {
        // The mouse in tree pixels: where it is on screen, as the tree is drawn there.
        gui::Begin(self);
        const double gui = gui::Call(varId == g_mouseX ? "device_mouse_x_to_gui" : "device_mouse_y_to_gui", {0});
        yyc::Free(out);
        *out = yyc::Real(gui / GuiScale());
        return 1;
    }
    return g_origGetBuiltin(self, varId, arrayIndex, out);
}

yyc::RValue* __cdecl LoadAsyncHook(yyc::CInstance* self, yyc::CInstance* other, yyc::RValue* result, int argc,
                                   yyc::RValue** args) {
    if (g_gridRoom) return result;  // the tree being made in the world: nothing to load (see kScriptLoadAsync)
    return g_origLoadAsync(self, other, result, argc, args);
}

// In the world room the tree is drawn in the GUI layer (below): its room-space draws are skipped.
void __cdecl ControlDrawHook(yyc::CInstance* self, yyc::CInstance* other) {
    if (!g_open) g_origControlDraw(self, other);
}

void __cdecl SkillDrawHook(yyc::CInstance* self, yyc::CInstance* other) {
    if (!g_open) g_origSkillDraw(self, other);
}

void SetWorldMatrix(yyc::CInstance* self, double scale) {
    yyc::RValue args[9] = {yyc::Real(0), yyc::Real(0), yyc::Real(0), yyc::Real(0),    yyc::Real(0),
                           yyc::Real(0), yyc::Real(scale), yyc::Real(scale), yyc::Real(1)};
    yyc::RValue matrix{};
    yyc::CallBuiltin("matrix_build", self, 9, args, &matrix);
    yyc::RValue setArgs[2] = {yyc::Real(kMatrixWorld), matrix};
    yyc::RValue result{};
    yyc::CallBuiltin("matrix_set", self, 2, setArgs, &result);
    yyc::Free(&result);
    yyc::Free(&matrix);
}

// The tree as its room would show it: the control and every skill, deepest first, scaled from 1280x720 to the GUI.
void DrawTree(yyc::CInstance* control) {
    gui::Begin(control);
    std::vector<std::pair<double, yyc::CInstance*>> skills;
    const int count = static_cast<int>(gui::Call("instance_number", {kObjSkill}));
    for (int i = 0; i < count; ++i) {
        const int id = static_cast<int>(gui::Call("instance_find", {kObjSkill, double(i)}));
        if (yyc::CInstance* skill = yyc::FindInstance(id)) {
            skills.push_back({yyc::GetBuiltinReal(skill, yyc::va::YYVar_depth), skill});
        }
    }
    std::sort(skills.begin(), skills.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    const double controlDepth = yyc::GetBuiltinReal(control, yyc::va::YYVar_depth);
    SetWorldMatrix(control, GuiScale());
    g_gridRoom = true;  // a skill draws the tree's backdrop over the whole room, and lays itself out by its middle
    bool controlDrawn = false;
    for (const auto& [depth, skill] : skills) {
        if (!controlDrawn && controlDepth >= depth) {
            g_origControlDraw(control, control);
            controlDrawn = true;
        }
        g_origSkillDraw(skill, skill);
    }
    if (!controlDrawn) g_origControlDraw(control, control);
    g_gridRoom = false;
    SetWorldMatrix(control, 1);
    gui::Call("gpu_set_fog", {0, 0, 0, 1});  // a skill's flash turns fog on; left on it would black the world out
}

// The control's GUI draw: first the tree (DrawTree), then the control's own GUI layer (names, descriptions).
void __cdecl ControlDrawGuiHook(yyc::CInstance* self, yyc::CInstance* other) {
    if (g_open) {
        __try {
            DrawTree(self);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("skilltab: drawing the tree crashed (code 0x%08lX)", GetExceptionCode());
        }
    }
    g_origControlDrawGui(self, other);
}

bool Hook(uint32_t va, void* detour, void* original) {
    const MH_STATUS status = MH_CreateHook(yyc::At<void*>(va), detour, static_cast<void**>(original));
    if (status != MH_OK) Log("skilltab: hook @ VA %08X failed: %s", va, MH_StatusToString(status));
    return status == MH_OK;
}

std::vector<int> Instances(int object) {
    std::vector<int> ids;
    const int count = static_cast<int>(gui::Call("instance_number", {double(object)}));
    for (int i = 0; i < count; ++i) {
        ids.push_back(static_cast<int>(gui::Call("instance_find", {double(object), double(i)})));
    }
    return ids;
}

void Open(yyc::CInstance* self) {
    // The runner numbers its builtin variables at start-up, after this DLL is loaded: looked up now.
    g_mouseX = VarId(kYYVarMouseX);
    g_mouseY = VarId(kYYVarMouseY);
    g_roomWidth = VarId(kYYVarRoomWidth);
    g_roomHeight = VarId(kYYVarRoomHeight);
    gui::Begin(self);
    const std::vector<int> playersBefore = Instances(kObjPlayer);
    g_gridRoom = true;
    g_control = static_cast<int>(gui::Call("instance_create_depth", {0, 0, kDepth, kObjSkillGridControl}));
    g_gridRoom = false;
    if (yyc::FindInstance(g_control) != nullptr) {
        yyc::RValue display = yyc::Real(1);
        yyc::SetVar(g_control, kSlotSkillDisplay, &display);
    }
    // The tree makes an objPlayer of its own, the avatar its room shows: here it would be a second player in the
    // world (its own step, "you are stuck" checks, our step callback). It does nothing and is not seen.
    for (const int id : Instances(kObjPlayer)) {
        if (std::find(playersBefore.begin(), playersBefore.end(), id) != playersBefore.end()) continue;
        yyc::CInstance* avatar = yyc::FindInstance(id);
        if (avatar == nullptr) continue;
        puppet::SetInert(avatar, true);
        yyc::SetBuiltinReal(avatar, kYYVarVisible, 0);
        g_avatars.push_back(id);
    }
    g_open = yyc::FindInstance(g_control) != nullptr;
    Log("skilltab: the skill tree opened in the world (%s)", g_open ? "ok" : "FAILED");
}

void Close(yyc::CInstance* self) {
    gui::Begin(self);
    for (const int id : g_avatars) {
        yyc::CInstance* avatar = yyc::FindInstance(id);
        if (avatar == nullptr) continue;
        gui::Call("instance_destroy", {double(id)});
        puppet::SetInert(avatar, false);
    }
    g_avatars.clear();
    gui::Call("instance_destroy", {kObjSkill});
    gui::Call("instance_destroy", {kObjSkillGridControl});
    g_open = false;
    g_control = 0;
    Log("skilltab: the skill tree closed");
}

}  // namespace

bool InstallHooks(bool (*inSession)()) {
    g_inSession = inSession;
    bool ok = Hook(kVariableGetBuiltin, reinterpret_cast<void*>(&GetBuiltinHook), &g_origGetBuiltin);
    ok = Hook(kScriptLoadAsync, reinterpret_cast<void*>(&LoadAsyncHook), &g_origLoadAsync) && ok;
    ok = Hook(ev::ControlDraw, reinterpret_cast<void*>(&ControlDrawHook), &g_origControlDraw) && ok;
    ok = Hook(ev::ControlDrawGui, reinterpret_cast<void*>(&ControlDrawGuiHook), &g_origControlDrawGui) && ok;
    ok = Hook(ev::SkillDraw, reinterpret_cast<void*>(&SkillDrawHook), &g_origSkillDraw) && ok;
    return ok;
}

bool InterceptRoomGoto(yyc::CInstance* self, int room) {
    // Work in progress (docs/coop/PLAYTEST2.md): the tree shows in the world, but buying a skill does not work yet,
    // so it is off unless COOP_INWORLD_SKILLTREE=1 - the game's own skill room is used meanwhile.
    static const bool enabled = GetEnvironmentVariableA("COOP_INWORLD_SKILLTREE", nullptr, 0) > 0;
    if (!enabled && !g_open) return false;
    if (g_inSession == nullptr || !g_inSession() || self == nullptr) return false;
    const int current = static_cast<int>(yyc::GetBuiltinReal(self, yyc::va::YYVar_room));
    if (room == kRoomSkillGrid && current == kRoomGame && !g_open) {
        Open(self);
        return g_open;
    }
    if (room == kRoomGame && current == kRoomGame && g_open) {
        Close(self);
        return true;
    }
    return false;
}

bool IsOpen() {
    if (g_open && yyc::FindInstance(g_control) == nullptr) Reset();  // gone with the room
    return g_open;
}

void DebugTick(yyc::CInstance* localPlayer) {
    static const unsigned at = [] {
        char value[16] = {};
        return GetEnvironmentVariableA("COOP_DEBUG_SKILLTREE", value, sizeof(value)) > 0 ? atoi(value) * 60u : 0u;
    }();
    static unsigned steps = 0;
    if (at == 0) return;
    ++steps;
    if (steps != at && steps != at + 600) return;
    yyc::RValue room = yyc::Real(steps == at ? kRoomSkillGrid : kRoomGame), result{};
    yyc::CallBuiltin("room_goto", localPlayer, 1, &room, &result);
    yyc::Free(&result);
    Log("skilltab: debug went to room %d", steps == at ? kRoomSkillGrid : kRoomGame);
}

void Reset() {
    g_avatars.clear();
    g_open = false;
    g_gridRoom = false;
    g_control = 0;
}

}  // namespace coop::skilltab
