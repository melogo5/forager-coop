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
