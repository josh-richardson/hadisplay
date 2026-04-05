#include "scene_icons.h"

#include "scene_draw.h"
#include "scene_style.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace hadisplay::scene {
namespace {

constexpr double kPi = 3.14159265358979323846;

void draw_plug_icon(RenderBuffer& buffer,
                    int width,
                    int height,
                    const Rect& rect,
                    Color value) {
    const int cx = rect.x + (rect.width / 2);
    const int top = rect.y + 4;
    draw_line(buffer, width, height, cx - 5, top, cx - 5, top + 10, 3, value);
    draw_line(buffer, width, height, cx + 5, top, cx + 5, top + 10, 3, value);
    draw_rect_thick(buffer, width, height, {cx - 11, top + 10, 22, 16}, 2, value);
    draw_line(buffer, width, height, cx, top + 26, cx, rect.y + rect.height - 4, 3, value);
}

}  // namespace

void draw_status_chip(RenderBuffer& buffer,
                      int width,
                      int height,
                      const Rect& rect,
                      Color border,
                      Color fill) {
    fill_rect(buffer, width, height, rect, fill);
    draw_rect_thick(buffer, width, height, rect, 2, border);
}

void draw_sun_icon(RenderBuffer& buffer,
                   int width,
                   int height,
                   const Rect& rect,
                   Color value) {
    const int cx = rect.x + (rect.width / 2);
    const int cy = rect.y + (rect.height / 2);
    const int core = std::max(6, std::min(rect.width, rect.height) / 6);
    fill_rect(buffer, width, height, {cx - core, cy - core, core * 2, core * 2}, value);

    const int inner = core + 5;
    const int outer = core + 15;
    for (int i = 0; i < 8; ++i) {
        const double angle = (kPi / 4.0) * static_cast<double>(i);
        const int x0 = cx + static_cast<int>(std::lround(std::cos(angle) * inner));
        const int y0 = cy + static_cast<int>(std::lround(std::sin(angle) * inner));
        const int x1 = cx + static_cast<int>(std::lround(std::cos(angle) * outer));
        const int y1 = cy + static_cast<int>(std::lround(std::sin(angle) * outer));
        draw_line(buffer, width, height, x0, y0, x1, y1, 3, value);
    }
}

void draw_wrench_icon(RenderBuffer& buffer,
                      int width,
                      int height,
                      const Rect& rect,
                      Color value) {
    const int left = rect.x + 12;
    const int right = rect.x + rect.width - 14;
    const int top = rect.y + 12;
    const int bottom = rect.y + rect.height - 12;
    const int mid_x = rect.x + (rect.width / 2) - 2;
    const int mid_y = rect.y + (rect.height / 2) + 2;

    draw_line(buffer, width, height, left, bottom - 2, mid_x, mid_y, 4, value);
    draw_line(buffer, width, height, mid_x, mid_y, right - 8, top + 8, 4, value);
    draw_line(buffer, width, height, right - 8, top + 8, right, top, 4, value);
    draw_line(buffer, width, height, right - 8, top + 8, right, top + 16, 4, value);
    fill_rect(buffer, width, height, {left - 2, bottom - 8, 10, 10}, value);
}

void draw_power_icon(RenderBuffer& buffer,
                     int width,
                     int height,
                     const Rect& rect,
                     Color value) {
    const int size = std::min(rect.width, rect.height);
    const int cx = rect.x + (rect.width / 2) - 1;
    const int cy = rect.y + (rect.height / 2);
    const int radius = std::max(7, (size / 2) - 10);
    draw_arc(buffer, width, height, cx, cy, radius, kPi * 1.8, kPi * 3.2, 3, value);
    draw_line(buffer, width, height, cx, rect.y + std::max(6, size / 5), cx, cy - radius / 2, 3, value);
}

void draw_cog_icon(RenderBuffer& buffer,
                   int width,
                   int height,
                   const Rect& rect,
                   Color value) {
    const int cx = rect.x + (rect.width / 2);
    const int cy = rect.y + (rect.height / 2);
    const int size = std::min(rect.width, rect.height);
    const int body_r = size / 3;
    const int tooth_r = size / 2 - 2;
    const int hole_r = size / 8;
    const int tooth_w = 5;

    // Draw 6 gear teeth as thick, short stubs.
    for (int i = 0; i < 6; ++i) {
        const double angle = (kPi / 3.0) * static_cast<double>(i);
        const double cos_a = std::cos(angle);
        const double sin_a = std::sin(angle);
        const int x0 = cx + static_cast<int>(std::lround(cos_a * (body_r - 2)));
        const int y0 = cy + static_cast<int>(std::lround(sin_a * (body_r - 2)));
        const int x1 = cx + static_cast<int>(std::lround(cos_a * tooth_r));
        const int y1 = cy + static_cast<int>(std::lround(sin_a * tooth_r));
        draw_line(buffer, width, height, x0, y0, x1, y1, tooth_w, value);
    }

    // Solid circular body using filled arcs at multiple radii.
    for (int r = body_r; r >= 1; --r) {
        draw_arc(buffer, width, height, cx, cy, r, 0.0, kPi * 2.0, 1, value);
    }

    // Center hole.
    for (int r = hole_r; r >= 1; --r) {
        draw_arc(buffer, width, height, cx, cy, r, 0.0, kPi * 2.0, 1,
                 active_theme().white);
    }
    fill_rect(buffer, width, height,
              {cx - hole_r, cy - hole_r, hole_r * 2, hole_r * 2},
              active_theme().white);
}

void draw_wifi_icon(RenderBuffer& buffer,
                    int width,
                    int height,
                    const Rect& rect,
                    bool connected,
                    Color value,
                    Color disconnected,
                    Color /*muted*/) {
    const int cx = rect.x + (rect.width / 2);
    const int cy = rect.y + rect.height - 14;
    const Color stroke = connected ? value : disconnected;
    draw_arc(buffer, width, height, cx, cy, 10, kPi * 1.14, kPi * 1.86, 3, stroke);
    draw_arc(buffer, width, height, cx, cy, 19, kPi * 1.14, kPi * 1.86, 3, stroke);
    draw_arc(buffer, width, height, cx, cy, 28, kPi * 1.14, kPi * 1.86, 3, stroke);
    fill_rect(buffer, width, height, {cx - 3, cy - 3, 6, 6}, stroke);
    if (!connected) {
        draw_line(buffer, width, height, rect.x + 14, rect.y + 14, rect.x + rect.width - 14, rect.y + rect.height - 14, 4, disconnected);
    }
}

void draw_cloud_icon(RenderBuffer& buffer,
                     int width,
                     int height,
                     const Rect& rect,
                     bool rainy,
                     Color value,
                     Color muted) {
    const Color stroke = rainy ? muted : value;
    fill_rect(buffer, width, height, {rect.x + 12, rect.y + 26, rect.width - 24, 12}, stroke);
    fill_rect(buffer, width, height, {rect.x + 8, rect.y + 20, 18, 18}, stroke);
    fill_rect(buffer, width, height, {rect.x + 24, rect.y + 12, 22, 22}, stroke);
    fill_rect(buffer, width, height, {rect.x + 42, rect.y + 18, 18, 18}, stroke);
    if (rainy) {
        draw_line(buffer, width, height, rect.x + 22, rect.y + 42, rect.x + 18, rect.y + 50, 2, value);
        draw_line(buffer, width, height, rect.x + 34, rect.y + 42, rect.x + 30, rect.y + 50, 2, value);
        draw_line(buffer, width, height, rect.x + 46, rect.y + 42, rect.x + 42, rect.y + 50, 2, value);
    }
}

void draw_battery_icon(RenderBuffer& buffer,
                       int width,
                       int height,
                       const Rect& rect,
                       int percent,
                       bool charging,
                       bool available,
                       Color value,
                       Color muted) {
    const Color stroke = available ? value : muted;
    const int plug_width = charging ? 22 : 0;
    const Rect battery{
        rect.x + 8 + plug_width,
        rect.y + 12,
        rect.width - 20 - plug_width,
        rect.height - 24,
    };
    draw_rect_thick(buffer, width, height, battery, 3, stroke);
    fill_rect(buffer,
              width,
              height,
              {battery.x + battery.width, battery.y + (battery.height / 3), 8, std::max(8, battery.height / 3)},
              stroke);

    if (available) {
        draw_text_centered(buffer,
                           width,
                           height,
                           battery,
                           battery.y + ((battery.height - 14) / 2),
                           std::to_string(std::clamp(percent, 0, 100)),
                           2,
                           value);
    } else {
        draw_text_centered(buffer, width, height, battery, battery.y + ((battery.height - 14) / 2), "--", 2, stroke);
    }

    if (charging) {
        draw_plug_icon(buffer, width, height, {rect.x + 4, rect.y + 10, 20, rect.height - 20}, value);
    }
}

}  // namespace hadisplay::scene
