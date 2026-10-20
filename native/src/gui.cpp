#include "gui.h"

#include <vector>

namespace coop::gui {

namespace {

constexpr uint32_t kScriptGetFont = 0x007F9E00;    // GetFont() -> the UI font for the current language
constexpr uint32_t kScriptDrawReset = 0x00789460;  // DrawReset()
constexpr int kMouseLeft = 1;                      // mb_left
constexpr double kFontCellOffset = 9;              // measured: glyph centre is this far above the cell centre

yyc::CInstance* g_self = nullptr;

void CallScript(uint32_t va, yyc::RValue* result) {
    yyc::At<yyc::ScriptFn>(va)(g_self, g_self, result, 0, nullptr);
}

double CallWith(const char* builtin, std::vector<yyc::RValue>& args) {
    yyc::RValue result{};
    yyc::CallBuiltin(builtin, g_self, static_cast<int>(args.size()), args.data(), &result);
    const double value = yyc::ToReal(result);
    yyc::Free(&result);
    return value;
}

}  // namespace

void Begin(yyc::CInstance* self) { g_self = self; }

double Call(const char* builtin, std::initializer_list<double> args) {
    std::vector<yyc::RValue> values;
    for (const double a : args) values.push_back(yyc::Real(a));
    return CallWith(builtin, values);
}

double GuiWidth() { return Call("display_get_gui_width", {}); }
double GuiHeight() { return Call("display_get_gui_height", {}); }
double MouseX() { return Call("device_mouse_x_to_gui", {0}); }
double MouseY() { return Call("device_mouse_y_to_gui", {0}); }
bool MousePressed() { return Call("mouse_check_button_pressed", {kMouseLeft}) != 0; }
bool KeyPressed(int virtualKey) { return Call("keyboard_check_pressed", {static_cast<double>(virtualKey)}) != 0; }

void Fill(const Rect& r, int color, double alpha) {
    Call("draw_set_alpha", {alpha});
    Call("draw_set_colour", {static_cast<double>(color)});
    Call("draw_rectangle", {r.x1, r.y1, r.x2, r.y2, 0});
    Call("draw_set_alpha", {1});
}

void Frame(const Rect& r, int color, double alpha) {
    Call("draw_set_alpha", {alpha});
    Call("draw_set_colour", {static_cast<double>(color)});
    Call("draw_rectangle", {r.x1, r.y1, r.x2, r.y2, 1});
    Call("draw_set_alpha", {1});
}

void SpritePart(int sprite, int frame, double left, double top, double w, double h, double x, double y,
                double xscale, double yscale, int blend) {
    if (w <= 0 || h <= 0 || xscale <= 0 || yscale <= 0) return;
    Call("draw_sprite_part_ext", {double(sprite), double(frame), left, top, w, h, x, y, xscale, yscale,
                                  double(blend), 1});
}

void Box9(int sprite, int frame, const Rect& r, double corner, double scale, int blend) {
    const double sw = Call("sprite_get_width", {double(sprite)});
    const double sh = Call("sprite_get_height", {double(sprite)});
    const double c = corner * scale;  // a corner on screen
    const double midW = r.x2 - r.x1 - 2 * c, midH = r.y2 - r.y1 - 2 * c;
    const double xs = midW / (sw - 2 * corner), ys = midH / (sh - 2 * corner);
    const double cols[3][3] = {{0, r.x1, scale}, {corner, r.x1 + c, xs}, {sw - corner, r.x2 - c, scale}};
    const double rows[3][3] = {{0, r.y1, scale}, {corner, r.y1 + c, ys}, {sh - corner, r.y2 - c, scale}};
    for (int i = 0; i < 3; ++i) {
        const double partH = i == 1 ? sh - 2 * corner : corner;
        for (int j = 0; j < 3; ++j) {
            const double partW = j == 1 ? sw - 2 * corner : corner;
            SpritePart(sprite, frame, cols[j][0], rows[i][0], partW, partH, cols[j][1], rows[i][1], cols[j][2],
                       rows[i][2], blend);
        }
    }
}

void Bar3(int sprite, int frame, const Rect& r, double cap, int blend) {
    const double sw = Call("sprite_get_width", {double(sprite)});
    const double sh = Call("sprite_get_height", {double(sprite)});
    const double scale = (r.y2 - r.y1) / sh;
    const double c = cap * scale;
    const double xs = (r.x2 - r.x1 - 2 * c) / (sw - 2 * cap);
    SpritePart(sprite, frame, 0, 0, cap, sh, r.x1, r.y1, scale, scale, blend);
    SpritePart(sprite, frame, cap, 0, sw - 2 * cap, sh, r.x1 + c, r.y1, xs, scale, blend);
    SpritePart(sprite, frame, sw - cap, 0, cap, sh, r.x2 - c, r.y1, scale, scale, blend);
}

void Text(double x, double y, const std::string& mixedCase, double scale, int color, int align) {
    // The game font draws lowercase letters as capitals and uses the capital codes for icons (coins, timers...).
    std::string text = mixedCase;
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    yyc::RValue font{};
    CallScript(kScriptGetFont, &font);
    std::vector<yyc::RValue> fontArg{font};
    CallWith("draw_set_font", fontArg);
    Call("draw_set_halign", {static_cast<double>(align)});
    Call("draw_set_valign", {1});  // fa_middle
    Call("draw_set_colour", {static_cast<double>(color)});
    // fa_middle centres the font's full cell; the glyphs sit in its upper part.
    const double glyphY = y + kFontCellOffset * scale;
    std::vector<yyc::RValue> args{yyc::Real(x), yyc::Real(glyphY), yyc::String(text.c_str()), yyc::Real(scale),
                                  yyc::Real(scale), yyc::Real(0)};
    CallWith("draw_text_transformed", args);
    yyc::Free(&args[2]);
}

void End() {
    yyc::RValue result{};
    CallScript(kScriptDrawReset, &result);
    yyc::Free(&result);
}

std::string TypedText() {
    yyc::RValue value{};
    yyc::GetBuiltinVar(g_self, yyc::va::YYVar_keyboard_string, &value);
    std::string text = yyc::ToString(value);
    yyc::Free(&value);
    return text;
}

void SetTypedText(const std::string& text) {
    yyc::RValue value = yyc::String(text.c_str());
    yyc::SetBuiltinVar(g_self, yyc::va::YYVar_keyboard_string, &value);
    yyc::Free(&value);
}

}  // namespace coop::gui
