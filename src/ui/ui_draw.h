// UI drawing layer: batched SDF quads (rounded boxes, soft shadows, gradients, hairlines, SDF
// text) in one dynamic vertex buffer, drawn on the backbuffer with premultiplied alpha.
// All coordinates are reference pixels (canvas 1080 units tall); conversion to physical pixels
// happens here. Internal to src/ui.
#pragma once
#include "../math/math.h"
#include "ui_font.h"
#include <string>

namespace ui {
namespace gfx {

using m::vec2;
using m::vec4;

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    Rect() = default;
    constexpr Rect(float x_, float y_, float w_, float h_) : x(x_), y(y_), w(w_), h(h_) {}
    float r() const { return x + w; }
    float b() const { return y + h; }
    float cx() const { return x + w * 0.5f; }
    float cy() const { return y + h * 0.5f; }
    vec2 center() const { return {cx(), cy()}; }
    bool contains(vec2 p) const { return p.x >= x && p.y >= y && p.x < x + w && p.y < y + h; }
    Rect inset(float d) const { return {x + d, y + d, w - 2 * d, h - 2 * d}; }
    Rect inset(float dx, float dy) const { return {x + dx, y + dy, w - 2 * dx, h - 2 * dy}; }
    Rect offset(float dx, float dy) const { return {x + dx, y + dy, w, h}; }
};
Rect intersect(const Rect& a, const Rect& b);

enum Layer { LAYER_BACK, LAYER_MAIN, LAYER_OVERLAY, LAYER_MODAL, LAYER_TOP, LAYER_COUNT };

bool init();
void shutdown();
void beginFrame(int fbWidth, int fbHeight);
void endFrame();  // uploads and draws every layer, restores GL state

float scale();        // physical pixels per reference pixel
float px();           // one physical pixel in reference units
vec2 viewSize();      // canvas size in reference units
float snap(float v);  // snaps a reference coordinate to the physical pixel grid

void setLayer(Layer l);
Layer layer();
void pushClip(const Rect& r);
void popClip();
bool clipContains(vec2 p);
void pushAlpha(float a);
void popAlpha();

// ---- Shapes ---------------------------------------------------------------------------------------
// Colours are straight-alpha display (sRGB) values.
void fill(const Rect& r, vec4 c, float radius = 0.0f);
void fillV(const Rect& r, vec4 top, vec4 bottom, float radius = 0.0f);  // vertical gradient
void fillH(const Rect& r, vec4 left, vec4 right, float radius = 0.0f);  // horizontal gradient
// Outline of 'thickness' reference units (at least one physical pixel), drawn inside r.
void stroke(const Rect& r, vec4 c, float thickness = 0.0f, float radius = 0.0f);
void shadow(const Rect& r, float radius, float blur, vec4 c);
// Horizontal hairline (one physical pixel unless thickness is given, snapped to the pixel grid).
void hline(float x0, float x1, float y, vec4 c, float thickness = 0.0f);
void vline(float x, float y0, float y1, vec4 c, float thickness = 0.0f);
// Hairline that fades out towards both ends (gold rules).
void hlineFade(float x0, float x1, float y, vec4 c, float fadeFrac = 0.35f, float thickness = 0.0f);
void line(vec2 a, vec2 b, vec4 c, float thickness);
void diamond(vec2 center, float radius, vec4 c, float strokeWidth = 0.0f);
void circle(vec2 center, float radius, vec4 c, float strokeWidth = 0.0f);
// Elliptic radial falloff: opaque inside t0, transparent beyond t1 (t in units of the radii).
void radial(vec2 center, vec2 radii, vec4 c, float t0 = 0.0f, float t1 = 1.0f);

// ---- Text ---------------------------------------------------------------------------------------
// Every text is shaped (text_shape.h): per-script font fallback, Arabic joining, bidirectional
// reordering. Alignment is the caller's choice (right-to-left layouts pass HAlign::Right).
enum class HAlign { Left, Center, Right };
struct TextStyle {
    int face = font::FACE_TEXT;
    float size = 26.0f;        // reference pixels per em
    vec4 color{1, 1, 1, 1};
    HAlign align = HAlign::Left;
    float tracking = 0.0f;     // extra letter spacing, em
    float weight = 0.0f;       // SDF dilation, in reference pixels (+ = bolder)
    float softness = 0.0f;     // extra edge blur in reference pixels (glows / shadows)
    int hand = -1;             // >= 0: a player's handwriting (font::HandStyle), 'face' is ignored
    int dir = -1;              // base direction: -1 auto (right-to-left when the UI language is and
                               // the text contains right-to-left script, else first strong
                               // character), 0 LTR, 1 RTL
};
// The interface's text size (Options > Display): every text but the page-sized titles and the
// handwriting is drawn and measured k times larger (0.8 - 1.6).
void setTextScale(float k);
float textScale();
float textWidth(const std::string& s, const TextStyle& st);
// Font size that fits s in maxWidth (never below minScale * st.size).
float fitSize(const std::string& s, const TextStyle& st, float maxWidth, float minScale = 0.7f);
// Base direction text() uses for s (0 LTR, 1 RTL).
int textDirection(const std::string& s, const TextStyle& st);
// Carets of text fields, on the line text() draws for s with Left alignment: offset (reference
// pixels from x) of the caret before the logical character 'index', and the index nearest to an
// offset. (ui.h's ui::text() hides the text:: shaping namespace from UI code, hence these.)
float caretOffset(const std::string& s, const TextStyle& st, int index);
int caretAt(const std::string& s, const TextStyle& st, float offset);
// Draws a single line; y is the baseline. Returns the advance width.
float text(const std::string& s, float x, float baseline, const TextStyle& st);
// Word-wrapped paragraph starting with its first baseline at 'baseline'. Returns the number of
// lines. lineHeight in reference pixels (0 = 1.3 * size). Explicit '\n' breaks lines. Lines break
// at spaces, and between CJK characters (with the usual line start / end prohibitions).
int textWrapped(const std::string& s, float x, float baseline, float maxWidth, const TextStyle& st, float lineHeight = 0.0f);
int wrapLineCount(const std::string& s, float maxWidth, const TextStyle& st);
// Width of the widest wrapped line.
float wrapWidth(const std::string& s, float maxWidth, const TextStyle& st);
float capHeight(const TextStyle& st);

}  // namespace gfx
}  // namespace ui
