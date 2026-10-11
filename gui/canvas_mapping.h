#pragma once

#include <optional>

// The GUI draws its fixed UI canvas (background image size x ui.json scale)
// stretched over the whole window, so hitboxes are in canvas coordinates.
// Cursor positions (glfwGetCursorPos) and window sizes (glfwGetWindowSize) are
// both in screen coordinates. The framebuffer, larger on high-DPI displays,
// plays no part in mapping a cursor to the canvas.
struct CanvasPoint {
    double x;
    double y;
};

// Canvas coordinates of a cursor position in a window of the given size.
// Nothing while the window has no area (for example when minimised).
inline std::optional<CanvasPoint> CursorToCanvas(double x, double y, int windowWidth, int windowHeight,
                                                 int canvasWidth, int canvasHeight) {
    if (windowWidth <= 0 || windowHeight <= 0) return std::nullopt;
    return CanvasPoint{x * static_cast<double>(canvasWidth) / windowWidth,
                       y * static_cast<double>(canvasHeight) / windowHeight};
}
