// GUI cursor -> UI canvas mapping (gui/canvas_mapping.h). The canvas is drawn
// stretched over the window, so clicks must be scaled back before hit-testing.
#include "canvas_mapping.h"
#include <cmath>
#include <cstdio>

namespace {
unsigned failures = 0;
void Check(bool condition, const char* what) {
    if (!condition) { std::fprintf(stderr, "canvas mapping: %s\n", what); ++failures; }
}
bool Near(double a, double b) { return std::fabs(a - b) < 1e-9; }
bool Maps(double x, double y, int ww, int wh, int cw, int ch, double ex, double ey) {
    const auto p = CursorToCanvas(x, y, ww, wh, cw, ch);
    return p && Near(p->x, ex) && Near(p->y, ey);
}
// A ui.json-style hitbox in canvas coordinates.
struct Box { double left, top, width, height; };
bool Hit(const Box& b, double x, double y) { return x >= b.left && x < b.left + b.width && y >= b.top && y < b.top + b.height; }
}

int main() {
    constexpr int kCanvasW = 1980, kCanvasH = 712; // gui/ui.png 3300x1188 x ui.json scale 0.6
    Check(Maps(1000, 400, kCanvasW, kCanvasH, kCanvasW, kCanvasH, 1000, 400), "window at canvas size: identity");
    Check(Maps(500, 200, kCanvasW / 2, kCanvasH / 2, kCanvasW, kCanvasH, 1000, 400), "half-size window scales up");
    Check(Maps(2000, 800, kCanvasW * 2, kCanvasH * 2, kCanvasW, kCanvasH, 1000, 400), "double-size window scales down");
    Check(Maps(330, 712, 990, 1424, kCanvasW, kCanvasH, 660, 356), "non-uniform resize scales each axis");
    Check(Maps(0, 0, 1187, 427, kCanvasW, kCanvasH, 0, 0), "origin stays at origin");
    Check(Maps(1187, 427, 1187, 427, kCanvasW, kCanvasH, kCanvasW, kCanvasH), "far corner maps to the canvas corner");
    Check(!CursorToCanvas(10, 10, 0, 427, kCanvasW, kCanvasH) && !CursorToCanvas(10, 10, 1187, 0, kCanvasW, kCanvasH),
          "a window without area yields no position (minimised)");
    // High-DPI: GLFW reports cursor and window size in screen coordinates while
    // the framebuffer is larger; only screen coordinates are used, so a 2x
    // framebuffer changes nothing.
    Check(Maps(500, 200, kCanvasW / 2, kCanvasH / 2, kCanvasW, kCanvasH, 1000, 400),
          "framebuffer scale does not enter the mapping");
    // The bug: clicking the centre of a button in a resized window must hit it.
    const Box play{143, 463, 103, 103}; // ui.json "play" [239, 772, 173, 173] x 0.6, as LoadUIConfig rounds
    const double cx = (play.left + play.width / 2) * 0.6, cy = (play.top + play.height / 2) * 0.6;
    const auto p = CursorToCanvas(cx, cy, static_cast<int>(kCanvasW * 0.6), static_cast<int>(kCanvasH * 0.6), kCanvasW, kCanvasH);
    Check(p && Hit(play, p->x, p->y), "centre of a button in a 0.6x window hits it");
    Check(!Hit(play, cx, cy), "unscaled cursor coordinates miss it (the previous behaviour)");
    if (failures) return 1;
    std::puts("canvas mapping tests passed");
    return 0;
}
