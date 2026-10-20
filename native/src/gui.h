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

constexpr int kWhite = Rgb(255, 255, 255);

void Fill(const Rect& r, int color, double alpha = 1.0);
// Part of a sprite's frame (sprite pixels from `left`, `top`, `w` x `h`) drawn at (x, y), scaled; `blend` tints it.
void SpritePart(int sprite, int frame, double left, double top, double w, double h, double x, double y,
                double xscale, double yscale, int blend = kWhite);
// A sprite's frame stretched over `r` with its corners (`corner` sprite pixels) kept whole at `scale`: the game's
// boxes (sprTextBox) at any size.
void Box9(int sprite, int frame, const Rect& r, double corner, double scale, int blend = kWhite);
// A sprite's frame stretched sideways over `r`, scaled to r's height, with its ends (`cap` sprite pixels) kept
// whole: a button body at any width.
void Bar3(int sprite, int frame, const Rect& r, double cap, int blend = kWhite);
void Frame(const Rect& r, int color, double alpha = 1.0);
// Text in the game's font (always shown in capitals); (x, y) is the anchor for `align`, vertically centred.
void Text(double x, double y, const std::string& text, double scale, int color, int align = kAlignLeft);
// Restores the draw state the game expects (font, colour, alignment).
void End();

// keyboard_string: the text typed since it was last reset.
std::string TypedText();
void SetTypedText(const std::string& text);

}  // namespace coop::gui
