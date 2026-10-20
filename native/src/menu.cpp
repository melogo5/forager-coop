#include "menu.h"

#include <windows.h>

#include <algorithm>
#include <string>
#include <vector>

#include "MinHook.h"
#include "character.h"
#include "gui.h"
#include "lan.h"
#include "log.h"
#include "session.h"
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
constexpr int kRoomGame = 8;  // rmGame
constexpr uint32_t kControlStep = 0x00E95C80;
constexpr uint32_t kControlDrawGui = 0x00E9BAA0;
constexpr uint32_t kButtonStep = 0x00EFCAC0;
constexpr uint32_t kButtonDraw = 0x00F01060;
constexpr uint32_t kButtonUser0 = 0x00EFFA20;

constexpr double kCoopButtonOffsetX = -96;  // room pixels: centred between EXIT and MODDING
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

// A copy of the MODDING button next to it; its click is redirected to the Coop screen.
void EnsureCoopButton(yyc::CInstance* control) {
    if (g_coopButtonId != 0 && yyc::FindInstance(g_coopButtonId) != nullptr) return;
    g_coopButtonId = 0;
    yyc::CInstance* modding = FindMenuButton(control, kSprMenuModding);
    if (modding == nullptr) return;
    const double x = yyc::GetBuiltinReal(modding, yyc::va::YYVar_x) + kCoopButtonOffsetX;
    const double y = yyc::GetBuiltinReal(modding, yyc::va::YYVar_y);
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
    l.create = {p.x2 - 230 * s, p.y1 + 24 * s, p.x2 - 24 * s, p.y1 + 84 * s};
    l.back = {p.x1 + 24 * s, p.y2 - 84 * s, p.x1 + 200 * s, p.y2 - 24 * s};
    l.connect = {p.x2 - 230 * s, p.y2 - 84 * s, p.x2 - 24 * s, p.y2 - 24 * s};
    l.ipBox = {l.connect.x1 - 340 * s, l.connect.y1, l.connect.x1 - 16 * s, l.connect.y2};
    l.nameBox = {l.ipBox.x1, l.ipBox.y1 - 76 * s, l.ipBox.x2, l.ipBox.y1 - 16 * s};
    const double rowHeight = 72 * s;
    double y = p.y1 + 110 * s;
    for (size_t i = 0; i < g_servers.size() && y + rowHeight < l.back.y1 - 40 * s; ++i, y += rowHeight + 10 * s) {
        const gui::Rect row{p.x1 + 24 * s, y, p.x2 - 24 * s, y + rowHeight};
        l.rows.push_back(row);
        l.joins.push_back({row.x2 - 190 * s, row.y1 + 10 * s, row.x2 - 12 * s, row.y2 - 10 * s});
    }
    return l;
}

void Button(const gui::Rect& r, const std::string& label, int fill, double mx, double my, double scale) {
    gui::Fill(r, fill, r.Contains(mx, my) ? 1.0 : 0.85);
    gui::Frame(r, color::kBorder);
    gui::Text((r.x1 + r.x2) / 2, (r.y1 + r.y2) / 2, label, scale, color::kText, gui::kAlignCenter);
}

void Draw(yyc::CInstance* self) {
    gui::Begin(self);
    const Layout l = ComputeLayout();
    const double s = l.scale;
    const double mx = gui::MouseX();
    const double my = gui::MouseY();
    const double textScale = 1.5 * s;

    gui::Fill({0, 0, gui::GuiWidth(), gui::GuiHeight()}, color::kShade, 0.6);
    gui::Fill(l.panel, color::kPanel);
    gui::Frame(l.panel, color::kBorder);
    gui::Text(l.panel.x1 + 32 * s, l.panel.y1 + 54 * s, "COOP", 3 * s, color::kBorder);
    Button(l.create, "CREATE", color::kGreen, mx, my, textScale);

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
        Button(l.joins[i], "JOIN", server.players < server.capacity ? color::kGreen : color::kGrey, mx, my, textScale);
    }

    Button(l.back, "BACK", color::kPurple, mx, my, textScale);
    gui::Text(l.ipBox.x1 - 16 * s, (l.ipBox.y1 + l.ipBox.y2) / 2, "IP:", textScale, color::kDim, gui::kAlignRight);
    gui::Fill(l.ipBox, g_ipFocused ? color::kRowHover : color::kRow);
    gui::Frame(l.ipBox, g_ipFocused ? color::kText : color::kBorder);
    const bool caret = g_ipFocused && (GetTickCount() / 500) % 2 == 0;
    gui::Text(l.ipBox.x1 + 14 * s, (l.ipBox.y1 + l.ipBox.y2) / 2, g_ipText + (caret ? "_" : ""), textScale,
              color::kText);
    Button(l.connect, "CONNECT", color::kGreen, mx, my, textScale);
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
    gui::End();
}

void StartJoin(const std::string& address, uint16_t port) {
    if (session::CurrentRole() != session::Role::kNone) {
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
void CreateServer(yyc::CInstance* control) {
    yyc::CInstance* play = FindMenuButton(control, kSprMenuPlay);
    if (play == nullptr) return;
    session::Host();
    g_open = false;
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

void HandleInput(yyc::CInstance* control) {
    UpdateIpInput();
    if (gui::KeyPressed(VK_ESCAPE)) {
        if (g_ipFocused || g_nameFocused) {
            g_ipFocused = g_nameFocused = false;
        } else {
            g_open = false;
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
        g_open = false;
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

void Step(yyc::CInstance* control) {
    gui::Begin(control);
    if (session::CurrentRole() == session::Role::kHost) lan::SetAnnouncement(false, "", 1, 2);  // back in the menu
    EnsureCoopButton(control);
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
    __try {
        Step(self);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("menu: step crashed (code 0x%08lX)", GetExceptionCode());
        g_open = false;
    }
}

void __cdecl ControlDrawGuiHook(yyc::CInstance* self, yyc::CInstance* other) {
    g_origControlDrawGui(self, other);
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

// The menu buttons' words are part of their sprites, so the COOP button is drawn here instead
// (room coordinates, over the MODDING-sized collision box it inherited).
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
    g_ipFocused = false;
    g_status = g_notice;  // empty unless a session just ended for a reason worth telling
    g_notice.clear();
    lan::StartListening();
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
    return ok;
}

}  // namespace coop
