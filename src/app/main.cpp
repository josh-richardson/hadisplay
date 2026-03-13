#include "app_config.h"
#include "ha_client.h"
#include "scene.h"
#include "system_status.h"

#include <fbink.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <signal.h>

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <iostream>
#include <optional>
#include <poll.h>
#include <set>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

constexpr const char* kTouchDevice = "/dev/input/event1";
constexpr int kTouchMaxX = 1447;
constexpr int kTouchMaxY = 1071;
constexpr int kMaxPartialRefreshes = 12;
constexpr auto kFullRefreshInterval = std::chrono::minutes(5);
constexpr auto kNormalLightPollInterval = std::chrono::minutes(1);
constexpr auto kNormalDevicePollInterval = std::chrono::minutes(5);
constexpr auto kNormalWeatherPollInterval = std::chrono::hours(1);
constexpr auto kDevPollInterval = std::chrono::seconds(10);

volatile sig_atomic_t g_running = 1;

struct TouchState {
    int x = -1;
    int y = -1;
    bool touching = false;
};

struct RenderState {
    int partial_refreshes = 0;
    std::chrono::steady_clock::time_point last_full_refresh = std::chrono::steady_clock::now();
};

struct ClockStatus {
    std::string time_label;
    std::string date_label;
};

std::string uppercase_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return value;
}

ClockStatus current_clock_status() {
    const std::time_t now = std::time(nullptr);
    std::tm local_tm{};
    localtime_r(&now, &local_tm);

    char time_buffer[6]{};
    char date_buffer[16]{};
    std::strftime(time_buffer, sizeof(time_buffer), "%H:%M", &local_tm);
    std::strftime(date_buffer, sizeof(date_buffer), "%a %d %b", &local_tm);
    return {time_buffer, uppercase_ascii(date_buffer)};
}

bool update_clock_status(hadisplay::SceneState& scene_state) {
    const ClockStatus clock = current_clock_status();
    if (scene_state.time_label == clock.time_label && scene_state.date_label == clock.date_label) {
        return false;
    }
    scene_state.time_label = clock.time_label;
    scene_state.date_label = clock.date_label;
    return true;
}

std::chrono::steady_clock::time_point next_minute_deadline() {
    const auto system_now = std::chrono::system_clock::now();
    const auto next_minute = std::chrono::time_point_cast<std::chrono::minutes>(system_now) + std::chrono::minutes(1);
    return std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(next_minute - system_now);
}

int poll_timeout_until(const std::chrono::steady_clock::time_point& now,
                       const std::chrono::steady_clock::time_point& next_full_refresh,
                       const std::chrono::steady_clock::time_point& next_clock_refresh,
                       const std::chrono::steady_clock::time_point& next_light_refresh,
                       const std::chrono::steady_clock::time_point& next_device_refresh,
                       const std::chrono::steady_clock::time_point& next_weather_refresh) {
    const auto next_due = std::min({next_full_refresh, next_clock_refresh, next_light_refresh, next_device_refresh, next_weather_refresh});
    if (next_due <= now) {
        return 0;
    }
    return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(next_due - now).count());
}

void apply_system_status(hadisplay::SceneState& scene_state, const hadisplay::SystemStatus& status) {
    scene_state.wifi_label = status.wifi_label;
    scene_state.wifi_connected = status.wifi_connected;
    scene_state.battery_label = status.battery_label;
    scene_state.battery_available = status.battery_available;
    scene_state.battery_percent = status.battery_percent;
    scene_state.battery_charging = status.battery_charging;
    scene_state.brightness_label = status.brightness_label;
    scene_state.brightness_percent = status.brightness_percent;
    scene_state.brightness_available = status.brightness_available;
}

bool update_system_status(hadisplay::SceneState& scene_state, const hadisplay::SystemStatus& status) {
    if (scene_state.wifi_label == status.wifi_label &&
        scene_state.wifi_connected == status.wifi_connected &&
        scene_state.battery_label == status.battery_label &&
        scene_state.battery_available == status.battery_available &&
        scene_state.battery_percent == status.battery_percent &&
        scene_state.battery_charging == status.battery_charging &&
        scene_state.brightness_label == status.brightness_label &&
        scene_state.brightness_percent == status.brightness_percent &&
        scene_state.brightness_available == status.brightness_available) {
        return false;
    }

    apply_system_status(scene_state, status);
    return true;
}

void apply_weather_state(hadisplay::SceneState& scene_state, const ha::WeatherState& weather) {
    scene_state.weather_available = weather.ok;
    scene_state.weather_condition = weather.condition;
    if (weather.ok) {
        scene_state.weather_range_label = std::to_string(weather.temperature_low) + "/" +
                                         std::to_string(weather.temperature_high) +
                                         weather.temperature_unit;
    } else {
        scene_state.weather_range_label = "--/--";
    }
}

bool update_weather_state(hadisplay::SceneState& scene_state, const ha::WeatherState& weather) {
    const std::string new_range = weather.ok
        ? std::to_string(weather.temperature_low) + "/" + std::to_string(weather.temperature_high) + weather.temperature_unit
        : "--/--";
    if (scene_state.weather_available == weather.ok &&
        scene_state.weather_condition == weather.condition &&
        scene_state.weather_range_label == new_range) {
        return false;
    }

    apply_weather_state(scene_state, weather);
    return true;
}

void signal_handler(int) {
    g_running = 0;
}

void map_touch_to_scene(int raw_x, int raw_y, int scene_w, int scene_h, int& out_x, int& out_y) {
    out_x = (kTouchMaxY - raw_y) * scene_w / (kTouchMaxY + 1);
    out_y = raw_x * scene_h / (kTouchMaxX + 1);
}

std::string concise_ha_error(const std::string& message) {
    const std::string lower = [] (std::string value) {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return value;
    }(message);

    if (lower.find(".env") != std::string::npos || lower.find("ha_url") != std::string::npos ||
        lower.find("ha_token") != std::string::npos) {
        return "CHECK CONFIG";
    }
    if (lower.find("401") != std::string::npos || lower.find("403") != std::string::npos) {
        return "HA AUTH FAILED";
    }
    if (lower.find("404") != std::string::npos) {
        return "ENTITY NOT FOUND";
    }
    if (lower.find("timed out") != std::string::npos || lower.find("couldn't connect") != std::string::npos ||
        lower.find("could not resolve host") != std::string::npos || lower.find("failed to connect") != std::string::npos) {
        return "HA UNREACHABLE";
    }
    return "HA REQUEST FAILED";
}

std::string state_label_from_text(const std::string& state) {
    if (state == "on") {
        return "ON";
    }
    if (state == "off") {
        return "OFF";
    }
    if (state == "unavailable") {
        return "UNAVAILABLE";
    }
    if (state == "unknown") {
        return "UNKNOWN";
    }
    return uppercase_ascii(state);
}

hadisplay::EntityKind scene_entity_kind(ha::EntityKind kind) {
    switch (kind) {
        case ha::EntityKind::Light: return hadisplay::EntityKind::Light;
        case ha::EntityKind::Switch: return hadisplay::EntityKind::Switch;
        case ha::EntityKind::Climate: return hadisplay::EntityKind::Climate;
    }
    return hadisplay::EntityKind::Light;
}

std::string entity_kind_label(hadisplay::EntityKind kind) {
    switch (kind) {
        case hadisplay::EntityKind::Light: return "LIGHT";
        case hadisplay::EntityKind::Switch: return "SOCKET";
        case hadisplay::EntityKind::Climate: return "THERMOSTAT";
    }
    return "DEVICE";
}

std::string climate_state_label(const ha::EntityState& entity) {
    if (!entity.available) {
        return "UNAVAILABLE";
    }
    if (entity.state == "off") {
        return "HEAT OFF";
    }
    if (entity.hvac_action == "heating") {
        return "HEATING";
    }
    if (entity.state == "heat") {
        return "HEAT ON";
    }
    return uppercase_ascii(entity.state);
}

hadisplay::EntityItem entity_item_from_state(const ha::EntityState& entity, bool selected) {
    hadisplay::EntityItem item;
    item.kind = scene_entity_kind(entity.kind);
    item.entity_id = entity.entity_id;
    item.name = entity.friendly_name;
    item.kind_label = entity_kind_label(item.kind);
    item.state_label = entity.kind == ha::EntityKind::Climate ? climate_state_label(entity) : state_label_from_text(entity.state);
    item.is_on = entity.is_on;
    item.available = entity.available;
    item.selected = selected;
    item.supports_detail = entity.supports_detail;
    item.supports_brightness = entity.supports_brightness;
    item.supports_color_temp = entity.supports_color_temp;
    item.supports_rgb = entity.supports_rgb;
    item.supports_heat_control = entity.supports_heat_control;
    item.brightness_percent = entity.brightness_percent;
    item.color_temp_kelvin = entity.color_temp_kelvin;
    item.min_color_temp_kelvin = entity.min_color_temp_kelvin;
    item.max_color_temp_kelvin = entity.max_color_temp_kelvin;
    item.rgb_red = entity.rgb_red;
    item.rgb_green = entity.rgb_green;
    item.rgb_blue = entity.rgb_blue;
    item.current_temperature = entity.current_temperature;
    item.target_temperature = entity.target_temperature;
    item.hvac_action = entity.hvac_action;
    return item;
}

std::optional<int> find_entity_index(const hadisplay::SceneState& scene_state, const std::string& entity_id) {
    for (std::size_t i = 0; i < scene_state.entities.size(); ++i) {
        if (scene_state.entities[i].entity_id == entity_id) {
            return static_cast<int>(i);
        }
    }
    return std::nullopt;
}

void apply_entity_update(hadisplay::SceneState& scene_state, int entity_index, const ha::EntityState& entity_state) {
    const bool selected = scene_state.entities[static_cast<std::size_t>(entity_index)].selected;
    scene_state.entities[static_cast<std::size_t>(entity_index)] = entity_item_from_state(entity_state, selected);
}

hadisplay::AppConfig config_from_scene(const hadisplay::SceneState& scene_state, const hadisplay::AppConfig& base_config) {
    hadisplay::AppConfig config = base_config;
    config.selected_entity_ids.clear();
    for (const hadisplay::EntityItem& entity : scene_state.entities) {
        if (entity.selected) {
            config.selected_entity_ids.push_back(entity.entity_id);
        }
    }
    return config;
}

void populate_entities(hadisplay::SceneState& scene_state,
                       const ha::EntityListResult& list_result,
                       const hadisplay::AppConfig& config) {
    std::set<std::string> selected_ids(config.selected_entity_ids.begin(), config.selected_entity_ids.end());
    std::string detail_entity_id;
    if (scene_state.detail_entity_index >= 0 &&
        scene_state.detail_entity_index < static_cast<int>(scene_state.entities.size())) {
        detail_entity_id = scene_state.entities[static_cast<std::size_t>(scene_state.detail_entity_index)].entity_id;
    }

    scene_state.entities.clear();
    scene_state.entities.reserve(list_result.entities.size());
    for (const ha::EntityState& entity : list_result.entities) {
        scene_state.entities.push_back(entity_item_from_state(entity, selected_ids.contains(entity.entity_id)));
    }

    if (!detail_entity_id.empty()) {
        const std::optional<int> detail_index = find_entity_index(scene_state, detail_entity_id);
        scene_state.detail_entity_index = detail_index.value_or(-1);
    } else {
        scene_state.detail_entity_index = -1;
    }
}

bool refresh_entities(hadisplay::SceneState& scene_state,
                      const ha::Client& ha_client,
                      const hadisplay::AppConfig& config) {
    if (!ha_client.configured()) {
        scene_state.status = "CHECK CONFIG";
        scene_state.entities.clear();
        return false;
    }

    const ha::EntityListResult list_result = ha_client.list_entities();
    if (!list_result.ok) {
        scene_state.status = concise_ha_error(list_result.message);
        return false;
    }

    populate_entities(scene_state, list_result, config);
    if (scene_state.view_mode == hadisplay::ViewMode::Dashboard) {
        bool any_selected = false;
        for (const auto& entity : scene_state.entities) {
            if (entity.selected) {
                any_selected = true;
                break;
            }
        }
        if (!any_selected) {
            scene_state.view_mode = hadisplay::ViewMode::Setup;
        }
    }
    if (scene_state.view_mode == hadisplay::ViewMode::Detail && scene_state.detail_entity_index < 0) {
        scene_state.view_mode = hadisplay::ViewMode::Dashboard;
    }
    scene_state.status = "DEVICES SYNCED";
    return true;
}

bool should_do_full_refresh(const RenderState& render_state, bool force_full_refresh) {
    if (force_full_refresh) {
        return true;
    }
    if (render_state.partial_refreshes >= kMaxPartialRefreshes) {
        return true;
    }
    return (std::chrono::steady_clock::now() - render_state.last_full_refresh) >= kFullRefreshInterval;
}

bool render(int fbfd,
            const hadisplay::SceneState& state,
            const std::vector<hadisplay::Button>& buttons,
            RenderState& render_state,
            bool force_full_refresh = false) {
    const auto buffer = hadisplay::render_scene(state, buttons);

    FBInkConfig cfg{};
    cfg.is_quiet = true;
    cfg.is_flashing = should_do_full_refresh(render_state, force_full_refresh);

    if (fbink_print_raw_data(fbfd,
                             const_cast<unsigned char*>(buffer.data()),
                             state.width,
                             state.height,
                             buffer.size(),
                             0,
                             0,
                             &cfg) < 0) {
        std::cerr << "fbink_print_raw_data failed.\n";
        return false;
    }

    if (cfg.is_flashing) {
        render_state.partial_refreshes = 0;
        render_state.last_full_refresh = std::chrono::steady_clock::now();
    } else {
        ++render_state.partial_refreshes;
    }

    return true;
}

void clear_screen(int fbfd, bool full_refresh) {
    FBInkConfig cfg{};
    cfg.is_quiet = true;
    cfg.is_flashing = full_refresh;
    fbink_cls(fbfd, &cfg, nullptr, false);
}

bool apply_remote_update(int fbfd,
                         hadisplay::SceneState& scene_state,
                         std::vector<hadisplay::Button>& buttons,
                         RenderState& render_state,
                         const ha::Client& ha_client,
                         int entity_index,
                         const std::string& busy_status,
                         const std::function<ha::Result()>& request) {
    scene_state.status = busy_status;
    buttons = hadisplay::buttons_for(scene_state);
    if (!render(fbfd, scene_state, buttons, render_state)) {
        return false;
    }

    const ha::Result result = request();
    if (!result.ok) {
        scene_state.status = concise_ha_error(result.message);
        return true;
    }

    const std::string entity_id = scene_state.entities[static_cast<std::size_t>(entity_index)].entity_id;
    const ha::EntityState updated = ha_client.fetch_entity_state(entity_id);
    if (!updated.ok) {
        scene_state.status = concise_ha_error(updated.message);
        return true;
    }

    apply_entity_update(scene_state, entity_index, updated);
    scene_state.status = "ENTITY UPDATED";
    return true;
}

bool handle_button_action(int fbfd,
                          hadisplay::SceneState& scene_state,
                          std::vector<hadisplay::Button>& buttons,
                          const ha::Client& ha_client,
                          hadisplay::DeviceStatus& device_status,
                          hadisplay::ConfigStore& config_store,
                          hadisplay::AppConfig& config,
                          bool& config_dirty,
                          RenderState& render_state,
                          int button_index,
                          bool& needs_redraw,
                          bool& force_full_refresh) {
    if (button_index < 0 || button_index >= static_cast<int>(buttons.size())) {
        return false;
    }

    scene_state.selected_button = button_index;
    const hadisplay::Button action_button = buttons[static_cast<std::size_t>(button_index)];

    switch (action_button.id) {
        case hadisplay::ButtonId::BrightnessToggle: {
            hadisplay::SystemStatus system_status;
            if (!device_status.cycle_brightness(system_status)) {
                needs_redraw = true;
                return false;
            }

            apply_system_status(scene_state, system_status);
            needs_redraw = true;
            return false;
        }
        case hadisplay::ButtonId::DevModeToggle:
            scene_state.dev_mode = !scene_state.dev_mode;
            needs_redraw = true;
            return false;
        case hadisplay::ButtonId::SetupToggleLight:
            if (action_button.value >= 0 && action_button.value < static_cast<int>(scene_state.entities.size())) {
                scene_state.entities[static_cast<std::size_t>(action_button.value)].selected =
                    !scene_state.entities[static_cast<std::size_t>(action_button.value)].selected;
                config_dirty = true;
                scene_state.status = "SELECTION UPDATED";
                needs_redraw = true;
            }
            return false;
        case hadisplay::ButtonId::SetupPreviousPage:
            scene_state.setup_page = std::max(0, scene_state.setup_page - 1);
            needs_redraw = true;
            return false;
        case hadisplay::ButtonId::SetupNextPage:
            ++scene_state.setup_page;
            needs_redraw = true;
            return false;
        case hadisplay::ButtonId::SetupRefresh:
            refresh_entities(scene_state, ha_client, config_from_scene(scene_state, config));
            needs_redraw = true;
            return false;
        case hadisplay::ButtonId::SetupSave: {
            config = config_from_scene(scene_state, config);
            std::string error;
            if (!config_store.save(config, error)) {
                scene_state.status = "CONFIG SAVE FAILED";
            } else {
                config_dirty = false;
                scene_state.status = "CONFIG SAVED";
                if (!config.selected_entity_ids.empty()) {
                    scene_state.view_mode = hadisplay::ViewMode::Dashboard;
                    scene_state.dashboard_page = 0;
                }
            }
            needs_redraw = true;
            return false;
        }
        case hadisplay::ButtonId::DashboardToggleLight:
            if (action_button.value >= 0 && action_button.value < static_cast<int>(scene_state.entities.size())) {
                const hadisplay::EntityItem& entity = scene_state.entities[static_cast<std::size_t>(action_button.value)];
                const std::string busy_status = entity.kind == hadisplay::EntityKind::Climate ? "SETTING HEATING" : "TOGGLING DEVICE";
                if (!apply_remote_update(fbfd,
                                         scene_state,
                                         buttons,
                                         render_state,
                                         ha_client,
                                         action_button.value,
                                         busy_status,
                                         [&]() {
                                             if (entity.kind == hadisplay::EntityKind::Light) {
                                                 return ha_client.toggle_light(entity.entity_id);
                                             }
                                             if (entity.kind == hadisplay::EntityKind::Switch) {
                                                 return ha_client.toggle_switch(entity.entity_id);
                                             }
                                             return ha_client.set_climate_hvac_mode(entity.entity_id, entity.is_on ? "off" : "heat");
                                         })) {
                    return true;
                }
                needs_redraw = true;
            }
            return false;
        case hadisplay::ButtonId::DashboardOpenDetail:
            scene_state.detail_entity_index = action_button.value;
            scene_state.view_mode = hadisplay::ViewMode::Detail;
            scene_state.status = "DETAIL VIEW";
            needs_redraw = true;
            return false;
        case hadisplay::ButtonId::DashboardPreviousPage:
            scene_state.dashboard_page = std::max(0, scene_state.dashboard_page - 1);
            needs_redraw = true;
            return false;
        case hadisplay::ButtonId::DashboardNextPage:
            ++scene_state.dashboard_page;
            needs_redraw = true;
            return false;
        case hadisplay::ButtonId::DashboardConfigure:
            scene_state.view_mode = hadisplay::ViewMode::Setup;
            scene_state.status = "SETUP VIEW";
            needs_redraw = true;
            return false;
        case hadisplay::ButtonId::DashboardRefresh:
            refresh_entities(scene_state, ha_client, config_from_scene(scene_state, config));
            needs_redraw = true;
            return false;
        case hadisplay::ButtonId::DetailBack:
            scene_state.view_mode = hadisplay::ViewMode::Dashboard;
            scene_state.status = "DASHBOARD";
            needs_redraw = true;
            return false;
        case hadisplay::ButtonId::DetailToggleLight:
            if (scene_state.detail_entity_index >= 0 && scene_state.detail_entity_index < static_cast<int>(scene_state.entities.size())) {
                const int entity_index = scene_state.detail_entity_index;
                const hadisplay::EntityItem& entity = scene_state.entities[static_cast<std::size_t>(entity_index)];
                if (!apply_remote_update(fbfd,
                                         scene_state,
                                         buttons,
                                         render_state,
                                         ha_client,
                                         entity_index,
                                         entity.kind == hadisplay::EntityKind::Climate ? "SETTING HEATING" : "TOGGLING DEVICE",
                                         [&]() {
                                             if (entity.kind == hadisplay::EntityKind::Light) {
                                                 return ha_client.toggle_light(entity.entity_id);
                                             }
                                             if (entity.kind == hadisplay::EntityKind::Switch) {
                                                 return ha_client.toggle_switch(entity.entity_id);
                                             }
                                             return ha_client.set_climate_hvac_mode(entity.entity_id, entity.is_on ? "off" : "heat");
                                         })) {
                    return true;
                }
                needs_redraw = true;
            }
            return false;
        case hadisplay::ButtonId::DetailBrightnessDown:
        case hadisplay::ButtonId::DetailBrightnessUp:
        case hadisplay::ButtonId::DetailSetRed:
        case hadisplay::ButtonId::DetailSetGreen:
        case hadisplay::ButtonId::DetailSetBlue:
        case hadisplay::ButtonId::DetailSetDaylight:
        case hadisplay::ButtonId::DetailSetNeutral:
        case hadisplay::ButtonId::DetailSetWarm: {
            if (action_button.value < 0 || action_button.value >= static_cast<int>(scene_state.entities.size())) {
                return false;
            }

            const int entity_index = action_button.value;
            const hadisplay::EntityItem& entity = scene_state.entities[static_cast<std::size_t>(entity_index)];
            if (entity.kind != hadisplay::EntityKind::Light) {
                return false;
            }
            const std::string entity_id = entity.entity_id;

            auto clamp_kelvin = [&](int value) {
                int min_kelvin = entity.min_color_temp_kelvin > 0 ? entity.min_color_temp_kelvin : 2200;
                int max_kelvin = entity.max_color_temp_kelvin > 0 ? entity.max_color_temp_kelvin : 6500;
                if (min_kelvin > max_kelvin) {
                    std::swap(min_kelvin, max_kelvin);
                }
                return std::clamp(value, min_kelvin, max_kelvin);
            };

            ha::Result result;
            if (action_button.id == hadisplay::ButtonId::DetailBrightnessDown ||
                action_button.id == hadisplay::ButtonId::DetailBrightnessUp) {
                const int delta = action_button.id == hadisplay::ButtonId::DetailBrightnessUp ? 10 : -10;
                result = ha_client.set_light_brightness(entity_id, std::clamp(entity.brightness_percent + delta, 0, 100));
            } else if (action_button.id == hadisplay::ButtonId::DetailSetRed) {
                result = ha_client.set_light_rgb(entity_id, 255, 0, 0);
            } else if (action_button.id == hadisplay::ButtonId::DetailSetGreen) {
                result = ha_client.set_light_rgb(entity_id, 0, 255, 0);
            } else if (action_button.id == hadisplay::ButtonId::DetailSetBlue) {
                result = ha_client.set_light_rgb(entity_id, 0, 0, 255);
            } else if (action_button.id == hadisplay::ButtonId::DetailSetDaylight) {
                const int daylight = clamp_kelvin(entity.max_color_temp_kelvin > 0 ? entity.max_color_temp_kelvin : 6500);
                result = ha_client.set_light_color_temperature(entity_id, daylight);
            } else if (action_button.id == hadisplay::ButtonId::DetailSetNeutral) {
                const int min_kelvin = entity.min_color_temp_kelvin > 0 ? entity.min_color_temp_kelvin : 2200;
                const int max_kelvin = entity.max_color_temp_kelvin > 0 ? entity.max_color_temp_kelvin : 6500;
                result = ha_client.set_light_color_temperature(entity_id, clamp_kelvin((min_kelvin + max_kelvin) / 2));
            } else {
                const int warm = clamp_kelvin(2800);
                result = ha_client.set_light_color_temperature(entity_id, warm);
            }

            if (!result.ok) {
                scene_state.status = concise_ha_error(result.message);
                needs_redraw = true;
                return false;
            }

            const ha::EntityState updated = ha_client.fetch_entity_state(entity_id);
            if (!updated.ok) {
                scene_state.status = concise_ha_error(updated.message);
                needs_redraw = true;
                return false;
            }

            apply_entity_update(scene_state, entity_index, updated);
            scene_state.status = "ENTITY UPDATED";
            needs_redraw = true;
            return false;
        }
        case hadisplay::ButtonId::Exit:
            scene_state.status = "EXITING TO NICKEL";
            force_full_refresh = true;
            needs_redraw = true;
            return true;
    }

    return false;
}

}  // namespace

int main() {
    FBInkConfig fbink_cfg{};
    fbink_cfg.is_quiet = true;

    const int fbfd = fbink_open();
    if (fbfd < 0) {
        std::cerr << "fbink_open failed.\n";
        return EXIT_FAILURE;
    }

    if (fbink_init(fbfd, &fbink_cfg) < 0) {
        std::cerr << "fbink_init failed.\n";
        fbink_close(fbfd);
        return EXIT_FAILURE;
    }

    FBInkState fb_state{};
    fbink_get_state(&fbink_cfg, &fb_state);

    hadisplay::SceneState scene_state{};
    scene_state.width = static_cast<int>(fb_state.view_width);
    scene_state.height = static_cast<int>(fb_state.view_height);
    scene_state.status = "STARTING";
    update_clock_status(scene_state);

    hadisplay::DeviceStatus device_status;
    apply_system_status(scene_state, device_status.snapshot());

    hadisplay::ConfigStore config_store;
    const hadisplay::ConfigLoadResult loaded_config = config_store.load();
    hadisplay::AppConfig config = loaded_config.config;
    bool config_dirty = false;
    if (!loaded_config.ok) {
        scene_state.status = "CONFIG RESET";
    }

    ha::Client ha_client({
        .base_url = config.ha_url,
        .token = config.ha_token,
        .weather_entity_id = config.ha_weather_entity,
    });
    apply_weather_state(scene_state, ha_client.fetch_weather_state());

    if (ha_client.configured()) {
        refresh_entities(scene_state, ha_client, config);
        if (!config.selected_entity_ids.empty()) {
            scene_state.view_mode = hadisplay::ViewMode::Dashboard;
        } else {
            scene_state.view_mode = hadisplay::ViewMode::Setup;
            if (scene_state.status == "DEVICES SYNCED") {
                scene_state.status = loaded_config.found ? "CONFIG EMPTY" : "SELECT LIGHTS";
            }
        }
    } else {
        scene_state.status = "CHECK CONFIG";
        scene_state.view_mode = hadisplay::ViewMode::Setup;
    }

    std::vector<hadisplay::Button> buttons = hadisplay::buttons_for(scene_state);

    const int touchfd = open(kTouchDevice, O_RDONLY | O_NONBLOCK);
    if (touchfd < 0) {
        std::cerr << "Failed to open " << kTouchDevice << ": " << std::strerror(errno) << "\n";
        fbink_close(fbfd);
        return EXIT_FAILURE;
    }

    if (ioctl(touchfd, EVIOCGRAB, 1) < 0) {
        std::cerr << "EVIOCGRAB warning: " << std::strerror(errno) << "\n";
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    clear_screen(fbfd, true);

    RenderState render_state{};
    if (!render(fbfd, scene_state, buttons, render_state, true)) {
        ioctl(touchfd, EVIOCGRAB, 0);
        close(touchfd);
        fbink_close(fbfd);
        return EXIT_FAILURE;
    }

    std::cerr << "hadisplay running. Screen: " << scene_state.width << "x"
              << scene_state.height << ". Touch grabbed.\n";

    TouchState touch{};
    bool needs_redraw = false;
    bool force_full_refresh = false;
    int pending_button = -1;
    auto now = std::chrono::steady_clock::now();
    auto next_clock_refresh = next_minute_deadline();
    auto next_light_refresh = now + kNormalLightPollInterval;
    auto next_device_refresh = now + kNormalDevicePollInterval;
    auto next_weather_refresh = now + kNormalWeatherPollInterval;

    struct pollfd pfd{};
    pfd.fd = touchfd;
    pfd.events = POLLIN;

    while (g_running) {
        now = std::chrono::steady_clock::now();
        const int ret = poll(&pfd,
                             1,
                             poll_timeout_until(now,
                                                render_state.last_full_refresh + kFullRefreshInterval,
                                                next_clock_refresh,
                                                next_light_refresh,
                                                next_device_refresh,
                                                next_weather_refresh));
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "poll error: " << std::strerror(errno) << "\n";
            break;
        }

        if (ret == 0) {
            if (should_do_full_refresh(render_state, false)) {
                force_full_refresh = true;
                needs_redraw = true;
            }
        } else {
            struct input_event ev{};
            while (read(touchfd, &ev, sizeof(ev)) == static_cast<ssize_t>(sizeof(ev))) {
                if (ev.type == EV_ABS) {
                    if (ev.code == ABS_MT_POSITION_X) {
                        touch.x = ev.value;
                    } else if (ev.code == ABS_MT_POSITION_Y) {
                        touch.y = ev.value;
                    }
                } else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
                    if (ev.value == 1) {
                        touch.touching = true;
                    } else if (ev.value == 0 && touch.touching) {
                        touch.touching = false;
                        if (touch.x >= 0 && touch.y >= 0) {
                            int sx = 0;
                            int sy = 0;
                            map_touch_to_scene(touch.x, touch.y, scene_state.width, scene_state.height, sx, sy);
                            const int released_on = hadisplay::button_at(buttons, sx, sy);
                            if (scene_state.pressed_button >= 0 && scene_state.pressed_button == released_on) {
                                pending_button = released_on;
                            }
                        }
                        scene_state.pressed_button = -1;
                        needs_redraw = true;
                        touch.x = -1;
                        touch.y = -1;
                    }
                } else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
                    if (touch.touching && touch.x >= 0 && touch.y >= 0) {
                        int sx = 0;
                        int sy = 0;
                        map_touch_to_scene(touch.x, touch.y, scene_state.width, scene_state.height, sx, sy);
                        const int pressed = hadisplay::button_at(buttons, sx, sy);
                        if (pressed != scene_state.pressed_button) {
                            scene_state.pressed_button = pressed;
                            needs_redraw = true;
                        }
                    }
                }
            }
        }

        now = std::chrono::steady_clock::now();
        const auto fast_or_light = scene_state.dev_mode ? kDevPollInterval : kNormalLightPollInterval;
        const auto fast_or_device = scene_state.dev_mode ? kDevPollInterval : kNormalDevicePollInterval;
        const auto fast_or_weather = scene_state.dev_mode ? kDevPollInterval : kNormalWeatherPollInterval;
        if (now >= next_clock_refresh) {
            if (update_clock_status(scene_state)) {
                needs_redraw = true;
            }
            next_clock_refresh = scene_state.dev_mode ? now + kDevPollInterval : next_minute_deadline();
        }
        if (now >= next_light_refresh) {
            if (refresh_entities(scene_state, ha_client, config_from_scene(scene_state, config))) {
                needs_redraw = true;
            }
            next_light_refresh = now + fast_or_light;
        }
        if (now >= next_device_refresh) {
            if (update_system_status(scene_state, device_status.snapshot())) {
                needs_redraw = true;
            }
            next_device_refresh = now + fast_or_device;
        }
        if (now >= next_weather_refresh) {
            if (update_weather_state(scene_state, ha_client.fetch_weather_state())) {
                needs_redraw = true;
            }
            next_weather_refresh = now + fast_or_weather;
        }

        if (needs_redraw) {
            buttons = hadisplay::buttons_for(scene_state);
            if (!render(fbfd, scene_state, buttons, render_state, force_full_refresh)) {
                break;
            }
            needs_redraw = false;
            force_full_refresh = false;
        }

        if (pending_button >= 0) {
            buttons = hadisplay::buttons_for(scene_state);
            const bool should_exit = handle_button_action(fbfd,
                                                          scene_state,
                                                          buttons,
                                                          ha_client,
                                                          device_status,
                                                          config_store,
                                                          config,
                                                          config_dirty,
                                                          render_state,
                                                          pending_button,
                                                          needs_redraw,
                                                          force_full_refresh);
            pending_button = -1;
            now = std::chrono::steady_clock::now();
            next_clock_refresh = scene_state.dev_mode ? now + kDevPollInterval : next_minute_deadline();
            next_light_refresh = now + (scene_state.dev_mode ? kDevPollInterval : kNormalLightPollInterval);
            next_device_refresh = now + (scene_state.dev_mode ? kDevPollInterval : kNormalDevicePollInterval);
            next_weather_refresh = now + (scene_state.dev_mode ? kDevPollInterval : kNormalWeatherPollInterval);

            if (needs_redraw) {
                buttons = hadisplay::buttons_for(scene_state);
                if (!render(fbfd, scene_state, buttons, render_state, force_full_refresh)) {
                    break;
                }
                needs_redraw = false;
                force_full_refresh = false;
            }

            if (should_exit) {
                break;
            }
        }
    }

    if (config_dirty) {
        std::string error;
        if (!config_store.save(config_from_scene(scene_state, config), error)) {
            std::cerr << "Failed to save config on exit: " << error << "\n";
        }
    }

    clear_screen(fbfd, true);
    ioctl(touchfd, EVIOCGRAB, 0);
    close(touchfd);
    fbink_close(fbfd);
    std::cerr << "hadisplay exiting.\n";
    return EXIT_SUCCESS;
}
