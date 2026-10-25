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
#include "skills.h"

namespace coop::skilltab {

namespace {

constexpr int kRoomGame = yyc::kRoomGame;
constexpr int kRoomSkillGrid = 9;          // rmSkillGrid: 1280x720, what the tree is laid out for
constexpr double kGridWidth = 1280, kGridHeight = 720;
constexpr int kObjSkillGridControl = 231;  // decompiled/data/objects.tsv
constexpr int kObjSkill = 232;
constexpr int kObjPlayer = yyc::obj::player;
constexpr uint32_t kYYVarVisible = 0x01AD8C80;
constexpr double kDepth = -15000;
// The tree's room shows it through a 493.7x277.7 camera that pans after the mouse (measured in rmSkillGrid); the
// world draws it the same way with a camera of its own.
constexpr double kCameraWidth = 493.714, kCameraHeight = 277.714;
// The tree fits the camera's width; its height does not: the camera moves up and down with the mouse's height on
// the screen (top of the screen: the tree's top row; bottom: its bottom row).
constexpr double kPanRangeY = 110;     // tree pixels above and below the middle
constexpr double kPanSpeed = 0.15;     // per step, of the way left
// The backdrop of the tree's room (measured on a 1280x720 screenshot of rmSkillGrid): pink diamonds on purple, a
// diamond every 83 px both ways, 35 px from its middle to a corner, both colours a little darker to the lower right.
constexpr double kDiamondPeriod = 83, kDiamondRadius = 35;
constexpr int kPurpleLight = 0x822B68, kPurpleDark = 0x6D2457;  // GML colours: 0xBBGGRR
constexpr int kPinkLight = 0xB33FBF, kPinkDark = 0x9635A0;
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
constexpr uint32_t SkillStep = 0x00E39DF0;
constexpr uint32_t ControlStep = 0x00E310C0;
constexpr uint32_t WorldGuiEnd = 0x00934370;  // WorldControl Draw_75 (Draw GUI End)
}  // namespace ev
constexpr int kSlotSkillSelected = 0x7BE;   // the control's skillSelected
constexpr int kSlotLearnedSkills = yyc::slot::learnedSkills;
constexpr int kSlotPoints = 0x7BD;          // objPlayer.skills: skill points to spend
constexpr int kSlotSkill = 0x7B6;           // objSkill.skill: its index in global.skillUnlocked
constexpr int kSlotSkillUnlocked = yyc::slot::skillUnlocked;

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
yyc::EventFn g_origSkillStep = nullptr;
yyc::EventFn g_origControlStep = nullptr;
yyc::EventFn g_origWorldGuiEnd = nullptr;
int VarId(uint32_t yyvarAddress) { return yyc::At<yyc::YYVar*>(yyvarAddress)->id; }

double g_cameraX = kGridWidth / 2, g_cameraY = kGridHeight / 2;  // the tree camera's middle, in tree pixels

// GUI pixels per tree pixel, through the tree camera.
double GuiScale() { return gui::Call("display_get_gui_height", {}) / kCameraHeight; }
double CameraLeft() { return g_cameraX - kCameraWidth / 2; }
double CameraTop() { return g_cameraY - kCameraHeight / 2; }

// Once per step: the camera eases toward the height the mouse points at.
void PanCamera(yyc::CInstance* self) {
    gui::Begin(self);
    const double height = gui::Call("display_get_gui_height", {});
    if (height <= 0) return;
    const double share = (std::max)(0.0, (std::min)(1.0, gui::Call("device_mouse_y_to_gui", {0}) / height));
    const double target = kGridHeight / 2 + (share * 2 - 1) * kPanRangeY;
    g_cameraX = kGridWidth / 2;
    g_cameraY += (target - g_cameraY) * kPanSpeed;
}

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
        *out = yyc::Real((varId == g_mouseX ? CameraLeft() : CameraTop()) + gui / GuiScale());
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

// The tree's steps lay it out from the room's size: they see the tree's room, not the world.
// A skill bought there in a session is bought for everyone: skills.h records what the purchase does.
void __cdecl SkillStepHook(yyc::CInstance* self, yyc::CInstance* other) {
    g_gridRoom = g_open;
    skills::RecordPurchases(g_inSession != nullptr && g_inSession());
    g_origSkillStep(self, other);
    skills::RecordPurchases(false);
    g_gridRoom = false;
}

void __cdecl ControlStepHook(yyc::CInstance* self, yyc::CInstance* other) {
    if (g_open) PanCamera(self);
    g_gridRoom = g_open;
    g_origControlStep(self, other);
    g_gridRoom = false;
}

double VarReal(int id, int slot) {
    yyc::RValue value{};
    const bool number = yyc::GetVar(id, slot, &value) && yyc::IsNumber(value);
    const double v = number ? yyc::ToReal(value) : -99;
    yyc::Free(&value);
    return v;
}

// Testing aid (with COOP_DEBUG_SKILLTREE): while open, every 2 s, the skills' state and the points.
void LogState(yyc::CInstance* self) {
    static unsigned ticks = 0;
    if (!g_open || ++ticks % 120 != 0) return;
    gui::Begin(self);
    int available = 0, learnedHere = 0;
    const int count = static_cast<int>(gui::Call("instance_number", {kObjSkill}));
    for (int i = 0; i < count; ++i) {
        yyc::CInstance* skill = yyc::FindInstance(static_cast<int>(gui::Call("instance_find", {kObjSkill, double(i)})));
        if (skill == nullptr) continue;
        const double image = yyc::GetBuiltinReal(skill, yyc::va::YYVar_image_index);
        if (image == 1) ++available;
        if (image == 2) ++learnedHere;
        if (image == 1 && ticks == 120) {
            Log("skilltab: skill at (%.0f, %.0f) available", yyc::GetBuiltinReal(skill, yyc::va::YYVar_x),
                yyc::GetBuiltinReal(skill, yyc::va::YYVar_y));
        }
    }
    const yyc::RValue* learned = yyc::GlobalVar(kSlotLearnedSkills);
    Log("skilltab: %d skills: %d available, %d learned (learnedSkills %g); selected %g; points %g", count, available,
        learnedHere, learned != nullptr ? yyc::ToReal(*learned) : -1, VarReal(g_control, kSlotSkillSelected),
        VarReal(static_cast<int>(yyc::GetBuiltinReal(self, yyc::va::YYVar_id)), kSlotPoints));
}

// Tree pixels to GUI pixels: scaled, then moved so the camera's corner is the GUI's corner.
void SetWorldMatrix(yyc::CInstance* self, double scale, double left = 0, double top = 0) {
    yyc::RValue args[9] = {yyc::Real(-left * scale), yyc::Real(-top * scale), yyc::Real(0),
                           yyc::Real(0),             yyc::Real(0),           yyc::Real(0),
                           yyc::Real(scale),         yyc::Real(scale),       yyc::Real(1)};
    yyc::RValue matrix{};
    yyc::CallBuiltin("matrix_build", self, 9, args, &matrix);
    yyc::RValue setArgs[2] = {yyc::Real(kMatrixWorld), matrix};
    yyc::RValue result{};
    yyc::CallBuiltin("matrix_set", self, 2, setArgs, &result);
    yyc::Free(&result);
    yyc::Free(&matrix);
}

int MixColour(int a, int b, double t) {
    int out = 0;
    for (int shift = 0; shift <= 16; shift += 8) {
        const int ca = (a >> shift) & 0xFF, cb = (b >> shift) & 0xFF;
        out |= static_cast<int>(ca + (cb - ca) * t + 0.5) << shift;
    }
    return out;
}

// The tree room's backdrop, in GUI pixels (it stays put while the tree camera moves, as a room background does).
void DrawBackdrop() {
    const double width = gui::Call("display_get_gui_width", {});
    const double height = gui::Call("display_get_gui_height", {});
    const double scale = height / kGridHeight;
    gui::Call("draw_set_alpha", {1});
    gui::Call("draw_rectangle_colour", {0, 0, width, height, double(kPurpleLight),
                                        double(MixColour(kPurpleLight, kPurpleDark, 0.5)), double(kPurpleDark),
                                        double(MixColour(kPurpleLight, kPurpleDark, 0.5)), 0});
    const double period = kDiamondPeriod * scale, radius = kDiamondRadius * scale;
    for (double y = 0; y < height + period; y += period) {
        for (double x = 17 * scale; x < width + period; x += period) {
            const double c = double(MixColour(kPinkLight, kPinkDark, (x / width + y / height) / 2));
            gui::Call("draw_triangle_colour", {x - radius, y, x, y - radius, x + radius, y, c, c, c, 0});
            gui::Call("draw_triangle_colour", {x - radius, y, x, y + radius, x + radius, y, c, c, c, 0});
        }
    }
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
    DrawBackdrop();
    SetWorldMatrix(control, GuiScale(), CameraLeft(), CameraTop());
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

// While open, the control's GUI draw (names, descriptions, SKILL POINTS) waits for WorldGuiEndHook.
void __cdecl ControlDrawGuiHook(yyc::CInstance* self, yyc::CInstance* other) {
    if (!g_open) g_origControlDrawGui(self, other);
}

void DrawOpenTree(yyc::CInstance* control) {
    __try {
        DrawTree(control);
        g_origControlDrawGui(control, control);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("skilltab: drawing the tree crashed (code 0x%08lX)", GetExceptionCode());
    }
}

// The world's last GUI draw (level-up banner, fades...): the tree goes over all of it, as its own room would.
void __cdecl WorldGuiEndHook(yyc::CInstance* self, yyc::CInstance* other) {
    g_origWorldGuiEnd(self, other);
    yyc::CInstance* control = g_open ? yyc::FindInstance(g_control) : nullptr;
    if (control != nullptr) DrawOpenTree(control);
}

bool Hook(uint32_t va, void* detour, void* original) {
    const MH_STATUS status = MH_CreateHook(yyc::At<void*>(va), detour, static_cast<void**>(original));
    if (status != MH_OK) Log("skilltab: hook @ VA %08X failed: %s", va, MH_StatusToString(status));
    return status == MH_OK;
}

// What LoadPlayer does in the tree's room, the part the world needs: each skill shows its state (image_index:
// available, learned). A skill with image_index 0 ignores the mouse (objSkill Step_0, line 0x126).
void ShowSkillStates() {
    yyc::RValue* unlocked = yyc::GlobalVar(kSlotSkillUnlocked);
    if (unlocked == nullptr || (unlocked->kind & yyc::kKindMask) != yyc::kArray) return;
    const int count = static_cast<int>(gui::Call("instance_number", {kObjSkill}));
    int shown = 0;
    for (int i = 0; i < count; ++i) {
        const int id = static_cast<int>(gui::Call("instance_find", {kObjSkill, double(i)}));
        yyc::CInstance* skill = yyc::FindInstance(id);
        if (skill == nullptr) continue;
        const double index = VarReal(id, kSlotSkill);
        if (index < 0) continue;
        const double state = yyc::ToReal(yyc::ArrayElement(unlocked, static_cast<int>(index)));
        if (state <= -1) continue;
        yyc::SetBuiltinReal(skill, yyc::va::YYVar_image_index, state);
        ++shown;
    }
    Log("skilltab: %d of %d skills available or learned", shown, count);
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
    g_cameraX = kGridWidth / 2;
    g_cameraY = kGridHeight / 2;
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
    if (g_open) ShowSkillStates();
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

void RefreshStates(yyc::CInstance* self) {
    if (!g_open || self == nullptr) return;
    gui::Begin(self);
    ShowSkillStates();
}

bool InstallHooks(bool (*inSession)()) {
    g_inSession = inSession;
    bool ok = Hook(kVariableGetBuiltin, reinterpret_cast<void*>(&GetBuiltinHook), &g_origGetBuiltin);
    ok = Hook(kScriptLoadAsync, reinterpret_cast<void*>(&LoadAsyncHook), &g_origLoadAsync) && ok;
    ok = Hook(ev::ControlDraw, reinterpret_cast<void*>(&ControlDrawHook), &g_origControlDraw) && ok;
    ok = Hook(ev::ControlDrawGui, reinterpret_cast<void*>(&ControlDrawGuiHook), &g_origControlDrawGui) && ok;
    ok = Hook(ev::SkillDraw, reinterpret_cast<void*>(&SkillDrawHook), &g_origSkillDraw) && ok;
    ok = Hook(ev::SkillStep, reinterpret_cast<void*>(&SkillStepHook), &g_origSkillStep) && ok;
    ok = Hook(ev::ControlStep, reinterpret_cast<void*>(&ControlStepHook), &g_origControlStep) && ok;
    ok = Hook(ev::WorldGuiEnd, reinterpret_cast<void*>(&WorldGuiEndHook), &g_origWorldGuiEnd) && ok;
    return ok;
}

bool InterceptRoomGoto(yyc::CInstance* self, int room) {
    // In a session the tree opens over the running world (PLAYTEST2.md #2): the game's own skill room would stop the
    // world for everyone while one player is in it. COOP_ROOM_SKILLTREE=1 goes back to the room.
    static const bool enabled = GetEnvironmentVariableA("COOP_ROOM_SKILLTREE", nullptr, 0) == 0;
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
    LogState(localPlayer);
    if (at == 0) return;
    ++steps;
    if (steps != at && steps != at + 1800) return;  // open for 30 s
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
