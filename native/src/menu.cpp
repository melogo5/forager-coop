#include "menu.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "MinHook.h"
#include "character.h"
#include "coop_button_png.h"
#include "coop_ui_png.h"
#include "debugport.h"
#include "gui.h"
#include "lan.h"
#include "log.h"
#include "mutant.h"
#include "session.h"
#include "steam.h"
#include "world.h"
#include "yyc.h"

namespace coop {

namespace {

constexpr unsigned kGuestLeaveSettleSteps = 60;  // SaveGame is asynchronous
unsigned g_guestLeaveSteps = 0;

// Main menu objects and events (decompiled/code/index.tsv, data/objects.tsv, data/sprites.tsv).
constexpr int kObjMenuButton = 249;  // objNewMainMenuButton
constexpr int kSprMenuPlay = 1329;   // sprMainMenuPlay: the PLAY button opens the save selection
constexpr int kSprMenuModding = 1327;
constexpr int kRoomGame = yyc::kRoomGame;
constexpr uint32_t kControlStep = 0x00E95C80;
constexpr uint32_t kControlDrawGui = 0x00E9BAA0;
constexpr uint32_t kButtonStep = 0x00EFCAC0;
constexpr uint32_t kButtonDraw = 0x00F01060;
constexpr uint32_t kButtonUser0 = 0x00EFFA20;

constexpr int kObjFeaturedFanArt = 254;           // the FEATURED FAN ART picture: COOP takes its place
constexpr double kCoopButtonAboveModding = 78;    // room pixels: the fan art's spot, above MODDING
constexpr int kSlotText = 0x866;                  // a menu button's `text` (var_slots.tsv)
constexpr double kCoopSpriteOriginX = 49, kCoopSpriteOriginY = 13;  // as sprMainMenuModding
// The COOP screen is made of the game's own pieces: its boxes and the main menu's button bodies.
constexpr int kSprTextBox = 1266;          // frames: 4 dark blue with a white rim, 5 dark blue with a gold rim
constexpr int kSprTextBoxDisabled = 1268;  // darker: 0 dark slate
constexpr double kBoxCorner = 8;           // sprite pixels kept whole at the corners of a box
constexpr double kBodyCap = 6;             // ... and at the ends of a button body
constexpr int kUiFrames = 4;
enum UiFrame { kUiGreen = 0, kUiPurple = 1, kUiGrey = 2, kUiPink = 3 };  // native/assets/coop_ui.png
constexpr size_t kMaxAddressLength = 21;    // "255.255.255.255:65535"
constexpr double kCoopButtonTextScale = 1;  // room pixels; the menu room is 320x180

namespace color {
constexpr int kShade = gui::Rgb(0, 0, 0);
constexpr int kPanel = gui::Rgb(38, 36, 58);
constexpr int kRow = gui::Rgb(58, 56, 88);
constexpr int kRowHover = gui::Rgb(78, 76, 118);
constexpr int kBorder = gui::Rgb(255, 196, 64);
constexpr int kGreen = gui::Rgb(92, 196, 72);
constexpr int kPurple = gui::Rgb(136, 84, 200);
constexpr int kPurpleLight = gui::Rgb(160, 110, 224);
constexpr int kButtonShadow = gui::Rgb(48, 26, 72);
constexpr int kGrey = gui::Rgb(110, 110, 130);
constexpr int kText = gui::Rgb(255, 255, 255);
constexpr int kDim = gui::Rgb(180, 180, 200);
constexpr int kPanelFloor = gui::Rgb(22, 24, 44);
constexpr int kButtonIdle = gui::Rgb(225, 225, 225);  // a button not under the mouse is a little darker
}  // namespace color

struct Layout {
    gui::Rect panel, create, back, ipBox, connect, nameBox;
    std::vector<gui::Rect> rows, joins;
    double scale;
};

yyc::EventFn g_origControlStep = nullptr;
yyc::EventFn g_origControlDrawGui = nullptr;
yyc::EventFn g_origButtonStep = nullptr;
yyc::EventFn g_origButtonDraw = nullptr;
yyc::EventFn g_origButtonUser0 = nullptr;

int g_coopButtonId = 0;
int g_coopSprite = -1;  // the COOP button's sprite (sprite_add); -1 not loaded yet, -2 could not be loaded
int g_uiSprite = -1;    // the COOP screen's button bodies; the same
bool g_open = false;
bool g_ipFocused = false;
std::string g_ipText;
bool g_nameFocused = false;
std::string g_nameText;  // the guest's name for the host (kept between games)
bool g_nameLoaded = false;
constexpr size_t kMaxNameLength = 16;
std::string g_status;
std::string g_notice;  // how the last session ended; shown when the COOP screen opens next
std::vector<lan::Server> g_servers;
bool g_loadingWorld = false;

bool IsCoopButton(yyc::CInstance* self) {
    return g_coopButtonId != 0 && yyc::GetBuiltinReal(self, yyc::va::YYVar_id) == g_coopButtonId;
}

yyc::CInstance* FindMenuButton(yyc::CInstance* self, int sprite) {
    gui::Begin(self);
    const int count = static_cast<int>(gui::Call("instance_number", {kObjMenuButton}));
    for (int i = 0; i < count; ++i) {
        const int id = static_cast<int>(gui::Call("instance_find", {kObjMenuButton, static_cast<double>(i)}));
        yyc::CInstance* button = yyc::FindInstance(id);
        if (button != nullptr && id != g_coopButtonId &&
            yyc::GetBuiltinReal(button, yyc::va::YYVar_sprite_index) == sprite) {
            return button;
        }
    }
    return nullptr;
}

// Writes an embedded PNG next to the saves, where the game's file functions look, and adds it as a sprite of
// `frames` frames side by side. The sprite index, or -1.
int AddSprite(yyc::CInstance* self, const unsigned char* png, size_t size, const char* file, int frames, double ox,
              double oy) {
    wchar_t local[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return -1;
    {
        const std::wstring path = std::wstring(local) + L"\\Forager\\" + std::wstring(file, file + strlen(file));
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(png), static_cast<std::streamsize>(size));
        if (!out) {
            Log("menu: cannot write %s", file);
            return -1;
        }
    }
    yyc::RValue args[6] = {yyc::String(file), yyc::Real(frames), yyc::Real(0), yyc::Real(0), yyc::Real(ox),
                           yyc::Real(oy)};
    yyc::RValue result{};
    yyc::CallBuiltin("sprite_add", self, 6, args, &result);
    const int sprite = static_cast<int>(yyc::ToReal(result));
    yyc::Free(&result);
    yyc::Free(&args[0]);
    Log("menu: sprite %s %s (%d)", file, sprite >= 0 ? "loaded" : "NOT loaded", sprite);
    return sprite >= 0 ? sprite : -1;
}

// The COOP button's own sprite, made like the game's menu buttons (native/assets/coop_button.png, embedded).
bool CoopSpriteReady(yyc::CInstance* self) {
    if (g_coopSprite == -1) {
        const int sprite = AddSprite(self, kCoopButtonPng, sizeof(kCoopButtonPng), "coop_button.png", 2,
                                     kCoopSpriteOriginX, kCoopSpriteOriginY);
        g_coopSprite = sprite >= 0 ? sprite : -2;
    }
    return g_coopSprite >= 0;
}

// The COOP screen's button bodies (native/assets/coop_ui.png, embedded).
bool UiSpriteReady(yyc::CInstance* self) {
    if (g_uiSprite == -1) {
        const int sprite = AddSprite(self, kCoopUiPng, sizeof(kCoopUiPng), "coop_ui.png", kUiFrames, 0, 0);
        g_uiSprite = sprite >= 0 ? sprite : -2;
    }
    return g_uiSprite >= 0;
}

// The FEATURED FAN ART picture goes: the COOP button stands in its place.
void RemoveFanArt() {
    if (gui::Call("instance_number", {kObjFeaturedFanArt}) > 0) gui::Call("instance_destroy", {kObjFeaturedFanArt});
}

// Once the button has set itself up as MODDING (its Alarm_0 fills `text`), it gets its own look: our sprite and,
// for languages the game writes over a blank button, the word.
void DressCoopButton(yyc::CInstance* control) {
    yyc::CInstance* button = g_coopButtonId != 0 ? yyc::FindInstance(g_coopButtonId) : nullptr;
    if (button == nullptr || !CoopSpriteReady(control)) return;
    if (yyc::GetBuiltinReal(button, yyc::va::YYVar_sprite_index) == g_coopSprite) return;
    yyc::RValue text{};
    yyc::GetVar(g_coopButtonId, kSlotText, &text);
    const bool setUp = (text.kind & yyc::kKindMask) == yyc::kString;
    yyc::Free(&text);
    if (!setUp) return;
    yyc::SetBuiltinReal(button, yyc::va::YYVar_sprite_index, g_coopSprite);
    yyc::RValue word = yyc::String("COOP");
    yyc::SetVar(g_coopButtonId, kSlotText, &word);
    yyc::Free(&word);
}

// A copy of the MODDING button in the fan art's place; its click is redirected to the Coop screen.
void EnsureCoopButton(yyc::CInstance* control) {
    if (g_coopButtonId != 0 && yyc::FindInstance(g_coopButtonId) != nullptr) return;
    g_coopButtonId = 0;
    yyc::CInstance* modding = FindMenuButton(control, kSprMenuModding);
    if (modding == nullptr) return;
    const double x = yyc::GetBuiltinReal(modding, yyc::va::YYVar_x);
    const double y = yyc::GetBuiltinReal(modding, yyc::va::YYVar_y) - kCoopButtonAboveModding;
    const double depth = yyc::GetBuiltinReal(modding, yyc::va::YYVar_depth);
    g_coopButtonId = static_cast<int>(gui::Call("instance_create_depth", {x, y, depth, kObjMenuButton}));
    if (yyc::CInstance* button = yyc::FindInstance(g_coopButtonId)) {
        // Before its Alarm_0 runs, so the button sizes itself like MODDING.
        yyc::SetBuiltinReal(button, yyc::va::YYVar_sprite_index, kSprMenuModding);
    }
    Log("menu: COOP button %d at (%.0f, %.0f)", g_coopButtonId, x, y);
}

Layout ComputeLayout() {
    Layout l{};
    const double w = gui::GuiWidth();
    const double h = gui::GuiHeight();
    l.scale = h / 720.0;
    const double s = l.scale;
    const double pw = (std::min)(960 * s, w - 40 * s);
    const double ph = (std::min)(600 * s, h - 40 * s);
    l.panel = {(w - pw) / 2, (h - ph) / 2, (w + pw) / 2, (h + ph) / 2};
    const gui::Rect& p = l.panel;
    // Buttons are 52 high: a button body (26 sprite pixels) at 2 screen pixels per sprite pixel, as the boxes.
    l.create = {p.x2 - 230 * s, p.y1 + 28 * s, p.x2 - 28 * s, p.y1 + 80 * s};
    l.back = {p.x1 + 28 * s, p.y2 - 80 * s, p.x1 + 204 * s, p.y2 - 28 * s};
    l.connect = {p.x2 - 230 * s, p.y2 - 80 * s, p.x2 - 28 * s, p.y2 - 28 * s};
    l.ipBox = {l.connect.x1 - 340 * s, l.connect.y1, l.connect.x1 - 16 * s, l.connect.y2};
    l.nameBox = {l.ipBox.x1, l.ipBox.y1 - 68 * s, l.ipBox.x2, l.ipBox.y1 - 16 * s};
    const double rowHeight = 72 * s;
    double y = p.y1 + 110 * s;
    for (size_t i = 0; i < g_servers.size() && y + rowHeight < l.back.y1 - 40 * s; ++i, y += rowHeight + 10 * s) {
        const gui::Rect row{p.x1 + 28 * s, y, p.x2 - 28 * s, y + rowHeight};
        l.rows.push_back(row);
        l.joins.push_back({row.x2 - 190 * s, row.y1 + 10 * s, row.x2 - 12 * s, row.y2 - 10 * s});
    }
    return l;
}

// Without the sprites (they could not be loaded) the screen is drawn in flat colours.
void FlatButton(const gui::Rect& r, const std::string& label, int fill, double mx, double my, double scale) {
    gui::Fill(r, fill, r.Contains(mx, my) ? 1.0 : 0.85);
    gui::Frame(r, color::kBorder);
    gui::Text((r.x1 + r.x2) / 2, (r.y1 + r.y2) / 2, label, scale, color::kText, gui::kAlignCenter);
}

void DrawFlat(const Layout& l) {
    const double s = l.scale;
    const double mx = gui::MouseX();
    const double my = gui::MouseY();
    const double textScale = 1.5 * s;

    gui::Fill({0, 0, gui::GuiWidth(), gui::GuiHeight()}, color::kShade, 0.6);
    gui::Fill(l.panel, color::kPanel);
    gui::Frame(l.panel, color::kBorder);
    gui::Text(l.panel.x1 + 32 * s, l.panel.y1 + 54 * s, "COOP", 3 * s, color::kBorder);
    FlatButton(l.create, "CREATE", color::kGreen, mx, my, textScale);

    if (g_servers.empty()) {
        gui::Text((l.panel.x1 + l.panel.x2) / 2, (l.panel.y1 + l.back.y1) / 2,
                  "Looking for servers in your network...", textScale, color::kDim, gui::kAlignCenter);
    }
    for (size_t i = 0; i < l.rows.size(); ++i) {
        const gui::Rect& row = l.rows[i];
        const lan::Server& server = g_servers[i];
        gui::Fill(row, row.Contains(mx, my) ? color::kRowHover : color::kRow);
        const double cy = (row.y1 + row.y2) / 2;
        gui::Text(row.x1 + 20 * s, cy - 14 * s, server.hostName, textScale, color::kText);
        gui::Text(row.x1 + 20 * s, cy + 16 * s, server.worldName + "   " + server.address, 1.5 * s, color::kDim);
        gui::Text(l.joins[i].x1 - 30 * s, cy, std::to_string(server.players) + "/" + std::to_string(server.capacity), textScale, color::kText,
                  gui::kAlignRight);
        FlatButton(l.joins[i], "JOIN", server.players < server.capacity ? color::kGreen : color::kGrey, mx, my, textScale);
    }

    FlatButton(l.back, "BACK", color::kPurple, mx, my, textScale);
    gui::Text(l.ipBox.x1 - 16 * s, (l.ipBox.y1 + l.ipBox.y2) / 2, "IP:", textScale, color::kDim, gui::kAlignRight);
    gui::Fill(l.ipBox, g_ipFocused ? color::kRowHover : color::kRow);
    gui::Frame(l.ipBox, g_ipFocused ? color::kText : color::kBorder);
    const bool caret = g_ipFocused && (GetTickCount() / 500) % 2 == 0;
    gui::Text(l.ipBox.x1 + 14 * s, (l.ipBox.y1 + l.ipBox.y2) / 2, g_ipText + (caret ? "_" : ""), textScale,
              color::kText);
    FlatButton(l.connect, "CONNECT", color::kGreen, mx, my, textScale);
    gui::Text(l.nameBox.x1 - 16 * s, (l.nameBox.y1 + l.nameBox.y2) / 2, "NAME:", textScale, color::kDim,
              gui::kAlignRight);
    gui::Fill(l.nameBox, g_nameFocused ? color::kRowHover : color::kRow);
    gui::Frame(l.nameBox, g_nameFocused ? color::kText : color::kBorder);
    const bool nameCaret = g_nameFocused && (GetTickCount() / 500) % 2 == 0;
    gui::Text(l.nameBox.x1 + 14 * s, (l.nameBox.y1 + l.nameBox.y2) / 2, g_nameText + (nameCaret ? "_" : ""),
              textScale, color::kText);
    if (!g_status.empty()) {
        gui::Text((l.panel.x1 + l.panel.x2) / 2, l.nameBox.y1 - 26 * s, g_status, 1.5 * s, color::kBorder,
                  gui::kAlignCenter);
    }
}

void SpriteButton(const gui::Rect& r, const std::string& label, int frame, double mx, double my, double textScale) {
    gui::Bar3(g_uiSprite, frame, r, kBodyCap, r.Contains(mx, my) ? gui::kWhite : color::kButtonIdle);
    // The body's face is its upper part (a darker rim below): the word sits a little above the middle.
    gui::Text((r.x1 + r.x2) / 2, r.y1 + (r.y2 - r.y1) * 0.42, label, textScale, color::kText, gui::kAlignCenter);
}

// A text field: the game's dark box, with a white rim while typing into it. (The game font has its own outline.)
void Field(const gui::Rect& r, const std::string& label, const std::string& text, bool focused, double k,
           double textScale) {
    const double s = k / 2;
    gui::Text(r.x1 - 16 * s, (r.y1 + r.y2) / 2, label, textScale, color::kDim,
                      gui::kAlignRight);
    gui::Box9(kSprTextBox, focused ? 4 : 5, r, kBoxCorner, k);
    const bool caret = focused && (GetTickCount() / 500) % 2 == 0;
    gui::Text(r.x1 + 18 * s, (r.y1 + r.y2) / 2, text + (caret ? "_" : ""), textScale, color::kText);
}

// The game's look: its boxes, the main menu's button bodies, the COOP button as the title.
void DrawSprites(const Layout& l) {
    const double s = l.scale;
    const double k = 2 * s;  // screen pixels per sprite pixel
    const double mx = gui::MouseX();
    const double my = gui::MouseY();
    const double textScale = 1.5 * s;

    gui::Fill({0, 0, gui::GuiWidth(), gui::GuiHeight()}, color::kShade, 0.6);
    // The box's middle is see-through (the game lays it over things to darken them): a solid floor under it.
    gui::Fill({l.panel.x1 + 2 * k, l.panel.y1 + 2 * k, l.panel.x2 - 2 * k, l.panel.y2 - 2 * k}, color::kPanelFloor);
    gui::Box9(kSprTextBox, 5, l.panel, kBoxCorner, k);
    // Title: the COOP button itself (its origin is its centre).
    gui::Call("draw_sprite_ext", {double(g_coopSprite), 0, l.panel.x1 + 40 * s + kCoopSpriteOriginX * k,
                                  l.create.y1 + kCoopSpriteOriginY * k, k, k, 0, gui::kWhite, 1});
    SpriteButton(l.create, "CREATE", kUiGreen, mx, my, textScale);

    if (g_servers.empty()) {
        gui::Text((l.panel.x1 + l.panel.x2) / 2, (l.panel.y1 + l.back.y1) / 2,
                          "Looking for servers in your network...", textScale, color::kDim,
                          gui::kAlignCenter);
    }
    for (size_t i = 0; i < l.rows.size(); ++i) {
        const gui::Rect& row = l.rows[i];
        const lan::Server& server = g_servers[i];
        const bool hover = row.Contains(mx, my);
        gui::Box9(hover ? kSprTextBox : kSprTextBoxDisabled, hover ? 4 : 0, row, kBoxCorner, k);
        const double cy = (row.y1 + row.y2) / 2;
        gui::Text(row.x1 + 24 * s, cy - 14 * s, server.hostName, textScale, color::kText);
        gui::Text(row.x1 + 24 * s, cy + 16 * s, server.worldName + "   " + server.address, 1.5 * s,
                          color::kDim);
        const bool room = server.players < server.capacity;
        gui::Text(l.joins[i].x1 - 30 * s, cy,
                          std::to_string(server.players) + "/" + std::to_string(server.capacity), textScale,
                          room ? color::kText : color::kDim, gui::kAlignRight);
        SpriteButton(l.joins[i], room ? "JOIN" : "FULL", room ? kUiGreen : kUiGrey, mx, my, textScale);
    }

    SpriteButton(l.back, "BACK", kUiPurple, mx, my, textScale);
    Field(l.nameBox, "NAME:", g_nameText, g_nameFocused, k, textScale);
    Field(l.ipBox, "IP:", g_ipText, g_ipFocused, k, textScale);
    SpriteButton(l.connect, "CONNECT", kUiGreen, mx, my, textScale);
    if (!g_status.empty()) {
        gui::Text((l.panel.x1 + l.panel.x2) / 2, l.nameBox.y1 - 26 * s, g_status, 1.5 * s,
                          color::kBorder, gui::kAlignCenter);
    }
}

void Draw(yyc::CInstance* self) {
    gui::Begin(self);
    const Layout l = ComputeLayout();
    if (CoopSpriteReady(self) && UiSpriteReady(self)) {
        DrawSprites(l);
    } else {
        DrawFlat(l);
    }
    gui::End();
}

// A session that never reached a world - a join still connecting (a mistyped IP retried for good), or CREATE and
// then back out of the save screen - ends, so the player can start another one (it took a game restart before).
// False if a session that did reach a world is still on.
bool CancelUnplayedSession(const char* why) {
    if (session::CurrentRole() == session::Role::kNone) return true;
    if (session::PlayedWorld() || g_loadingWorld) return false;
    Log("menu: %s - the session that never reached a world is cancelled", why);
    const bool guest = session::CurrentRole() == session::Role::kGuest;
    session::Leave();
    if (guest) world::RemoveGuestSaveSlot();  // what had come of the host's world
    return true;
}

// Accepted a Steam invite: join that host through Steam. The name is the one typed in the COOP screen last (or
// the computer's), as for any join.
void StartJoinSteam(uint64_t host) {
    if (!CancelUnplayedSession("a Steam invite")) {
        Log("menu: a Steam invite, but already in a session");
        return;
    }
    character::SetGuestName(g_nameText.empty() ? character::SavedGuestName() : g_nameText);
    session::JoinSteam(host);
    g_open = true;  // the COOP screen tells how the join goes
    g_status = "Joining your friend through Steam...";
}

void StartJoin(const std::string& address, uint16_t port) {
    if (!CancelUnplayedSession("another join")) {
        g_status = "Already in a session - restart the game to join another one";
        return;
    }
    if (g_nameText.empty()) {
        g_status = "Enter your name first - the host keeps your character under it";
        return;
    }
    character::SetGuestName(g_nameText);
    session::Join(address, port);
    g_status = "Connecting to " + address + "...";
}

void ConnectByIp() {
    std::string address = g_ipText;
    uint16_t port = kGamePort;
    const size_t colon = address.find(':');
    if (colon != std::string::npos) {
        port = static_cast<uint16_t>(atoi(address.c_str() + colon + 1));
        address.resize(colon);
    }
    if (address.empty() || port == 0) {
        g_status = "Enter the host's IP, e.g. 25.12.34.56";
        return;
    }
    StartJoin(address, port);
}

// CREATE: host whatever the player picks on the game's own save screen (an existing save or a new world).
// Testing aid: COOP_DEBUG_HOST_SLOT=<n> hosts save slot n (saveData<n>) straight away, without the save screen
// (its rows move with every world played, so a test cannot count on a row).
int DebugHostSlot() {
    char value[16] = {};
    return GetEnvironmentVariableA("COOP_DEBUG_HOST_SLOT", value, sizeof(value)) > 0 ? atoi(value) : -1;
}

void CreateServer(yyc::CInstance* control) {
    yyc::CInstance* play = FindMenuButton(control, kSprMenuPlay);
    if (play == nullptr || !CancelUnplayedSession("CREATE")) return;
    session::Host();
    g_open = false;
    yyc::RValue* saveFile = yyc::GlobalVar(yyc::slot::saveFile);
    if (const int slot = DebugHostSlot(); slot >= 0 && saveFile != nullptr) {
        yyc::Free(saveFile);
        *saveFile = yyc::Real(slot);
        gui::Begin(control);
        gui::Call("room_goto", {kRoomGame});
        Log("menu: debug: hosting save slot %d", slot);
        return;
    }
    yyc::PerformEvent(play, play, yyc::kEvOther, yyc::kEvUser0);
}

void LoadGuestWorld(yyc::CInstance* control) {
    yyc::RValue* saveFile = yyc::GlobalVar(yyc::slot::saveFile);
    if (saveFile == nullptr) return;
    yyc::Free(saveFile);
    *saveFile = yyc::Real(world::kGuestSlot);
    g_loadingWorld = true;
    g_open = false;
    gui::Begin(control);
    gui::Call("room_goto", {kRoomGame});
    Log("menu: loading the host world from slot %d", world::kGuestSlot);
}

void UpdateNameInput() {
    if (!g_nameFocused) return;
    std::string typed = gui::TypedText();
    typed.erase(std::remove_if(typed.begin(), typed.end(),
                               [](char c) {
                                   return !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                            (c >= '0' && c <= '9') || c == '-' || c == '_');
                               }),
                typed.end());
    if (typed.size() > kMaxNameLength) typed.resize(kMaxNameLength);
    g_nameText = typed;
}

void UpdateIpInput() {
    UpdateNameInput();
    if (!g_ipFocused) return;
    std::string typed = gui::TypedText();
    typed.erase(std::remove_if(typed.begin(), typed.end(),
                               [](char c) { return !((c >= '0' && c <= '9') || c == '.' || c == ':'); }),
                typed.end());
    if (typed.size() > kMaxAddressLength) typed.resize(kMaxAddressLength);
    g_ipText = typed;
}

// Back/Esc: leaving the screen while still connecting gives the join up.
void CloseScreen() {
    if (session::CurrentRole() == session::Role::kGuest) CancelUnplayedSession("Back while connecting");
    g_open = false;
}

void HandleInput(yyc::CInstance* control) {
    UpdateIpInput();
    if (gui::KeyPressed(VK_ESCAPE)) {
        if (g_ipFocused || g_nameFocused) {
            g_ipFocused = g_nameFocused = false;
        } else {
            CloseScreen();
        }
        return;
    }
    if (g_nameFocused && gui::KeyPressed(VK_RETURN)) {
        g_nameFocused = false;
        return;
    }
    if (g_ipFocused && gui::KeyPressed(VK_RETURN)) {
        g_ipFocused = false;
        ConnectByIp();
        return;
    }
    if (!gui::MousePressed()) return;
    const Layout l = ComputeLayout();
    const double mx = gui::MouseX();
    const double my = gui::MouseY();
    g_ipFocused = l.ipBox.Contains(mx, my);
    g_nameFocused = l.nameBox.Contains(mx, my);
    if (g_ipFocused) {
        gui::SetTypedText(g_ipText);
    } else if (g_nameFocused) {
        gui::SetTypedText(g_nameText);
    } else if (l.create.Contains(mx, my)) {
        CreateServer(control);
    } else if (l.back.Contains(mx, my)) {
        CloseScreen();
    } else if (l.connect.Contains(mx, my)) {
        ConnectByIp();
    } else {
        for (size_t i = 0; i < l.joins.size(); ++i) {
            if (l.joins[i].Contains(mx, my) && g_servers[i].players < g_servers[i].capacity) {
                StartJoin(g_servers[i].address, g_servers[i].port);
            }
        }
    }
}

// Testing aid: COOP_DEBUG_DUMPSPRITES=1 saves the main menu's sprites as PNGs (one per frame) into
// %LOCALAPPDATA%\Forager (coop_sprite_*.png), the material the COOP button's sprite is made from. Drawn onto a
// surface and saved from it (sprite_save does nothing for sprites on texture pages). Draw event only.
void DumpSprites(yyc::CInstance* control) {
    static bool done = false;
    if (done || GetEnvironmentVariableA("COOP_DEBUG_DUMPSPRITES", nullptr, 0) == 0) return;
    done = true;
    gui::Begin(control);
    const std::pair<const char*, int> sprites[] = {
        {"sprPlayerIdle", 0},          {"sprMainMenuExit", 1325},    {"sprMainMenuExtras", 1326},
        {"sprMainMenuModding", 1327},  {"sprMainMenuOptions", 1328}, {"sprMainMenuPlay", 1329},
        {"sprMainMenuRoadmap", 1330},  {"sprMainMenuCommunity", 1331}, {"sprMainMenuCredits", 1333},
        {"sprTextBox", 1266},          {"sprTextBoxDisabled", 1268}, {"sprSaveSlot", 1285},
        {"sprSaveSlotOverlay", 1286},  {"sprMainMenuButton", 1287},  {"sprMainMenuIcons", 1292},
        {"sprMenuTabWindow", 1222},    {"sprInventorySlot", 1243},   {"sprCheckbox", 1283},
        {"sprHeart", 1269},
    };
    for (const auto& [name, index] : sprites) {
        const double sprite = index;
        const int frames = static_cast<int>(gui::Call("sprite_get_number", {sprite}));
        const double w = gui::Call("sprite_get_width", {sprite}), h = gui::Call("sprite_get_height", {sprite});
        const double ox = gui::Call("sprite_get_xoffset", {sprite}), oy = gui::Call("sprite_get_yoffset", {sprite});
        Log("menu: sprite %s: %d frames, %gx%g, origin (%g, %g)", name, frames, w, h, ox, oy);
        for (int i = 0; i < frames; ++i) {
            const double surface = gui::Call("surface_create", {w, h});
            gui::Call("surface_set_target", {surface});
            gui::Call("draw_clear_alpha", {0, 0});
            gui::Call("draw_sprite", {sprite, double(i), ox, oy});
            gui::Call("surface_reset_target", {});
            const std::string file = std::string("coop_sprite_") + name + "_" + std::to_string(i) + ".png";
            yyc::RValue args[2] = {yyc::Real(surface), yyc::String(file.c_str())};
            yyc::RValue result{};
            yyc::CallBuiltin("surface_save", control, 2, args, &result);
            yyc::Free(&result);
            yyc::Free(&args[1]);
            gui::Call("surface_free", {surface});
        }
    }
    Log("menu: debug sprites saved");
}

void Step(yyc::CInstance* control) {
    gui::Begin(control);
    steam::Init(GetCommandLineA());
    steam::DebugTick();
    if (const uint64_t host = steam::TakeJoinRequest()) StartJoinSteam(host);
    if (session::CurrentRole() == session::Role::kHost) lan::SetAnnouncement(false, "", 1, 2);  // back in the menu
    RemoveFanArt();
    EnsureCoopButton(control);
    DressCoopButton(control);
    // Back in the main menu after playing, the session is over: the host tells the guest, and both can start
    // a new one. A guest's working copy of the host's world (slot 99) goes, so it never sits in the player's
    // own save list.
    if (session::CurrentRole() != session::Role::kNone && session::PlayedWorld()) {
        if (session::CurrentRole() == session::Role::kGuest) {
            // "Save and exit" has just saved the guest's character into slot 99 (asynchronously): give it a
            // moment, then send it to the host, so nothing taken in the last seconds is lost (PLAYTEST.md #2).
            if (++g_guestLeaveSteps < kGuestLeaveSettleSteps) return;
            g_guestLeaveSteps = 0;
            character::GuestUploadNow();
            Log("menu: the guest's last save sent to the host");
            world::RemoveGuestSaveSlot();
        } else if (!session::HostFarewellDone()) {
            return;
        }
        session::Leave();
        g_loadingWorld = false;
        g_notice = session::TakeNotice();
        g_status = g_notice;
    }
    if (session::CurrentRole() == session::Role::kGuest && !g_loadingWorld) {
        if (world::GuestWorldReady() && character::GuestReady()) {
            LoadGuestWorld(control);
            return;
        }
        if (session::IsConnected()) g_status = "Connected - receiving the host's world...";
    }
    if (!g_open) return;
    g_servers = lan::Servers();
    HandleInput(control);
}

void __cdecl ControlStepHook(yyc::CInstance* self, yyc::CInstance* other) {
    g_origControlStep(self, other);
    debugport::Tick(self);  // the query port answers in the menus too (`self` is the menu, not a player)
    __try {
        Step(self);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("menu: step crashed (code 0x%08lX)", GetExceptionCode());
        g_open = false;
    }
}

void __cdecl ControlDrawGuiHook(yyc::CInstance* self, yyc::CInstance* other) {
    g_origControlDrawGui(self, other);
    DumpSprites(self);
    if (!g_open) return;
    __try {
        Draw(self);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("menu: draw crashed (code 0x%08lX)", GetExceptionCode());
        g_open = false;
    }
}

// While the Coop screen is open the main menu buttons must not react.
void __cdecl ButtonStepHook(yyc::CInstance* self, yyc::CInstance* other) {
    if (!g_open) g_origButtonStep(self, other);
}

// Without its sprite (it could not be loaded) the COOP button is drawn here instead: the menu buttons' words are
// part of their sprites (room coordinates, over the MODDING-sized collision box it inherited).
void DrawCoopButton(yyc::CInstance* self) {
    gui::Begin(self);
    const gui::Rect box{yyc::GetBuiltinReal(self, yyc::va::YYVar_bbox_left),
                        yyc::GetBuiltinReal(self, yyc::va::YYVar_bbox_top),
                        yyc::GetBuiltinReal(self, yyc::va::YYVar_bbox_right),
                        yyc::GetBuiltinReal(self, yyc::va::YYVar_bbox_bottom)};
    const bool hover = box.Contains(yyc::GetBuiltinReal(self, yyc::va::YYVar_mouse_x),
                                    yyc::GetBuiltinReal(self, yyc::va::YYVar_mouse_y));
    gui::Fill({box.x1 + 1, box.y1 + 2, box.x2 + 1, box.y2 + 2}, color::kButtonShadow);
    gui::Fill(box, hover ? color::kPurpleLight : color::kPurple);
    gui::Frame(box, color::kButtonShadow);
    gui::Text((box.x1 + box.x2) / 2, (box.y1 + box.y2) / 2, "COOP", kCoopButtonTextScale, color::kText,
              gui::kAlignCenter);
    gui::End();
}

void __cdecl ButtonDrawHook(yyc::CInstance* self, yyc::CInstance* other) {
    if (!IsCoopButton(self)) {
        g_origButtonDraw(self, other);
        return;
    }
    if (g_coopSprite >= 0) {
        // Drawn by the game like the other buttons - once it wears our sprite (until then it would say MODDING).
        if (yyc::GetBuiltinReal(self, yyc::va::YYVar_sprite_index) == g_coopSprite) g_origButtonDraw(self, other);
        return;
    }
    __try {
        DrawCoopButton(self);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("menu: COOP button draw crashed (code 0x%08lX)", GetExceptionCode());
    }
}

void __cdecl ButtonUser0Hook(yyc::CInstance* self, yyc::CInstance* other) {
    if (!IsCoopButton(self)) {
        g_origButtonUser0(self, other);
        return;
    }
    g_open = true;
    if (!g_nameLoaded) {
        g_nameText = character::SavedGuestName();
        g_nameLoaded = true;
    }
    Log("menu: COOP screen opened");
    // Back here as a host that never got into a world: it backed out of the save screen.
    if (session::CurrentRole() == session::Role::kHost) CancelUnplayedSession("back in the menu after CREATE");
    g_ipFocused = false;
    g_status = g_notice;  // empty unless a session just ended for a reason worth telling
    g_notice.clear();
    lan::StartListening();
}

// COOP -> CREATE -> "+": coop worlds are Classic only (session.cpp sends a host of another mode back to the menu),
// so the mode screen (uiModeSelect) shows only Classic. Its other mode buttons - LUI nodes, which are instances - are
// faded out, shrunk to nothing and made inert. They stay on modeButtonList: taken off it, a click on Classic no
// longer opened its PLAY panel. Arrow keys can still land on a hidden mode; session.cpp then turns the world away.
constexpr uint32_t kModeSelectCreate = 0x00D47440;  // gml_Object_uiModeSelect_Create_0
constexpr int kSlotOtherModes[] = {0x7B2, 0x7F0, 0x556};  // singleIslandButton, speedRunButton, hardModeButton
constexpr int kSlotNodeHidden[] = {0x55, 0x815, 0x913, 0x91C, 0x820, 0x822, 0x5A3};  // alpha, startAlpha, xscale,
                                                                                    // yscale, startX/Yscale, interactable
yyc::EventFn g_origModeSelectCreate = nullptr;

void __cdecl ModeSelectCreateHook(yyc::CInstance* self, yyc::CInstance* other) {
    g_origModeSelectCreate(self, other);
    if (session::CurrentRole() != session::Role::kHost || Mutant("modes")) return;
    const int screen = static_cast<int>(yyc::GetBuiltinReal(self, yyc::va::YYVar_id));
    int hidden = 0;
    for (const int slot : kSlotOtherModes) {
        yyc::RValue nodeValue{};
        const bool haveNode = yyc::GetVar(screen, slot, &nodeValue) && yyc::IsNumber(nodeValue);
        const int node = haveNode ? static_cast<int>(yyc::ToReal(nodeValue)) : 0;
        yyc::Free(&nodeValue);
        if (yyc::FindInstance(node) == nullptr) continue;
        for (const int var : kSlotNodeHidden) {
            yyc::RValue zero = yyc::Real(0);
            yyc::SetVar(node, var, &zero);
        }
        ++hidden;
    }
    Log("menu: mode screen for a coop world - %d modes other than Classic hidden", hidden);
}

bool Hook(uint32_t va, void* detour, void* original) {
    const MH_STATUS status = MH_CreateHook(yyc::At<void*>(va), detour, static_cast<void**>(original));
    if (status != MH_OK) Log("menu: hook @ VA %08X failed: %s", va, MH_StatusToString(status));
    return status == MH_OK;
}

}  // namespace

bool MenuInstall() {
    bool ok = Hook(kControlStep, reinterpret_cast<void*>(&ControlStepHook), &g_origControlStep);
    ok = Hook(kControlDrawGui, reinterpret_cast<void*>(&ControlDrawGuiHook), &g_origControlDrawGui) && ok;
    ok = Hook(kButtonStep, reinterpret_cast<void*>(&ButtonStepHook), &g_origButtonStep) && ok;
    ok = Hook(kButtonDraw, reinterpret_cast<void*>(&ButtonDrawHook), &g_origButtonDraw) && ok;
    ok = Hook(kButtonUser0, reinterpret_cast<void*>(&ButtonUser0Hook), &g_origButtonUser0) && ok;
    ok = Hook(kModeSelectCreate, reinterpret_cast<void*>(&ModeSelectCreateHook), &g_origModeSelectCreate) && ok;
    return ok;
}

}  // namespace coop
