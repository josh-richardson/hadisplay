#include "scene_layout.h"

#include <algorithm>

namespace hadisplay::scene {
namespace {

Rect system_bar_rect(int width, int height, bool compact_ui) {
    if (compact_ui) {
        return {0, 0, width, std::clamp(height / 16, 62, 78)};
    }
    return {0, 0, width, std::clamp(height / 14, 74, 98)};
}

int top_icon_size(int height, bool compact_ui) {
    if (compact_ui) {
        return std::clamp(system_bar_rect(0, height, true).height - 22, 38, 54);
    }
    return std::clamp(system_bar_rect(0, height, false).height - 26, 52, 76);
}

Rect main_frame_rect(int width, int height, bool compact_ui) {
    const Rect system_bar = system_bar_rect(width, height, compact_ui);
    const int outer_margin = compact_ui ? std::clamp(width / 28, 18, 32) : std::clamp(width / 24, 24, 48);
    const int top_gap = compact_ui ? std::clamp(height / 80, 10, 18) : std::clamp(height / 60, 14, 24);
    return {
        outer_margin,
        system_bar.height + top_gap,
        width - (outer_margin * 2),
        height - system_bar.height - top_gap - outer_margin,
    };
}

Rect power_button_rect(int width, int height, bool compact_ui) {
    const Rect bar = system_bar_rect(width, height, compact_ui);
    const int button_height = compact_ui ? std::clamp(bar.height - 18, 42, 56) : std::clamp(bar.height - 26, 52, 76);
    return {
        width - button_height - (compact_ui ? 17 : 18),
        bar.y + ((bar.height - button_height) / 2),
        button_height,
        button_height,
    };
}

Rect brightness_button_rect(int width, int height, bool compact_ui) {
    const Rect bar = system_bar_rect(width, height, compact_ui);
    const Rect power = power_button_rect(width, height, compact_ui);
    const int button_height = compact_ui ? std::clamp(bar.height - 18, 42, 56) : std::clamp(bar.height - 26, 52, 76);
    const int button_width = compact_ui ? std::clamp(width / 9, 88, 118) : std::clamp(width / 8, 108, 150);
    return {
        power.x - button_width - (compact_ui ? 10 : 14),
        bar.y + ((bar.height - button_height) / 2),
        button_width,
        button_height,
    };
}

Rect wifi_indicator_rect(int width, int height, bool compact_ui) {
    const Rect bar = system_bar_rect(width, height, compact_ui);
    const Rect brightness = brightness_button_rect(width, height, compact_ui);
    const int size = top_icon_size(height, compact_ui);
    return {
        brightness.x - size - (compact_ui ? 10 : 14),
        bar.y + ((bar.height - size) / 2) - (compact_ui ? 1 : 5),
        size,
        size,
    };
}

Rect battery_indicator_rect(int width, int height, bool compact_ui) {
    const Rect bar = system_bar_rect(width, height, compact_ui);
    const Rect wifi = wifi_indicator_rect(width, height, compact_ui);
    const int button_height = compact_ui ? std::clamp(bar.height - 18, 42, 56) : std::clamp(bar.height - 26, 52, 76);
    const int button_width = compact_ui ? std::clamp(width / 8, 96, 126) : std::clamp(width / 8, 118, 156);
    return {
        wifi.x - button_width - (compact_ui ? 10 : 14),
        bar.y + ((bar.height - button_height) / 2),
        button_width,
        button_height,
    };
}

Rect dev_indicator_rect(int width, int height, bool compact_ui) {
    const Rect bar = system_bar_rect(width, height, compact_ui);
    const Rect battery = battery_indicator_rect(width, height, compact_ui);
    const int size = top_icon_size(height, compact_ui);
    return {
        battery.x - size - (compact_ui ? 8 : 12),
        bar.y + ((bar.height - size) / 2) - (compact_ui ? 1 : 5),
        size,
        size,
    };
}

}  // namespace

SceneLayout make_scene_layout(int width, int height, bool compact_ui) {
    SceneLayout layout{};
    layout.system_bar = system_bar_rect(width, height, compact_ui);
    layout.outer = main_frame_rect(width, height, compact_ui);
    const int inset = compact_ui ? std::clamp(width / 42, 14, 22) : std::clamp(width / 36, 18, 30);
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
        layout.header.y + layout.header.height + (compact_ui ? 14 : 18),
        layout.outer.width - (inset * 2),
        layout.footer.y - (layout.header.y + layout.header.height + (compact_ui ? 28 : 36)),
    };
    layout.power_button = power_button_rect(width, height, compact_ui);
    layout.brightness_button = brightness_button_rect(width, height, compact_ui);
    layout.wifi_button = wifi_indicator_rect(width, height, compact_ui);
    layout.battery_button = battery_indicator_rect(width, height, compact_ui);
    layout.dev_button = dev_indicator_rect(width, height, compact_ui);
    layout.clock_rect = {
        compact_ui ? 16 : 24,
        layout.system_bar.y + (compact_ui ? 8 : 12),
        compact_ui ? 150 : 190,
        layout.system_bar.height - (compact_ui ? 16 : 24),
    };
    layout.weather_rect = {
        layout.clock_rect.x + layout.clock_rect.width - (compact_ui ? 18 : 36),
        layout.system_bar.y + (compact_ui ? 18 : 30),
        std::max(compact_ui ? 104 : 120, layout.dev_button.x - (layout.clock_rect.x + layout.clock_rect.width - (compact_ui ? 8 : 20))),
        layout.system_bar.height - (compact_ui ? 12 : 20),
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
