#include "scene_layout.h"

#include <algorithm>

namespace hadisplay::scene {
namespace {

Rect system_bar_rect(int width, int height) {
    return {0, 0, width, std::clamp(height / 14, 74, 98)};
}

int top_icon_size(int height) {
    return std::clamp(system_bar_rect(0, height).height - 26, 52, 76);
}

Rect main_frame_rect(int width, int height) {
    const Rect system_bar = system_bar_rect(width, height);
    const int outer_margin = std::clamp(width / 24, 24, 48);
    const int top_gap = std::clamp(height / 60, 14, 24);
    return {
        outer_margin,
        system_bar.height + top_gap,
        width - (outer_margin * 2),
        height - system_bar.height - top_gap - outer_margin,
    };
}

Rect brightness_button_rect(int width, int height) {
    const Rect bar = system_bar_rect(width, height);
    const int button_height = std::clamp(bar.height - 26, 52, 76);
    const int button_width = std::clamp(width / 8, 108, 150);
    return {
        width - button_width - 18,
        bar.y + ((bar.height - button_height) / 2),
        button_width,
        button_height,
    };
}

Rect wifi_indicator_rect(int width, int height) {
    const Rect bar = system_bar_rect(width, height);
    const Rect brightness = brightness_button_rect(width, height);
    const int size = top_icon_size(height);
    return {
        brightness.x - size - 14,
        bar.y + ((bar.height - size) / 2) - 5,
        size,
        size,
    };
}

Rect battery_indicator_rect(int width, int height) {
    const Rect bar = system_bar_rect(width, height);
    const Rect wifi = wifi_indicator_rect(width, height);
    const int button_height = std::clamp(bar.height - 26, 52, 76);
    const int button_width = std::clamp(width / 8, 118, 156);
    return {
        wifi.x - button_width - 14,
        bar.y + ((bar.height - button_height) / 2),
        button_width,
        button_height,
    };
}

Rect dev_indicator_rect(int width, int height) {
    const Rect bar = system_bar_rect(width, height);
    const Rect battery = battery_indicator_rect(width, height);
    const int size = top_icon_size(height);
    return {
        battery.x - size - 12,
        bar.y + ((bar.height - size) / 2) - 5,
        size,
        size,
    };
}

}  // namespace

SceneLayout make_scene_layout(int width, int height) {
    SceneLayout layout{};
    layout.system_bar = system_bar_rect(width, height);
    layout.outer = main_frame_rect(width, height);
    const int inset = std::clamp(width / 36, 18, 30);
    const int header_height = std::clamp(layout.outer.height / 6, 108, 148);
    const int footer_height = std::clamp(layout.outer.height / 9, 86, 120);
    layout.header = {
        layout.outer.x + inset,
        layout.outer.y + inset,
        layout.outer.width - (inset * 2),
        header_height,
    };
    layout.footer = {
        layout.outer.x + inset,
        layout.outer.y + layout.outer.height - inset - footer_height,
        layout.outer.width - (inset * 2),
        footer_height,
    };
    layout.body = {
        layout.outer.x + inset,
        layout.header.y + layout.header.height + 18,
        layout.outer.width - (inset * 2),
        layout.footer.y - (layout.header.y + layout.header.height + 36),
    };
    layout.brightness_button = brightness_button_rect(width, height);
    layout.wifi_button = wifi_indicator_rect(width, height);
    layout.battery_button = battery_indicator_rect(width, height);
    layout.dev_button = dev_indicator_rect(width, height);
    layout.clock_rect = {
        24,
        layout.system_bar.y + 12,
        190,
        layout.system_bar.height - 24,
    };
    layout.weather_rect = {
        layout.clock_rect.x + layout.clock_rect.width - 36,
        layout.system_bar.y + 30,
        std::max(120, layout.dev_button.x - (layout.clock_rect.x + layout.clock_rect.width - 20)),
        layout.system_bar.height - 20,
    };
    return layout;
}

Rect grid_cell(const Rect& bounds, int columns, int rows, int column, int row, int gutter) {
    const int cell_width = (bounds.width - ((columns - 1) * gutter)) / columns;
    const int cell_height = (bounds.height - ((rows - 1) * gutter)) / rows;
    return {
        bounds.x + (column * (cell_width + gutter)),
        bounds.y + (row * (cell_height + gutter)),
        cell_width,
        cell_height,
    };
}

}  // namespace hadisplay::scene
