#include "scene.h"

#include "scene_draw.h"
#include "scene_icons.h"
#include "scene_layout.h"
#include "scene_style.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <set>
#include <sstream>
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

int max_page_index(int item_count, int page_size) {
    if (item_count <= 0) {
        return 0;
    }
    return std::max(0, (item_count - 1) / page_size);
}

std::string lowercase_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string setup_room_name_for_entity(const EntityItem& entity) {
    return entity.room_label.empty() ? "UNASSIGNED" : entity.room_label;
}

std::string uppercase_room_name_for_entity(const EntityItem& entity) {
    return scene::uppercase_ascii(setup_room_name_for_entity(entity));
}

bool matches_setup_type_filter(const EntityItem& entity, SetupTypeFilter filter) {
    switch (filter) {
        case SetupTypeFilter::All: return true;
        case SetupTypeFilter::Lights: return entity.kind == EntityKind::Light;
        case SetupTypeFilter::Switches: return entity.kind == EntityKind::Switch;
        case SetupTypeFilter::Climate: return entity.kind == EntityKind::Climate;
        case SetupTypeFilter::Sensors: return entity.kind == EntityKind::Sensor;
    }
    return true;
}

bool matches_hidden_patterns(const EntityItem& entity, const std::vector<std::string>& patterns) {
    if (patterns.empty()) {
        return false;
    }

    const std::string haystack = lowercase_ascii(entity.entity_id + "\n" + entity.name + "\n" + setup_room_name_for_entity(entity));
    for (const std::string& pattern : patterns) {
        if (!pattern.empty() && haystack.find(lowercase_ascii(pattern)) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::vector<int> filtered_setup_entity_indices(const SceneState& state) {
    std::vector<int> indices;
    for (std::size_t i = 0; i < state.entities.size(); ++i) {
        const EntityItem& entity = state.entities[i];
        if (!matches_setup_type_filter(entity, state.setup_type_filter)) {
            continue;
        }
        if (matches_hidden_patterns(entity, state.hidden_entity_patterns)) {
            continue;
        }
        if (state.setup_browse_mode == SetupBrowseMode::Rooms &&
            !state.setup_room_label.empty() &&
            setup_room_name_for_entity(entity) != state.setup_room_label) {
            continue;
        }
        indices.push_back(static_cast<int>(i));
    }

    std::sort(indices.begin(), indices.end(), [&](int left_index, int right_index) {
        const EntityItem& left = state.entities[static_cast<std::size_t>(left_index)];
        const EntityItem& right = state.entities[static_cast<std::size_t>(right_index)];
        if (state.setup_browse_mode == SetupBrowseMode::List) {
            if (left.kind != right.kind) {
                return static_cast<int>(left.kind) < static_cast<int>(right.kind);
            }
        }
        return lowercase_ascii(left.name) < lowercase_ascii(right.name);
    });
    return indices;
}

std::vector<std::string> filtered_setup_rooms(const SceneState& state) {
    std::vector<std::string> rooms;
    std::set<std::string> seen;
    for (int entity_index : filtered_setup_entity_indices(state)) {
        const std::string room = setup_room_name_for_entity(state.entities[static_cast<std::size_t>(entity_index)]);
        if (seen.insert(room).second) {
            rooms.push_back(room);
        }
    }
    std::sort(rooms.begin(), rooms.end(), [](const std::string& left, const std::string& right) {
        return lowercase_ascii(left) < lowercase_ascii(right);
    });
    return rooms;
}

std::vector<int> setup_page_entity_indices(const SceneState& state) {
    const std::vector<int> filtered = filtered_setup_entity_indices(state);
    const int page = std::clamp(state.setup_page, 0, max_page_index(static_cast<int>(filtered.size()), kSetupPageSize));
    const int start = page * kSetupPageSize;
    const int count = std::min(kSetupPageSize, static_cast<int>(filtered.size()) - start);

    std::vector<int> page_entities;
    page_entities.reserve(std::max(0, count));
    for (int i = 0; i < count; ++i) {
        page_entities.push_back(filtered[static_cast<std::size_t>(start + i)]);
    }
    return page_entities;
}

std::string setup_type_filter_label(SetupTypeFilter filter) {
    switch (filter) {
        case SetupTypeFilter::All: return "ALL";
        case SetupTypeFilter::Lights: return "LIGHTS";
        case SetupTypeFilter::Switches: return "SWITCHES";
        case SetupTypeFilter::Climate: return "CLIMATE";
        case SetupTypeFilter::Sensors: return "SENSORS";
    }
    return "ALL";
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

Rect setup_header_button_rect(const scene::SceneLayout& layout, int index, int count) {
    const int gutter = 14;
    const int width = std::clamp((layout.header.width - 120 - ((count - 1) * gutter)) / std::max(1, count), 160, 210);
    const int height = std::clamp(layout.header.height / 3, 40, 48);
    const int total_width = (count * width) + ((count - 1) * gutter);
    const int y = layout.header.y + layout.header.height - height - 6;
    return {
        layout.header.x + layout.header.width - total_width + (index * (width + gutter)),
        y,
        width,
        height,
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
                          buffer.format == PixelFormat::RGBA32 ? scene::active_theme().accent_red : kMid,
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
    } else if (entity.kind == EntityKind::Climate) {
        if (entity.target_temperature > 0) {
            summary += " SET " + std::to_string(entity.target_temperature) + "C";
        }
        if (!entity.hvac_action.empty()) {
            summary += " " + scene::uppercase_ascii(entity.hvac_action);
        }
    } else {
        if (!entity.device_class.empty()) {
            summary = entity.device_class;
            std::replace(summary.begin(), summary.end(), '_', ' ');
            summary = scene::uppercase_ascii(summary);
        }
        if (!entity.unit_label.empty()) {
            summary += " " + entity.unit_label;
        }
        if (entity.supports_history) {
            summary += " GRAPH";
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
    if (entity.kind == EntityKind::Sensor) {
        std::string detail = entity.device_class.empty() ? "SENSOR" : entity.device_class;
        std::replace(detail.begin(), detail.end(), '_', ' ');
        detail = scene::uppercase_ascii(detail);
        if (!entity.unit_label.empty()) {
            detail += " " + entity.unit_label;
        }
        return detail;
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
    return entity.kind == EntityKind::Sensor || entity.supports_detail;
}

std::string format_metric_value(double value) {
    std::ostringstream stream;
    const double rounded = std::round(value);
    if (std::fabs(value - rounded) < 0.05) {
        stream << static_cast<long long>(rounded);
    } else if (std::fabs(value) >= 100.0) {
        stream << std::fixed << std::setprecision(0) << value;
    } else if (std::fabs(value) >= 10.0) {
        stream << std::fixed << std::setprecision(1) << value;
    } else {
        stream << std::fixed << std::setprecision(2) << value;
    }
    return stream.str();
}

void draw_sensor_history_graph(RenderBuffer& buffer,
                               int width,
                               int height,
                               const Rect& bounds,
                               const SceneState& state,
                               const EntityItem& entity) {
    scene::fill_rect(buffer, width, height, bounds, kWhite);
    scene::draw_rect_thick(buffer, width, height, bounds, 2, kDark);
    scene::draw_text(buffer, width, height, bounds.x + 18, bounds.y + 18, "24H HISTORY", 3, kDark);

    if (state.detail_history_loading && state.detail_history_entity_id == entity.entity_id) {
        scene::draw_text_centered(buffer,
                                  width,
                                  height,
                                  bounds,
                                  bounds.y + (bounds.height / 2) - 12,
                                  "LOADING HISTORY",
                                  3,
                                  kMid);
        return;
    }

    if (!state.detail_history_available ||
        state.detail_history_entity_id != entity.entity_id ||
        state.detail_history_values.size() < 2) {
        scene::draw_text_centered(buffer,
                                  width,
                                  height,
                                  bounds,
                                  bounds.y + (bounds.height / 2) - 12,
                                  "NO HISTORY",
                                  3,
                                  kMid);
        return;
    }

    const std::string unit_suffix = entity.unit_label.empty() ? "" : (" " + entity.unit_label);
    const std::string current_label = "NOW " + format_metric_value(state.detail_history_values.back()) + unit_suffix;
    scene::draw_text(buffer,
                     width,
                     height,
                     bounds.x + bounds.width - 240,
                     bounds.y + 18,
                     scene::fit_text_to_width(current_label, 2, 220),
                     2,
                     kDark);

    constexpr int kAxisLabelWidth = 84;
    constexpr int kBottomLabelHeight = 42;
    const Rect plot{
        bounds.x + 18 + kAxisLabelWidth,
        bounds.y + 56,
        bounds.width - 36 - kAxisLabelWidth,
        bounds.height - 84 - kBottomLabelHeight,
    };
    scene::fill_rect(buffer, width, height, plot, kWhite);
    scene::draw_rect_thick(buffer, width, height, plot, 2, kMid);

    const auto& values = state.detail_history_values;
    const double min_value = state.detail_history_min;
    const double max_value = state.detail_history_max;
    const double span = std::max(0.001, max_value - min_value);
    const double mid_value = min_value + (span * 0.5);
    const int drawable_width = std::max(1, plot.width - 12);
    const Color graph_color = buffer.format == PixelFormat::RGBA32 ? scene::active_theme().accent_blue : kDark;

    auto y_for_value = [&](double value) {
        const double normalized = (value - min_value) / span;
        const double clamped = std::clamp(normalized, 0.0, 1.0);
        return plot.y + plot.height - 8 - static_cast<int>(std::lround(clamped * static_cast<double>(plot.height - 16)));
    };

    const int top_y = y_for_value(max_value);
    const int mid_y = y_for_value(mid_value);
    const int bottom_y = y_for_value(min_value);
    scene::draw_line(buffer, width, height, plot.x + 1, top_y, plot.x + plot.width - 2, top_y, 1, kLight);
    scene::draw_line(buffer, width, height, plot.x + 1, mid_y, plot.x + plot.width - 2, mid_y, 1, kLight);
    scene::draw_line(buffer, width, height, plot.x + 1, bottom_y, plot.x + plot.width - 2, bottom_y, 1, kLight);

    int prev_x = plot.x + 6;
    int prev_y = y_for_value(values.front());
    for (int x = 1; x < drawable_width; ++x) {
        const std::size_t index = static_cast<std::size_t>((static_cast<long long>(x) * static_cast<long long>(values.size() - 1)) /
                                                           std::max<long long>(1, drawable_width - 1));
        const int draw_x = plot.x + 6 + x;
        const int draw_y = y_for_value(values[index]);
        scene::draw_line(buffer, width, height, prev_x, prev_y, draw_x, draw_y, 2, graph_color);
        prev_x = draw_x;
        prev_y = draw_y;
    }

    const Rect y_axis{
        bounds.x + 14,
        plot.y,
        kAxisLabelWidth - 12,
        plot.height,
    };
    const auto draw_right_aligned_label = [&](int y, const std::string& label, Color color) {
        const std::string fitted = scene::fit_text_to_width(label, 2, y_axis.width);
        const int x = y_axis.x + std::max(0, y_axis.width - scene::text_width(fitted, 2));
        scene::draw_text(buffer, width, height, x, y, fitted, 2, color);
    };
    draw_right_aligned_label(y_axis.y + 8, format_metric_value(max_value) + unit_suffix, kMid);
    draw_right_aligned_label(y_axis.y + (y_axis.height / 2) + 6, format_metric_value(mid_value) + unit_suffix, kMid);
    draw_right_aligned_label(y_axis.y + y_axis.height - 10, format_metric_value(min_value) + unit_suffix, kMid);

    const Rect x_axis{
        plot.x,
        plot.y + plot.height + 10,
        plot.width,
        kBottomLabelHeight,
    };
    scene::draw_text(buffer,
                     width,
                     height,
                     x_axis.x,
                     x_axis.y + 18,
                     "24H AGO",
                     2,
                     kMid);
    scene::draw_text_centered(buffer,
                              width,
                              height,
                              x_axis,
                              x_axis.y + 18,
                              "12H",
                              2,
                              kMid);
    const std::string now_label = "NOW";
    scene::draw_text(buffer,
                     width,
                     height,
                     x_axis.x + std::max(0, x_axis.width - scene::text_width(now_label, 2)),
                     x_axis.y + 18,
                     now_label,
                     2,
                     kMid);
}

void draw_setup_view(RenderBuffer& buffer,
                     const SceneState& state,
                     const scene::SceneLayout& layout,
                     const std::vector<Button>& buttons) {
    const std::vector<int> filtered_entities = filtered_setup_entity_indices(state);
    const std::vector<std::string> filtered_rooms = filtered_setup_rooms(state);
    const int gutter = 18;
    const bool room_list_view = state.setup_browse_mode == SetupBrowseMode::Rooms && state.setup_room_label.empty();

    if (room_list_view) {
        const int page = std::clamp(state.setup_page, 0, max_page_index(static_cast<int>(filtered_rooms.size()), kSetupPageSize));
        const int start = page * kSetupPageSize;
        const int count = std::min(kSetupPageSize, static_cast<int>(filtered_rooms.size()) - start);
        const int row_height = count > 0 ? (layout.body.height - ((count - 1) * gutter)) / std::max(1, count) : layout.body.height;
        const std::string subtitle = std::to_string(selected_entity_count(state)) + " SELECTED / " +
                                     std::to_string(static_cast<int>(filtered_rooms.size())) + " ROOMS";
        draw_header(buffer, state.width, state.height, layout, "SELECT BY ROOM", subtitle, state.status);

        if (filtered_rooms.empty()) {
            scene::draw_text_centered(buffer,
                                      state.width,
                                      state.height,
                                      layout.body,
                                      layout.body.y + (layout.body.height / 2) - 24,
                                      "NO ROOMS MATCH FILTERS",
                                      3,
                                      kDark);
            scene::draw_text_centered(buffer,
                                      state.width,
                                      state.height,
                                      layout.body,
                                      layout.body.y + (layout.body.height / 2) + 24,
                                      "CHANGE TYPE OR CONFIG",
                                      2,
                                      kMid);
            return;
        }

        for (int i = 0; i < count; ++i) {
            const std::string& room = filtered_rooms[static_cast<std::size_t>(start + i)];
            const Rect row{
                layout.body.x,
                layout.body.y + (i * (row_height + gutter)),
                layout.body.width,
                row_height,
            };
            int button_index = -1;
            find_button(buttons, ButtonId::SetupOpenRoom, start + i, &button_index);
            const bool pressed = button_index == state.pressed_button;
            const bool selected = button_index == state.selected_button;
            draw_button_frame(buffer, state.width, state.height, row, pressed, selected);

            int total_in_room = 0;
            int selected_in_room = 0;
            for (int entity_index : filtered_entities) {
                const EntityItem& entity = state.entities[static_cast<std::size_t>(entity_index)];
                if (setup_room_name_for_entity(entity) != room) {
                    continue;
                }
                ++total_in_room;
                if (entity.selected) {
                    ++selected_in_room;
                }
            }

            scene::draw_text(buffer,
                             state.width,
                             state.height,
                             row.x + 24,
                             row.y + 18,
                             scene::fit_text_to_width(scene::uppercase_ascii(room), 3, row.width - 48),
                             3,
                             pressed ? kWhite : kDark);
            scene::draw_text(buffer,
                             state.width,
                             state.height,
                             row.x + 24,
                             row.y + row.height - 54,
                             scene::fit_text_to_width(std::to_string(total_in_room) + " DEVICES", 2, (row.width / 2) - 32),
                             2,
                             pressed ? kLight : kMid);
            scene::draw_text(buffer,
                             state.width,
                             state.height,
                             row.x + row.width - 220,
                             row.y + row.height - 54,
                             scene::fit_text_to_width(std::to_string(selected_in_room) + "/" + std::to_string(total_in_room) + " SELECTED", 2, 200),
                             2,
                             pressed ? kWhite : kDark);
        }
        return;
    }

    const std::vector<int> page_entities = setup_page_entity_indices(state);
    const int count = static_cast<int>(page_entities.size());
    const int row_height = count > 0 ? (layout.body.height - ((count - 1) * gutter)) / std::max(1, count) : layout.body.height;
    const std::string title = state.setup_room_label.empty() ? "SELECT DEVICES" : state.setup_room_label;
    const std::string subtitle = std::to_string(selected_entity_count(state)) + " SELECTED / " +
                                 std::to_string(static_cast<int>(filtered_entities.size())) + " SHOWN";
    draw_header(buffer, state.width, state.height, layout, title, subtitle, state.status);

    if (page_entities.empty()) {
        scene::draw_text_centered(buffer,
                                  state.width,
                                  state.height,
                                  layout.body,
                                  layout.body.y + (layout.body.height / 2) - 24,
                                  "NO DEVICES MATCH FILTERS",
                                  3,
                                  kDark);
        scene::draw_text_centered(buffer,
                                  state.width,
                                  state.height,
                                  layout.body,
                                  layout.body.y + (layout.body.height / 2) + 24,
                                  "CHANGE TYPE OR CONFIG",
                                  2,
                                  kMid);
        return;
    }

    for (int i = 0; i < count; ++i) {
        const int entity_index = page_entities[static_cast<std::size_t>(i)];
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
                         row.x + row.width - 220,
                         row.y + 18,
                         scene::fit_text_to_width(scene::uppercase_ascii(state.setup_room_label.empty() ? uppercase_room_name_for_entity(entity) : entity.kind_label), 2, 202),
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
        const int primary_button_index = toggle_button_index >= 0 ? toggle_button_index : detail_button_index;
        const bool pressed = primary_button_index == state.pressed_button;
        const bool selected_card = primary_button_index == state.selected_button;
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
        } else if (entity.kind == EntityKind::Sensor && !entity.unit_label.empty()) {
            scene::draw_text(buffer,
                             state.width,
                             state.height,
                             card.x + 18,
                             card.y + 134,
                             scene::fit_text_to_width(scene::uppercase_ascii(entity.unit_label), 2, card.width - 110),
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

        if (detail_button != nullptr &&
            entity_supports_dashboard_detail(entity) &&
            !(detail_button->rect.x == card.x &&
              detail_button->rect.y == card.y &&
              detail_button->rect.width == card.width &&
              detail_button->rect.height == card.height)) {
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
        if (entity.kind == EntityKind::Climate || entity.kind == EntityKind::Sensor) {
            summary_fill = scene::active_theme().highlight;
        } else {
            summary_fill = scene::active_theme().warning;
        }
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
    } else if (entity.kind == EntityKind::Sensor) {
        summary_detail = entity.device_class.empty() ? "SENSOR" : entity.device_class;
        std::replace(summary_detail.begin(), summary_detail.end(), '_', ' ');
        summary_detail = scene::uppercase_ascii(summary_detail);
        if (!entity.unit_label.empty()) {
            summary_detail += " " + entity.unit_label;
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

    if (entity.kind == EntityKind::Sensor) {
        draw_sensor_history_graph(buffer,
                                  state.width,
                                  state.height,
                                  {layout.body.x, layout.body.y + 248, layout.body.width, layout.body.height - 248},
                                  state,
                                  entity);
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
            case ButtonId::SetupOpenRoom:
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
        const bool room_list_view = state.setup_browse_mode == SetupBrowseMode::Rooms && state.setup_room_label.empty();
        const int header_control_count = 2;
        const int footer_count = 4;
        const int gutter = 18;
        if (room_list_view) {
            const std::vector<std::string> rooms = filtered_setup_rooms(state);
            const int page = std::clamp(state.setup_page, 0, max_page_index(static_cast<int>(rooms.size()), kSetupPageSize));
            const int start = page * kSetupPageSize;
            const int count = std::min(kSetupPageSize, static_cast<int>(rooms.size()) - start);
            const int row_height = count > 0 ? (layout.body.height - ((count - 1) * gutter)) / std::max(1, count) : layout.body.height;
            for (int i = 0; i < count; ++i) {
                buttons.push_back({
                    .id = ButtonId::SetupOpenRoom,
                    .label = rooms[static_cast<std::size_t>(start + i)],
                    .rect = {layout.body.x, layout.body.y + (i * (row_height + gutter)), layout.body.width, row_height},
                    .value = start + i,
                });
            }
        } else {
            const std::vector<int> page_entities = setup_page_entity_indices(state);
            const int count = static_cast<int>(page_entities.size());
            const int row_height = count > 0 ? (layout.body.height - ((count - 1) * gutter)) / std::max(1, count) : layout.body.height;
            for (int i = 0; i < count; ++i) {
                const int entity_index = page_entities[static_cast<std::size_t>(i)];
                buttons.push_back({
                    .id = ButtonId::SetupToggleLight,
                    .label = state.entities[static_cast<std::size_t>(entity_index)].name,
                    .rect = {layout.body.x, layout.body.y + (i * (row_height + gutter)), layout.body.width, row_height},
                    .value = entity_index,
                });
            }
        }
        buttons.push_back({.id = ButtonId::SetupCycleBrowseMode,
                           .label = state.setup_browse_mode == SetupBrowseMode::List ? "ROOMS" : (state.setup_room_label.empty() ? "LIST" : "BACK"),
                           .rect = setup_header_button_rect(layout, 0, header_control_count)});
        buttons.push_back({.id = ButtonId::SetupCycleTypeFilter,
                           .label = setup_type_filter_label(state.setup_type_filter),
                           .rect = setup_header_button_rect(layout, 1, header_control_count)});
        buttons.push_back({.id = ButtonId::SetupRefresh, .label = "REFRESH", .rect = footer_button_rect(layout, footer_count, 0)});
        buttons.push_back({.id = ButtonId::SetupPreviousPage, .label = "PREV", .rect = footer_button_rect(layout, footer_count, 1)});
        buttons.push_back({.id = ButtonId::SetupNextPage, .label = "NEXT", .rect = footer_button_rect(layout, footer_count, 2)});
        buttons.push_back({.id = ButtonId::SetupSave, .label = "SAVE", .rect = footer_button_rect(layout, footer_count, 3)});
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
            if (entity.kind == EntityKind::Sensor && entity_supports_dashboard_detail(entity)) {
                buttons.push_back({
                    .id = ButtonId::DashboardOpenDetail,
                    .label = entity.name,
                    .rect = card,
                    .value = entity_index,
                });
                continue;
            }
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

    if (state.detail_entity_index >= 0 && state.detail_entity_index < static_cast<int>(state.entities.size())) {
        const EntityItem& entity = state.entities[static_cast<std::size_t>(state.detail_entity_index)];
        if (entity.kind == EntityKind::Light || entity.kind == EntityKind::Climate) {
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
        }
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
