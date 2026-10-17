// Immediate-mode drawing and input on top of GML builtins, in GUI coordinates. Game thread only:
// drawing calls belong in a Draw GUI event, input queries in a Step event.
#pragma once

#include <initializer_list>
#include <string>

#include "yyc.h"

namespace coop::gui {

constexpr int kAlignLeft = 0;
constexpr int kAlignCenter = 1;
constexpr int kAlignRight = 2;

constexpr int Rgb(int r, int g, int b) { return r | (g << 8) | (b << 16); }  // GML colours are BGR

struct Rect {
    double x1, y1, x2, y2;
    bool Contains(double x, double y) const { return x >= x1 && x <= x2 && y >= y1 && y <= y2; }
};

// Every call needs an instance to run in; set it at the start of each event.
void Begin(yyc::CInstance* self);

double Call(const char* builtin, std::initializer_list<double> args);
double GuiWidth();
double GuiHeight();
double MouseX();
double MouseY();
bool MousePressed();              // left button went down this step
bool KeyPressed(int virtualKey);  // e.g. VK_ESCAPE, VK_RETURN, VK_BACK

void Fill(const Rect& r, int color, double alpha = 1.0);
void Frame(const Rect& r, int color, double alpha = 1.0);
// Text in the game's font (always shown in capitals); (x, y) is the anchor for `align`, vertically centred.
void Text(double x, double y, const std::string& text, double scale, int color, int align = kAlignLeft);
// Restores the draw state the game expects (font, colour, alignment).
void End();

// keyboard_string: the text typed since it was last reset.
std::string TypedText();
void SetTypedText(const std::string& text);

}  // namespace coop::gui
