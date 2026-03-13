#include "scene.h"

#include "scene_draw.h"
#include "scene_icons.h"
#include "scene_layout.h"
#include "scene_style.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace hadisplay {
namespace {

constexpr int kSetupPageSize = 5;
constexpr int kDashboardPageSize = 6;

#define kWhite scene::active_theme().white
#define kLight scene::active_theme().light
#define kMid scene::active_theme().mid
#define kDark scene::active_theme().dark

bool weather_is_rainy(const std::string& condition) {
    return condition.find("rain") != std::string::npos ||
           condition.find("pour") != std::string::npos ||
           condition.find("shower") != std::string::npos ||
           condition.find("lightning") != std::string::npos;
}

bool weather_is_sunny(const std::string& condition) {
    return condition.find("sun") != std::string::npos ||
           condition.find("clear") != std::string::npos;
}

int selected_entity_count(const SceneState& state) {
    return static_cast<int>(std::count_if(state.entities.begin(), state.entities.end(), [](const EntityItem& entity) {
        return entity.selected;
    }));
}

std::vector<int> selected_entity_indices(const SceneState& state) {
    std::vector<int> indices;
    for (std::size_t i = 0; i < state.entities.size(); ++i) {
        if (state.entities[i].selected) {
            indices.push_back(static_cast<int>(i));
        }
    }
    return indices;
}

int max_page_index(int item_count, int page_size) {
    if (item_count <= 0) {
        return 0;
    }
    return std::max(0, (item_count - 1) / page_size);
}

Rect footer_button_rect(const scene::SceneLayout& layout, int count, int index) {
    const int gutter = 18;
    const int width = (layout.footer.width - ((count - 1) * gutter)) / count;
    return {
        layout.footer.x + (index * (width + gutter)),
        layout.footer.y,
        width,
        layout.footer.height,
    };
}

Rect detail_action_button_rect(const scene::SceneLayout& layout, int index, int count) {
    const int columns = 3;
    const int rows = std::max(1, (count + columns - 1) / columns);
    const int gutter = 18;
    const int start_y = layout.body.y + 352;
    const Rect bounds{
        layout.body.x,
        start_y,
        layout.body.width,
        std::max(120, (layout.body.y + layout.body.height) - start_y),
    };
    return scene::grid_cell(bounds, columns, rows, index % columns, index / columns, gutter);
}

Rect climate_info_row_rect(const scene::SceneLayout& layout, int index) {
    const int start_y = layout.body.y + 252;
    const int gutter = 18;
    const int row_height = 82;
    return {
        layout.body.x,
        start_y + (index * (row_height + gutter)),
        layout.body.width,
        row_height,
    };
}

const Button* find_button(const std::vector<Button>& buttons, ButtonId id, int value, int* index_out = nullptr) {
    for (std::size_t i = 0; i < buttons.size(); ++i) {
        if (buttons[i].id == id && buttons[i].value == value) {
            if (index_out != nullptr) {
                *index_out = static_cast<int>(i);
            }
            return &buttons[i];
        }
    }
    return nullptr;
}

void draw_button_frame(RenderBuffer& buffer,
                       int width,
                       int height,
                       const Rect& rect,
                       bool pressed,
                       bool selected) {
    Color fill = kWhite;
    Color border = kDark;
    if (pressed) {
        fill = kDark;
        border = kDark;
    } else if (selected) {
        fill = scene::active_theme().highlight;
    }

    scene::fill_rect(buffer, width, height, rect, fill);
    scene::draw_rect_thick(buffer, width, height, rect, 2, border);
}

void draw_text_button(RenderBuffer& buffer,
                      int width,
                      int height,
                      const Rect& rect,
                      const std::string& label,
                      bool pressed,
                      bool selected,
                      int scale = 3) {
    Color fill = kWhite;
    Color text_color = kDark;
    const std::string upper = scene::uppercase_ascii(label);
    if (buffer.format == PixelFormat::RGBA32) {
        if (upper == "RED") {
            fill = scene::active_theme().accent_red;
            text_color = kWhite;
        } else if (upper == "GREEN") {
            fill = scene::active_theme().accent_green;
            text_color = kWhite;
        } else if (upper == "BLUE") {
            fill = scene::active_theme().accent_blue;
            text_color = kWhite;
        } else if (upper == "DAYLIGHT") {
            fill = scene::active_theme().accent_yellow;
        } else if (upper == "WARM") {
            fill = scene::active_theme().accent_orange;
            text_color = kWhite;
        } else if (upper == "NEUTRAL") {
            fill = scene::active_theme().warning;
        }
    }
    if (pressed) {
        fill = kDark;
        text_color = kWhite;
    } else if (selected) {
        fill = buffer.format == PixelFormat::RGBA32 ? scene::active_theme().highlight : kLight;
    }
    scene::fill_rect(buffer, width, height, rect, fill);
    scene::draw_rect_thick(buffer, width, height, rect, 2, kDark);
    scene::draw_text_centered(buffer,
                              width,
                              height,
                              rect,
                              rect.y + ((rect.height - (7 * scale)) / 2),
                              upper,
                              scale,
                              text_color);
}

void draw_checkbox(RenderBuffer& buffer,
                   int width,
                   int height,
                   const Rect& rect,
                   bool checked) {
    scene::draw_rect_thick(buffer, width, height, rect, 2, kDark);
    if (!checked) {
        return;
    }
    scene::draw_line(buffer, width, height, rect.x + 6, rect.y + (rect.height / 2), rect.x + 12, rect.y + rect.height - 8, 3, kDark);
    scene::draw_line(buffer, width, height, rect.x + 12, rect.y + rect.height - 8, rect.x + rect.width - 6, rect.y + 6, 3, kDark);
}

void draw_header(RenderBuffer& buffer,
                 int width,
                 int height,
                 const scene::SceneLayout& layout,
                 const std::string& title,
                 const std::string& subtitle,
                 const std::string& status) {
    scene::draw_text(buffer,
                     width,
                     height,
                     layout.header.x,
                     layout.header.y + 10,
                     scene::fit_text_to_width(scene::uppercase_ascii(title), 5, layout.header.width),
                     5,
                     kDark);
    scene::draw_text(buffer,
                     width,
                     height,
                     layout.header.x,
                     layout.header.y + 56,
                     scene::fit_text_to_width(scene::uppercase_ascii(subtitle), 2, layout.header.width),
                     2,
                     kMid);
    scene::draw_text(buffer,
                     width,
                     height,
                     layout.header.x,
                     layout.header.y + layout.header.height - 28,
                     scene::fit_text_to_width(scene::uppercase_ascii(status), 2, layout.header.width),
                     2,
                     kDark);
    scene::fill_rect(buffer, width, height, {layout.header.x, layout.header.y + layout.header.height + 8, layout.header.width, 2}, kMid);
}

void draw_top_bar(RenderBuffer& buffer,
                  const SceneState& state,
                  const scene::SceneLayout& layout,
                  bool brightness_pressed,
                  bool dev_pressed) {
    scene::fill_rect(buffer, state.width, state.height, layout.system_bar, kDark);
    scene::draw_line(buffer,
                     state.width,
                     state.height,
                     0,
                     layout.system_bar.height - 2,
                     state.width - 1,
                     layout.system_bar.height - 2,
                     2,
                     kMid);

    scene::draw_text(buffer,
                     state.width,
                     state.height,
                     layout.clock_rect.x,
                     layout.system_bar.y + 14,
                     scene::fit_text_to_width(scene::uppercase_ascii(state.time_label), 4, layout.clock_rect.width),
                     4,
                     kWhite);
    scene::draw_text(buffer,
                     state.width,
                     state.height,
                     layout.clock_rect.x,
                     layout.system_bar.y + 56,
                     scene::fit_text_to_width(scene::uppercase_ascii(state.date_label), 2, layout.clock_rect.width),
                     2,
                     kLight);

    if (state.weather_available) {
        if (weather_is_sunny(state.weather_condition)) {
            scene::draw_sun_icon(buffer,
                                 state.width,
                                 state.height,
                                 {layout.weather_rect.x + 4, layout.weather_rect.y - 28, 72, layout.weather_rect.height + 18},
                                 buffer.format == PixelFormat::RGBA32 ? scene::active_theme().accent_yellow : kWhite);
        } else {
            scene::draw_cloud_icon(buffer,
                                   state.width,
                                   state.height,
                                   {layout.weather_rect.x - 2, layout.weather_rect.y - 4, 104, layout.weather_rect.height + 26},
                                   weather_is_rainy(state.weather_condition),
                                   buffer.format == PixelFormat::RGBA32 ? scene::active_theme().accent_blue : kWhite,
                                   kLight);
        }
    }
    scene::draw_text(buffer,
                     state.width,
                     state.height,
                     layout.weather_rect.x + 108,
                     layout.weather_rect.y + 18,
                     scene::fit_text_to_width(scene::uppercase_ascii(state.weather_range_label), 2, layout.weather_rect.width - 112),
                     2,
                     kWhite);

    scene::draw_status_chip(buffer,
                            state.width,
                            state.height,
                            layout.brightness_button,
                            kWhite,
                            brightness_pressed ? kMid : kWhite);
    scene::draw_sun_icon(buffer,
                         state.width,
                         state.height,
                         {layout.brightness_button.x + 10, layout.brightness_button.y + 6, 40, layout.brightness_button.height - 12},
                         buffer.format == PixelFormat::RGBA32 ? scene::active_theme().accent_yellow : kDark);
    scene::draw_text_centered(buffer,
                              state.width,
                              state.height,
                              {layout.brightness_button.x + 46,
                               layout.brightness_button.y,
                               layout.brightness_button.width - 46,
                               layout.brightness_button.height},
                              layout.brightness_button.y + ((layout.brightness_button.height - 21) / 2),
                              state.brightness_available ? state.brightness_label : "--",
                              3,
                              kDark);

    if (state.dev_mode || dev_pressed) {
        scene::fill_rect(buffer, state.width, state.height, layout.dev_button, dev_pressed ? kLight : kWhite);
    }
    scene::draw_wrench_icon(buffer,
                            state.width,
                            state.height,
                            {layout.dev_button.x + 4, layout.dev_button.y + 4, layout.dev_button.width - 8, layout.dev_button.height - 8},
                            state.dev_mode ? kWhite : kMid);
    if (state.dev_mode) {
        scene::draw_rect_thick(buffer, state.width, state.height, layout.dev_button, 2, kWhite);
    }

    scene::draw_wifi_icon(buffer,
                          state.width,
                          state.height,
                          layout.wifi_button,
                          state.wifi_connected,
                          buffer.format == PixelFormat::RGBA32 ? scene::active_theme().accent_blue : kWhite,
                          kMid);

    scene::draw_battery_icon(buffer,
                             state.width,
                             state.height,
                             layout.battery_button,
                             state.battery_percent,
                             state.battery_charging,
                             state.battery_available,
                             buffer.format == PixelFormat::RGBA32 && state.battery_charging ? scene::active_theme().accent_green : kWhite,
                             kMid);
}

std::string setup_summary(const EntityItem& entity) {
    std::string summary = entity.kind_label;
    if (entity.kind == EntityKind::Light) {
        if (entity.supports_brightness) {
            summary += " DIM";
        }
        if (entity.supports_color_temp) {
            summary += " TEMP";
        }
        if (entity.supports_rgb) {
            summary += " RGB";
        }
        if (!entity.supports_brightness && !entity.supports_color_temp && !entity.supports_rgb) {
            summary += " ON/OFF";
        }
    } else if (entity.kind == EntityKind::Switch) {
        summary += " SOCKET";
    } else {
        if (entity.target_temperature > 0) {
            summary += " SET " + std::to_string(entity.target_temperature) + "C";
        }
        if (!entity.hvac_action.empty()) {
            summary += " " + scene::uppercase_ascii(entity.hvac_action);
        }
    }
    return summary;
}

std::string dashboard_detail(const EntityItem& entity) {
    if (entity.kind == EntityKind::Light) {
        return entity.supports_brightness ? ("BRI " + std::to_string(entity.brightness_percent) + "%") : "TAP TO TOGGLE";
    }
    if (entity.kind == EntityKind::Switch) {
        return "TAP TO TOGGLE";
    }

    std::string detail;
    if (entity.current_temperature > 0) {
        detail += "NOW " + std::to_string(entity.current_temperature) + "C ";
    }
    if (entity.target_temperature > 0) {
        detail += "SET " + std::to_string(entity.target_temperature) + "C";
    }
    if (detail.empty()) {
        detail = entity.hvac_action.empty() ? "THERMOSTAT" : scene::uppercase_ascii(entity.hvac_action);
    }
    return detail;
}

bool entity_supports_dashboard_detail(const EntityItem& entity) {
    return entity.supports_detail;
}

void draw_setup_view(RenderBuffer& buffer,
                     const SceneState& state,
                     const scene::SceneLayout& layout,
                     const std::vector<Button>& buttons) {
    const int page = std::clamp(state.setup_page, 0, max_page_index(static_cast<int>(state.entities.size()), kSetupPageSize));
    const int start = page * kSetupPageSize;
    const int count = std::min(kSetupPageSize, static_cast<int>(state.entities.size()) - start);
    const int gutter = 18;
    const int row_height = count > 0 ? (layout.body.height - ((count - 1) * gutter)) / std::max(1, count) : layout.body.height;

    const std::string subtitle = std::to_string(selected_entity_count(state)) + " SELECTED";
    draw_header(buffer, state.width, state.height, layout, "SELECT DEVICES", subtitle, state.status);

    if (state.entities.empty()) {
        scene::draw_text_centered(buffer,
                                  state.width,
                                  state.height,
                                  layout.body,
                                  layout.body.y + (layout.body.height / 2) - 24,
                                  "NO DEVICES FOUND",
                                  3,
                                  kDark);
        return;
    }

    for (int i = 0; i < count; ++i) {
        const int entity_index = start + i;
        const EntityItem& entity = state.entities[static_cast<std::size_t>(entity_index)];
        const Rect row{
            layout.body.x,
            layout.body.y + (i * (row_height + gutter)),
            layout.body.width,
            row_height,
        };
        int button_index = -1;
        find_button(buttons, ButtonId::SetupToggleLight, entity_index, &button_index);
        const bool pressed = button_index == state.pressed_button;
        const bool selected = button_index == state.selected_button;
        draw_button_frame(buffer, state.width, state.height, row, pressed, selected);

        const Rect checkbox{row.x + 18, row.y + (row.height / 2) - 16, 32, 32};
        draw_checkbox(buffer, state.width, state.height, checkbox, entity.selected);

        scene::draw_text(buffer,
                         state.width,
                         state.height,
                         row.x + 72,
                         row.y + 18,
                         scene::fit_text_to_width(scene::uppercase_ascii(entity.name), 3, row.width - 188),
                         3,
                         pressed ? kWhite : kDark);

        scene::draw_text(buffer,
                         state.width,
                         state.height,
                         row.x + row.width - 150,
                         row.y + 18,
                         scene::fit_text_to_width(scene::uppercase_ascii(entity.kind_label), 2, 132),
                         2,
                         pressed ? kLight : kMid);

        scene::draw_text(buffer,
                         state.width,
                         state.height,
                         row.x + 72,
                         row.y + row.height - 54,
                         scene::fit_text_to_width(scene::uppercase_ascii(setup_summary(entity)), 2, row.width - 240),
                         2,
                         pressed ? kLight : kMid);

        scene::draw_text(buffer,
                         state.width,
                         state.height,
                         row.x + row.width - 164,
                         row.y + row.height - 54,
                         scene::fit_text_to_width(scene::uppercase_ascii(entity.state_label), 2, 148),
                         2,
                         pressed ? kWhite : kDark);
    }
}

void draw_dashboard_view(RenderBuffer& buffer,
                         const SceneState& state,
                         const scene::SceneLayout& layout,
                         const std::vector<Button>& buttons) {
    const std::vector<int> selected = selected_entity_indices(state);
    const int page = std::clamp(state.dashboard_page, 0, max_page_index(static_cast<int>(selected.size()), kDashboardPageSize));
    draw_header(buffer,
                state.width,
                state.height,
                layout,
                "HOME DASHBOARD",
                std::to_string(static_cast<int>(selected.size())) + " DEVICES",
                state.status);

    if (selected.empty()) {
        scene::draw_text_centered(buffer,
                                  state.width,
                                  state.height,
                                  layout.body,
                                  layout.body.y + (layout.body.height / 2) - 24,
                                  "NO DEVICES CONFIGURED",
                                  3,
                                  kDark);
        scene::draw_text_centered(buffer,
                                  state.width,
                                  state.height,
                                  layout.body,
                                  layout.body.y + (layout.body.height / 2) + 24,
                                  "OPEN SETUP TO SELECT THEM",
                                  2,
                                  kMid);
        return;
    }

    const int start = page * kDashboardPageSize;
    const int count = std::min(kDashboardPageSize, static_cast<int>(selected.size()) - start);
    const int gutter = 18;

    for (int i = 0; i < count; ++i) {
        const int entity_index = selected[static_cast<std::size_t>(start + i)];
        const EntityItem& entity = state.entities[static_cast<std::size_t>(entity_index)];
        const Rect card = scene::grid_cell(layout.body, 2, 3, i % 2, i / 2, gutter);

        int toggle_button_index = -1;
        int detail_button_index = -1;
        find_button(buttons, ButtonId::DashboardToggleLight, entity_index, &toggle_button_index);
        const Button* detail_button = find_button(buttons, ButtonId::DashboardOpenDetail, entity_index, &detail_button_index);
        const bool pressed = toggle_button_index == state.pressed_button;
        const bool selected_card = toggle_button_index == state.selected_button;
        draw_button_frame(buffer, state.width, state.height, card, pressed, selected_card);

        scene::draw_text(buffer,
                         state.width,
                         state.height,
                         card.x + 18,
                         card.y + 18,
                         scene::fit_text_to_width(scene::uppercase_ascii(entity.name), 3, card.width - 110),
                         3,
                         pressed ? kWhite : kDark);
        scene::draw_text(buffer,
                         state.width,
                         state.height,
                         card.x + 18,
                         card.y + 52,
                         scene::fit_text_to_width(scene::uppercase_ascii(entity.kind_label), 2, card.width - 110),
                         2,
                         pressed ? kLight : kMid);

        scene::draw_text(buffer,
                         state.width,
                         state.height,
                         card.x + 18,
                         card.y + 90,
                         scene::fit_text_to_width(scene::uppercase_ascii(entity.state_label), 4, card.width - 110),
                         4,
                         pressed ? kWhite : kDark);

        if (entity.kind == EntityKind::Climate && !entity.hvac_action.empty()) {
            scene::draw_text(buffer,
                             state.width,
                             state.height,
                             card.x + 18,
                             card.y + 134,
                             scene::fit_text_to_width(scene::uppercase_ascii(entity.hvac_action), 2, card.width - 110),
                             2,
                             pressed ? kLight : kDark);
        }

        scene::draw_text(buffer,
                         state.width,
                         state.height,
                         card.x + 18,
                         card.y + card.height - 46,
                         scene::fit_text_to_width(scene::uppercase_ascii(dashboard_detail(entity)), 2, card.width - 110),
                         2,
                         pressed ? kLight : kMid);

        if (detail_button != nullptr && entity_supports_dashboard_detail(entity)) {
            const bool detail_pressed = detail_button_index == state.pressed_button;
            const bool detail_selected = detail_button_index == state.selected_button;
            draw_text_button(buffer,
                             state.width,
                             state.height,
                             detail_button->rect,
                             "",
                             detail_pressed,
                             detail_selected,
                             2);
            scene::draw_cog_icon(buffer,
                                 state.width,
                                 state.height,
                                 {detail_button->rect.x + 8, detail_button->rect.y + 8, detail_button->rect.width - 16, detail_button->rect.height - 16},
                                 detail_pressed ? kWhite : kDark);
        }
    }
}

void draw_detail_row(RenderBuffer& buffer,
                     int width,
                     int height,
                     const Rect& row,
                     const std::string& label,
                     const std::string& value,
                     const Button* minus_button,
                     int minus_button_index,
                     const Button* plus_button,
                     int plus_button_index,
                     const SceneState& state) {
    scene::fill_rect(buffer, width, height, row, kWhite);
    scene::draw_rect_thick(buffer, width, height, row, 2, kDark);
    scene::draw_text(buffer, width, height, row.x + 18, row.y + 20, scene::uppercase_ascii(label), 3, kDark);
    scene::draw_text(buffer, width, height, row.x + 18, row.y + row.height - 36, scene::uppercase_ascii(value), 2, kMid);

    if (minus_button != nullptr) {
        draw_text_button(buffer,
                         width,
                         height,
                         minus_button->rect,
                         "-",
                         minus_button_index == state.pressed_button,
                         minus_button_index == state.selected_button,
                         4);
    }
    if (plus_button != nullptr) {
        draw_text_button(buffer,
                         width,
                         height,
                         plus_button->rect,
                         "+",
                         plus_button_index == state.pressed_button,
                         plus_button_index == state.selected_button,
                         4);
    }
}

void draw_climate_info_row(RenderBuffer& buffer,
                           int width,
                           int height,
                           const Rect& row,
                           const std::string& label,
                           const std::string& value) {
    scene::fill_rect(buffer, width, height, row, kWhite);
    scene::draw_rect_thick(buffer, width, height, row, 2, kDark);
    scene::draw_text(buffer, width, height, row.x + 18, row.y + 18, scene::uppercase_ascii(label), 2, kMid);
    scene::draw_text(buffer, width, height, row.x + 18, row.y + 40, scene::uppercase_ascii(value), 3, kDark);
}

void draw_detail_view(RenderBuffer& buffer,
                      const SceneState& state,
                      const scene::SceneLayout& layout,
                      const std::vector<Button>& buttons) {
    if (state.detail_entity_index < 0 || state.detail_entity_index >= static_cast<int>(state.entities.size())) {
        draw_header(buffer, state.width, state.height, layout, "DETAIL", "INVALID DEVICE", state.status);
        return;
    }

    const EntityItem& entity = state.entities[static_cast<std::size_t>(state.detail_entity_index)];

    draw_header(buffer,
                state.width,
                state.height,
                layout,
                entity.name,
                scene::uppercase_ascii(entity.entity_id),
                state.status);

    const Rect summary{
        layout.body.x,
        layout.body.y,
        layout.body.width,
        160,
    };
    Color summary_fill = kLight;
    if (buffer.format == PixelFormat::RGBA32) {
        summary_fill = entity.kind == EntityKind::Climate ? scene::active_theme().highlight : scene::active_theme().warning;
    }
    scene::fill_rect(buffer, state.width, state.height, summary, summary_fill);
    scene::draw_rect_thick(buffer, state.width, state.height, summary, 2, kDark);
    scene::draw_text(buffer, state.width, state.height, summary.x + 20, summary.y + 18, scene::uppercase_ascii(entity.state_label), 5, kDark);

    std::string summary_detail;
    if (entity.kind == EntityKind::Light) {
        summary_detail = "BRI " + std::to_string(entity.brightness_percent) + "%";
    } else if (entity.kind == EntityKind::Climate) {
        summary_detail = "NOW " + std::to_string(entity.current_temperature) + "C SET " + std::to_string(entity.target_temperature) + "C";
        if (!entity.hvac_action.empty()) {
            summary_detail += " " + scene::uppercase_ascii(entity.hvac_action);
        }
    } else {
        summary_detail = scene::uppercase_ascii(entity.kind_label);
    }
    scene::draw_text(buffer,
                     state.width,
                     state.height,
                     summary.x + 20,
                     summary.y + 90,
                     scene::fit_text_to_width(scene::uppercase_ascii(summary_detail), 2, summary.width - 40),
                     2,
                     kDark);

    int toggle_button_index = -1;
    const Button* toggle_button = find_button(buttons, ButtonId::DetailToggleLight, state.detail_entity_index, &toggle_button_index);
    if (toggle_button != nullptr) {
        std::string toggle_label = entity.is_on ? "TURN OFF" : "TURN ON";
        if (entity.kind == EntityKind::Climate) {
            toggle_label = entity.is_on ? "HEAT OFF" : "HEAT ON";
        }
        draw_text_button(buffer,
                         state.width,
                         state.height,
                         toggle_button->rect,
                         toggle_label,
                         toggle_button_index == state.pressed_button,
                         toggle_button_index == state.selected_button,
                         3);
    }

    if (entity.kind == EntityKind::Light) {
        if (entity.supports_brightness) {
            int minus_index = -1;
            int plus_index = -1;
            const Button* minus_button = find_button(buttons, ButtonId::DetailBrightnessDown, state.detail_entity_index, &minus_index);
            const Button* plus_button = find_button(buttons, ButtonId::DetailBrightnessUp, state.detail_entity_index, &plus_index);
            draw_detail_row(buffer,
                            state.width,
                            state.height,
                            {layout.body.x, layout.body.y + 248, layout.body.width, 86},
                            "BRIGHTNESS",
                            std::to_string(entity.brightness_percent) + "%",
                            minus_button,
                            minus_index,
                            plus_button,
                            plus_index,
                            state);
        }

        std::vector<ButtonId> action_ids;
        if (entity.supports_rgb) {
            action_ids.push_back(ButtonId::DetailSetRed);
            action_ids.push_back(ButtonId::DetailSetGreen);
            action_ids.push_back(ButtonId::DetailSetBlue);
        }
        if (entity.supports_color_temp) {
            action_ids.push_back(ButtonId::DetailSetDaylight);
            action_ids.push_back(ButtonId::DetailSetNeutral);
            action_ids.push_back(ButtonId::DetailSetWarm);
        }

        for (std::size_t i = 0; i < action_ids.size(); ++i) {
            int button_index = -1;
            const Button* button = find_button(buttons, action_ids[i], state.detail_entity_index, &button_index);
            if (button == nullptr) {
                continue;
            }
            draw_text_button(buffer,
                             state.width,
                             state.height,
                             button->rect,
                             button->label,
                             button_index == state.pressed_button,
                             button_index == state.selected_button,
                             3);
        }
        return;
    }

    if (entity.kind == EntityKind::Climate) {
        draw_climate_info_row(buffer,
                              state.width,
                              state.height,
                              climate_info_row_rect(layout, 0),
                              "CURRENT",
                              std::to_string(entity.current_temperature) + "C");
        draw_climate_info_row(buffer,
                              state.width,
                              state.height,
                              climate_info_row_rect(layout, 1),
                              "TARGET",
                              std::to_string(entity.target_temperature) + "C");
        draw_climate_info_row(buffer,
                              state.width,
                              state.height,
                              climate_info_row_rect(layout, 2),
                              "HEATING",
                              entity.hvac_action.empty() ? entity.state_label : scene::uppercase_ascii(entity.hvac_action));
        return;
    }

    scene::draw_text_centered(buffer,
                              state.width,
                              state.height,
                              layout.body,
                              layout.body.y + 300,
                              "NO EXTRA CONTROLS",
                              3,
                              kMid);
}

void draw_footer_buttons(RenderBuffer& buffer,
                         const SceneState& state,
                         const std::vector<Button>& buttons) {
    for (std::size_t i = 0; i < buttons.size(); ++i) {
        switch (buttons[i].id) {
            case ButtonId::BrightnessToggle:
            case ButtonId::DevModeToggle:
            case ButtonId::SetupToggleLight:
            case ButtonId::DashboardToggleLight:
            case ButtonId::DashboardOpenDetail:
            case ButtonId::DetailToggleLight:
            case ButtonId::DetailBrightnessDown:
            case ButtonId::DetailBrightnessUp:
            case ButtonId::DetailSetRed:
            case ButtonId::DetailSetGreen:
            case ButtonId::DetailSetBlue:
            case ButtonId::DetailSetDaylight:
            case ButtonId::DetailSetNeutral:
            case ButtonId::DetailSetWarm:
                continue;
            default:
                break;
        }

        draw_text_button(buffer,
                         state.width,
                         state.height,
                         buttons[i].rect,
                         buttons[i].label,
                         static_cast<int>(i) == state.pressed_button,
                         static_cast<int>(i) == state.selected_button,
                         3);
    }
}

}  // namespace

std::vector<Button> buttons_for(const SceneState& state) {
    const scene::SceneLayout layout = scene::make_scene_layout(state.width, state.height);
    std::vector<Button> buttons{
        {.id = ButtonId::BrightnessToggle, .label = "BRIGHTNESS", .rect = layout.brightness_button},
        {.id = ButtonId::DevModeToggle, .label = "DEV", .rect = layout.dev_button},
    };

    if (state.view_mode == ViewMode::Setup) {
        const int page = std::clamp(state.setup_page, 0, max_page_index(static_cast<int>(state.entities.size()), kSetupPageSize));
        const int start = page * kSetupPageSize;
        const int count = std::min(kSetupPageSize, static_cast<int>(state.entities.size()) - start);
        const int gutter = 18;
        const int row_height = count > 0 ? (layout.body.height - ((count - 1) * gutter)) / std::max(1, count) : layout.body.height;
        for (int i = 0; i < count; ++i) {
            buttons.push_back({
                .id = ButtonId::SetupToggleLight,
                .label = state.entities[static_cast<std::size_t>(start + i)].name,
                .rect = {layout.body.x, layout.body.y + (i * (row_height + gutter)), layout.body.width, row_height},
                .value = start + i,
            });
        }
        buttons.push_back({.id = ButtonId::SetupRefresh, .label = "REFRESH", .rect = footer_button_rect(layout, 5, 0)});
        buttons.push_back({.id = ButtonId::SetupPreviousPage, .label = "PREV", .rect = footer_button_rect(layout, 5, 1)});
        buttons.push_back({.id = ButtonId::SetupNextPage, .label = "NEXT", .rect = footer_button_rect(layout, 5, 2)});
        buttons.push_back({.id = ButtonId::SetupSave, .label = "SAVE", .rect = footer_button_rect(layout, 5, 3)});
        buttons.push_back({.id = ButtonId::Exit, .label = "EXIT", .rect = footer_button_rect(layout, 5, 4)});
        return buttons;
    }

    if (state.view_mode == ViewMode::Dashboard) {
        const std::vector<int> selected = selected_entity_indices(state);
        const int page = std::clamp(state.dashboard_page, 0, max_page_index(static_cast<int>(selected.size()), kDashboardPageSize));
        const int start = page * kDashboardPageSize;
        const int count = std::min(kDashboardPageSize, static_cast<int>(selected.size()) - start);
        const int gutter = 18;
        for (int i = 0; i < count; ++i) {
            const int entity_index = selected[static_cast<std::size_t>(start + i)];
            const EntityItem& entity = state.entities[static_cast<std::size_t>(entity_index)];
            const Rect card = scene::grid_cell(layout.body, 2, 3, i % 2, i / 2, gutter);
            if (entity_supports_dashboard_detail(entity)) {
                buttons.push_back({
                    .id = ButtonId::DashboardOpenDetail,
                    .label = "DETAIL",
                    .rect = {card.x + card.width - 78, card.y + 14, 64, 64},
                    .value = entity_index,
                });
            }
            buttons.push_back({
                .id = ButtonId::DashboardToggleLight,
                .label = entity.name,
                .rect = card,
                .value = entity_index,
            });
        }
        buttons.push_back({.id = ButtonId::DashboardConfigure, .label = "SETUP", .rect = footer_button_rect(layout, 5, 0)});
        buttons.push_back({.id = ButtonId::DashboardRefresh, .label = "REFRESH", .rect = footer_button_rect(layout, 5, 1)});
        buttons.push_back({.id = ButtonId::DashboardPreviousPage, .label = "PREV", .rect = footer_button_rect(layout, 5, 2)});
        buttons.push_back({.id = ButtonId::DashboardNextPage, .label = "NEXT", .rect = footer_button_rect(layout, 5, 3)});
        buttons.push_back({.id = ButtonId::Exit, .label = "EXIT", .rect = footer_button_rect(layout, 5, 4)});
        return buttons;
    }

    const Rect toggle_rect{
        layout.body.x,
        layout.body.y + 178,
        layout.body.width,
        52,
    };
    buttons.push_back({
        .id = ButtonId::DetailToggleLight,
        .label = "TOGGLE",
        .rect = toggle_rect,
        .value = state.detail_entity_index,
    });

    if (state.detail_entity_index >= 0 && state.detail_entity_index < static_cast<int>(state.entities.size())) {
        const EntityItem& entity = state.entities[static_cast<std::size_t>(state.detail_entity_index)];
        if (entity.kind == EntityKind::Light) {
            if (entity.supports_brightness) {
                const Rect base{layout.body.x, layout.body.y + 248, layout.body.width, 86};
                buttons.push_back({.id = ButtonId::DetailBrightnessDown, .label = "-", .rect = {base.x + base.width - 212, base.y + 10, 88, base.height - 20}, .value = state.detail_entity_index});
                buttons.push_back({.id = ButtonId::DetailBrightnessUp, .label = "+", .rect = {base.x + base.width - 106, base.y + 10, 88, base.height - 20}, .value = state.detail_entity_index});
            }

            std::vector<std::pair<ButtonId, std::string>> action_buttons;
            if (entity.supports_rgb) {
                action_buttons.push_back({ButtonId::DetailSetRed, "RED"});
                action_buttons.push_back({ButtonId::DetailSetGreen, "GREEN"});
                action_buttons.push_back({ButtonId::DetailSetBlue, "BLUE"});
            }
            if (entity.supports_color_temp) {
                action_buttons.push_back({ButtonId::DetailSetDaylight, "DAYLIGHT"});
                action_buttons.push_back({ButtonId::DetailSetNeutral, "NEUTRAL"});
                action_buttons.push_back({ButtonId::DetailSetWarm, "WARM"});
            }

            for (std::size_t i = 0; i < action_buttons.size(); ++i) {
                buttons.push_back({
                    .id = action_buttons[i].first,
                    .label = action_buttons[i].second,
                    .rect = detail_action_button_rect(layout, static_cast<int>(i), static_cast<int>(action_buttons.size())),
                    .value = state.detail_entity_index,
                });
            }
        }
    }

    buttons.push_back({.id = ButtonId::DetailBack, .label = "BACK", .rect = footer_button_rect(layout, 2, 0)});
    buttons.push_back({.id = ButtonId::Exit, .label = "EXIT", .rect = footer_button_rect(layout, 2, 1)});
    return buttons;
}

int button_at(const std::vector<Button>& buttons, int x, int y) {
    for (std::size_t i = 0; i < buttons.size(); ++i) {
        if (scene::contains(buttons[i].rect, x, y)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

RenderBuffer render_scene(const SceneState& state, const std::vector<Button>& buttons, PixelFormat pixel_format) {
    scene::set_active_theme(scene::theme_for(pixel_format));
    RenderBuffer buffer = scene::make_render_buffer(state.width, state.height, pixel_format, kWhite);
    const scene::SceneLayout layout = scene::make_scene_layout(state.width, state.height);

    int brightness_button_index = -1;
    int dev_button_index = -1;
    find_button(buttons, ButtonId::BrightnessToggle, -1, &brightness_button_index);
    find_button(buttons, ButtonId::DevModeToggle, -1, &dev_button_index);
    draw_top_bar(buffer,
                 state,
                 layout,
                 brightness_button_index == state.pressed_button,
                 dev_button_index == state.pressed_button);

    scene::fill_rect(buffer, state.width, state.height, layout.outer, kWhite);
    scene::draw_rect_thick(buffer, state.width, state.height, layout.outer, 2, kDark);

    switch (state.view_mode) {
        case ViewMode::Setup:
            draw_setup_view(buffer, state, layout, buttons);
            break;
        case ViewMode::Dashboard:
            draw_dashboard_view(buffer, state, layout, buttons);
            break;
        case ViewMode::Detail:
            draw_detail_view(buffer, state, layout, buttons);
            break;
    }

    draw_footer_buttons(buffer, state, buttons);
    return buffer;
}

}  // namespace hadisplay
