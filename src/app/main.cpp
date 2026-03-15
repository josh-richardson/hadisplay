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
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <poll.h>
#include <set>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
#include <variant>
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

struct DisplaySettings {
    hadisplay::DisplayMode requested_mode = hadisplay::DisplayMode::Auto;
    hadisplay::DisplayMode effective_mode = hadisplay::DisplayMode::Grayscale;
    hadisplay::PixelFormat pixel_format = hadisplay::PixelFormat::Gray8;
    bool has_color_panel = false;
};

struct ClockStatus {
    std::string time_label;
    std::string date_label;
};

struct EntityRefreshCompletion {
    std::uint64_t epoch = 0;
    ha::EntityListResult list_result;
    std::string success_status = "DEVICES SYNCED";
    std::string empty_selection_status = "DEVICES SYNCED";
};

struct WeatherRefreshCompletion {
    std::uint64_t epoch = 0;
    ha::WeatherState weather;
};

struct SensorHistoryCompletion {
    std::uint64_t epoch = 0;
    ha::SensorHistoryResult history;
};

enum class EntityActionType {
    ToggleLight = 0,
    ToggleSwitch,
    SetClimateMode,
    SetBrightness,
    SetColorTemperature,
    SetRgb,
};

struct EntityActionRequest {
    EntityActionType type = EntityActionType::ToggleLight;
    std::string entity_id;
    int value1 = 0;
    int value2 = 0;
    int value3 = 0;
    std::string text;
    std::string success_status = "ENTITY UPDATED";
};

struct EntityActionCompletion {
    std::uint64_t epoch = 0;
    std::string entity_id;
    ha::Result request_result;
    ha::EntityState updated_state;
    std::string success_status = "ENTITY UPDATED";
};

using AsyncCompletion = std::variant<EntityRefreshCompletion, WeatherRefreshCompletion, EntityActionCompletion, SensorHistoryCompletion>;

struct AsyncMailbox {
    std::mutex mutex;
    std::vector<AsyncCompletion> completions;
    int wake_read_fd = -1;
    int wake_write_fd = -1;
};

struct AsyncState {
    std::shared_ptr<AsyncMailbox> mailbox = std::make_shared<AsyncMailbox>();
    bool entity_refresh_pending = false;
    bool weather_refresh_pending = false;
    bool entity_action_pending = false;
    bool sensor_history_pending = false;
    std::uint64_t next_entity_epoch = 0;
    std::uint64_t latest_entity_applied_epoch = 0;
    std::uint64_t next_weather_epoch = 0;
    std::uint64_t latest_weather_applied_epoch = 0;
    std::uint64_t next_history_epoch = 0;
    std::uint64_t pending_history_epoch = 0;
    std::uint64_t latest_history_applied_epoch = 0;
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

void post_async_completion(const std::shared_ptr<AsyncMailbox>& mailbox, AsyncCompletion completion) {
    std::lock_guard<std::mutex> lock(mailbox->mutex);
    mailbox->completions.push_back(std::move(completion));
    if (mailbox->wake_write_fd >= 0) {
        const std::uint8_t signal = 1U;
        const ssize_t written = write(mailbox->wake_write_fd, &signal, sizeof(signal));
        (void)written;
    }
}

std::vector<AsyncCompletion> take_async_completions(AsyncState& async_state) {
    std::lock_guard<std::mutex> lock(async_state.mailbox->mutex);
    std::vector<AsyncCompletion> completions;
    completions.swap(async_state.mailbox->completions);
    return completions;
}

void drain_async_wake_fd(const AsyncMailbox& mailbox) {
    if (mailbox.wake_read_fd < 0) {
        return;
    }

    std::array<std::uint8_t, 64> buffer{};
    while (read(mailbox.wake_read_fd, buffer.data(), buffer.size()) > 0) {
    }
}

bool configure_async_mailbox(AsyncState& async_state) {
    int pipe_fds[2]{-1, -1};
    if (pipe(pipe_fds) < 0) {
        std::cerr << "Failed to create async wake pipe: " << std::strerror(errno) << "\n";
        return false;
    }

    for (int fd : pipe_fds) {
        const int current_flags = fcntl(fd, F_GETFL, 0);
        if (current_flags < 0 || fcntl(fd, F_SETFL, current_flags | O_NONBLOCK) < 0) {
            std::cerr << "Failed to set async wake pipe nonblocking: " << std::strerror(errno) << "\n";
            close(pipe_fds[0]);
            close(pipe_fds[1]);
            return false;
        }
        const int current_fd_flags = fcntl(fd, F_GETFD, 0);
        if (current_fd_flags < 0 || fcntl(fd, F_SETFD, current_fd_flags | FD_CLOEXEC) < 0) {
            std::cerr << "Failed to set async wake pipe cloexec: " << std::strerror(errno) << "\n";
            close(pipe_fds[0]);
            close(pipe_fds[1]);
            return false;
        }
    }

    async_state.mailbox->wake_read_fd = pipe_fds[0];
    async_state.mailbox->wake_write_fd = pipe_fds[1];
    return true;
}

void close_async_mailbox(AsyncState& async_state) {
    if (async_state.mailbox->wake_read_fd >= 0) {
        close(async_state.mailbox->wake_read_fd);
        async_state.mailbox->wake_read_fd = -1;
    }
    if (async_state.mailbox->wake_write_fd >= 0) {
        close(async_state.mailbox->wake_write_fd);
        async_state.mailbox->wake_write_fd = -1;
    }
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

std::string concise_history_error(const std::string& message) {
    const std::string lower = [] (std::string value) {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return value;
    }(message);

    if (lower.find("no history") != std::string::npos || lower.find("no numeric") != std::string::npos) {
        return "NO HISTORY";
    }
    return concise_ha_error(message);
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
        case ha::EntityKind::Sensor: return hadisplay::EntityKind::Sensor;
    }
    return hadisplay::EntityKind::Light;
}

std::string entity_kind_label(hadisplay::EntityKind kind) {
    switch (kind) {
        case hadisplay::EntityKind::Light: return "LIGHT";
        case hadisplay::EntityKind::Switch: return "SOCKET";
        case hadisplay::EntityKind::Climate: return "THERMOSTAT";
        case hadisplay::EntityKind::Sensor: return "SENSOR";
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
    item.supports_history = entity.kind == ha::EntityKind::Sensor;
    item.has_numeric_value = entity.has_numeric_value;
    item.numeric_value = entity.numeric_value;
    item.device_class = entity.device_class;
    item.unit_label = entity.unit_of_measurement;
    item.hvac_action = entity.hvac_action;
    return item;
}

void clear_detail_history(hadisplay::SceneState& scene_state) {
    scene_state.detail_history_entity_id.clear();
    scene_state.detail_history_loading = false;
    scene_state.detail_history_available = false;
    scene_state.detail_history_values.clear();
    scene_state.detail_history_min = 0.0;
    scene_state.detail_history_max = 0.0;
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

hadisplay::AppConfig selection_config_for_sync(const hadisplay::SceneState& scene_state, const hadisplay::AppConfig& base_config) {
    if (scene_state.entities.empty()) {
        return base_config;
    }
    return config_from_scene(scene_state, base_config);
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

    if (scene_state.detail_entity_index < 0 ||
        scene_state.entities[static_cast<std::size_t>(scene_state.detail_entity_index)].kind != hadisplay::EntityKind::Sensor ||
        scene_state.entities[static_cast<std::size_t>(scene_state.detail_entity_index)].entity_id != scene_state.detail_history_entity_id) {
        clear_detail_history(scene_state);
    }
}

bool apply_entity_list_result(hadisplay::SceneState& scene_state,
                              const ha::EntityListResult& list_result,
                              const hadisplay::AppConfig& config) {
    if (!list_result.ok) {
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
    return true;
}

bool has_selected_entities(const hadisplay::AppConfig& config) {
    return !config.selected_entity_ids.empty();
}

void schedule_entity_refresh(AsyncState& async_state,
                             const ha::Client& ha_client,
                             const std::string& success_status,
                             const std::string& empty_selection_status) {
    if (async_state.entity_refresh_pending) {
        return;
    }

    async_state.entity_refresh_pending = true;
    const std::uint64_t epoch = ++async_state.next_entity_epoch;
    const std::shared_ptr<AsyncMailbox> mailbox = async_state.mailbox;
    std::thread([mailbox, ha_client, epoch, success_status, empty_selection_status]() mutable {
        post_async_completion(mailbox,
                              EntityRefreshCompletion{
                                  .epoch = epoch,
                                  .list_result = ha_client.list_entities(),
                                  .success_status = success_status,
                                  .empty_selection_status = empty_selection_status,
                              });
    }).detach();
}

void schedule_weather_refresh(AsyncState& async_state, const ha::Client& ha_client) {
    if (async_state.weather_refresh_pending) {
        return;
    }

    async_state.weather_refresh_pending = true;
    const std::uint64_t epoch = ++async_state.next_weather_epoch;
    const std::shared_ptr<AsyncMailbox> mailbox = async_state.mailbox;
    std::thread([mailbox, ha_client, epoch]() mutable {
        post_async_completion(mailbox,
                              WeatherRefreshCompletion{
                                  .epoch = epoch,
                                  .weather = ha_client.fetch_weather_state(),
                              });
    }).detach();
}

void schedule_sensor_history(AsyncState& async_state, const ha::Client& ha_client, const std::string& entity_id) {
    const std::uint64_t epoch = ++async_state.next_history_epoch;
    async_state.sensor_history_pending = true;
    async_state.pending_history_epoch = epoch;
    const std::shared_ptr<AsyncMailbox> mailbox = async_state.mailbox;
    std::thread([mailbox, ha_client, epoch, entity_id]() mutable {
        post_async_completion(mailbox,
                              SensorHistoryCompletion{
                                  .epoch = epoch,
                                  .history = ha_client.fetch_sensor_history(entity_id),
                              });
    }).detach();
}

bool schedule_entity_action(AsyncState& async_state, const ha::Client& ha_client, EntityActionRequest request) {
    if (async_state.entity_action_pending) {
        return false;
    }

    async_state.entity_action_pending = true;
    const std::uint64_t epoch = ++async_state.next_entity_epoch;
    const std::shared_ptr<AsyncMailbox> mailbox = async_state.mailbox;
    std::thread([mailbox, ha_client, epoch, request = std::move(request)]() mutable {
        ha::Result request_result;
        switch (request.type) {
            case EntityActionType::ToggleLight:
                request_result = ha_client.toggle_light(request.entity_id);
                break;
            case EntityActionType::ToggleSwitch:
                request_result = ha_client.toggle_switch(request.entity_id);
                break;
            case EntityActionType::SetClimateMode:
                request_result = ha_client.set_climate_hvac_mode(request.entity_id, request.text);
                break;
            case EntityActionType::SetBrightness:
                request_result = ha_client.set_light_brightness(request.entity_id, request.value1);
                break;
            case EntityActionType::SetColorTemperature:
                request_result = ha_client.set_light_color_temperature(request.entity_id, request.value1);
                break;
            case EntityActionType::SetRgb:
                request_result = ha_client.set_light_rgb(request.entity_id, request.value1, request.value2, request.value3);
                break;
        }

        ha::EntityState updated_state;
        if (request_result.ok) {
            updated_state = ha_client.fetch_entity_state(request.entity_id);
        } else {
            updated_state.entity_id = request.entity_id;
        }

        post_async_completion(mailbox,
                              EntityActionCompletion{
                                  .epoch = epoch,
                                  .entity_id = request.entity_id,
                                  .request_result = std::move(request_result),
                                  .updated_state = std::move(updated_state),
                                  .success_status = request.success_status,
                              });
    }).detach();
    return true;
}

bool process_async_completions(AsyncState& async_state,
                               hadisplay::SceneState& scene_state,
                               const hadisplay::AppConfig& config) {
    bool needs_redraw = false;
    for (AsyncCompletion& completion : take_async_completions(async_state)) {
        if (std::holds_alternative<EntityRefreshCompletion>(completion)) {
            async_state.entity_refresh_pending = false;
            EntityRefreshCompletion refresh = std::move(std::get<EntityRefreshCompletion>(completion));
            if (refresh.epoch < async_state.latest_entity_applied_epoch) {
                continue;
            }
            async_state.latest_entity_applied_epoch = refresh.epoch;
            if (!apply_entity_list_result(scene_state, refresh.list_result, selection_config_for_sync(scene_state, config))) {
                scene_state.status = concise_ha_error(refresh.list_result.message);
                needs_redraw = true;
                continue;
            }
            scene_state.status = has_selected_entities(selection_config_for_sync(scene_state, config))
                ? refresh.success_status
                : refresh.empty_selection_status;
            needs_redraw = true;
            continue;
        }

        if (std::holds_alternative<WeatherRefreshCompletion>(completion)) {
            async_state.weather_refresh_pending = false;
            WeatherRefreshCompletion weather = std::move(std::get<WeatherRefreshCompletion>(completion));
            if (weather.epoch < async_state.latest_weather_applied_epoch) {
                continue;
            }
            async_state.latest_weather_applied_epoch = weather.epoch;
            if (update_weather_state(scene_state, weather.weather)) {
                needs_redraw = true;
            }
            continue;
        }

        if (std::holds_alternative<SensorHistoryCompletion>(completion)) {
            SensorHistoryCompletion history = std::move(std::get<SensorHistoryCompletion>(completion));
            if (history.epoch < async_state.latest_history_applied_epoch) {
                continue;
            }
            async_state.latest_history_applied_epoch = history.epoch;
            if (history.epoch == async_state.pending_history_epoch) {
                async_state.sensor_history_pending = false;
            }
            if (history.history.entity_id != scene_state.detail_history_entity_id) {
                continue;
            }

            scene_state.detail_history_loading = false;
            if (!history.history.ok) {
                scene_state.detail_history_available = false;
                scene_state.detail_history_values.clear();
                scene_state.status = concise_history_error(history.history.message);
                needs_redraw = true;
                continue;
            }

            scene_state.detail_history_available = true;
            scene_state.detail_history_values = std::move(history.history.values);
            scene_state.detail_history_min = history.history.min_value;
            scene_state.detail_history_max = history.history.max_value;
            scene_state.status = "DETAIL VIEW";
            needs_redraw = true;
            continue;
        }

        async_state.entity_action_pending = false;
        EntityActionCompletion action = std::move(std::get<EntityActionCompletion>(completion));
        if (action.epoch < async_state.latest_entity_applied_epoch) {
            continue;
        }
        async_state.latest_entity_applied_epoch = action.epoch;
        if (!action.request_result.ok) {
            scene_state.status = concise_ha_error(action.request_result.message);
            needs_redraw = true;
            continue;
        }
        if (!action.updated_state.ok) {
            scene_state.status = concise_ha_error(action.updated_state.message);
            needs_redraw = true;
            continue;
        }

        const std::optional<int> entity_index = find_entity_index(scene_state, action.entity_id);
        if (entity_index.has_value()) {
            apply_entity_update(scene_state, *entity_index, action.updated_state);
        }
        scene_state.status = action.success_status;
        needs_redraw = true;
    }
    return needs_redraw;
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

DisplaySettings resolve_display_settings(const FBInkState& fb_state, hadisplay::DisplayMode requested_mode) {
    const bool has_color_panel = fb_state.has_color_panel;
    const hadisplay::DisplayMode effective_mode = [&]() {
        switch (requested_mode) {
            case hadisplay::DisplayMode::Color:
                return has_color_panel ? hadisplay::DisplayMode::Color : hadisplay::DisplayMode::Grayscale;
            case hadisplay::DisplayMode::Grayscale:
                return hadisplay::DisplayMode::Grayscale;
            case hadisplay::DisplayMode::Auto:
                return has_color_panel ? hadisplay::DisplayMode::Color : hadisplay::DisplayMode::Grayscale;
        }
        return hadisplay::DisplayMode::Grayscale;
    }();

    return {
        .requested_mode = requested_mode,
        .effective_mode = effective_mode,
        .pixel_format = effective_mode == hadisplay::DisplayMode::Color ? hadisplay::PixelFormat::RGBA32
                                                                        : hadisplay::PixelFormat::Gray8,
        .has_color_panel = has_color_panel,
    };
}

bool render(int fbfd,
            const hadisplay::SceneState& state,
            const std::vector<hadisplay::Button>& buttons,
            const DisplaySettings& display_settings,
            RenderState& render_state,
            bool force_full_refresh = false) {
    const auto buffer = hadisplay::render_scene(state, buttons, display_settings.pixel_format);

    FBInkConfig cfg{};
    cfg.is_quiet = true;
    if (display_settings.effective_mode == hadisplay::DisplayMode::Color) {
        cfg.is_flashing = should_do_full_refresh(render_state, force_full_refresh);
        cfg.wfm_mode = cfg.is_flashing ? WFM_GCC16 : WFM_GLRC16;
    } else {
        cfg.is_flashing = should_do_full_refresh(render_state, force_full_refresh);
    }

    if (fbink_print_raw_data(fbfd,
                             const_cast<unsigned char*>(buffer.pixels.data()),
                             state.width,
                             state.height,
                             buffer.pixels.size(),
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

void clear_screen(int fbfd, bool full_refresh, const DisplaySettings& display_settings) {
    FBInkConfig cfg{};
    cfg.is_quiet = true;
    cfg.is_flashing = full_refresh;
    if (display_settings.effective_mode == hadisplay::DisplayMode::Color) {
        cfg.wfm_mode = WFM_GCC16;
    }
    fbink_cls(fbfd, &cfg, nullptr, false);
}

bool handle_button_action(hadisplay::SceneState& scene_state,
                          std::vector<hadisplay::Button>& buttons,
                          const ha::Client& ha_client,
                          hadisplay::DeviceStatus& device_status,
                          hadisplay::ConfigStore& config_store,
                          hadisplay::AppConfig& config,
                          AsyncState& async_state,
                          bool& config_dirty,
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
            if (!ha_client.configured()) {
                scene_state.status = "CHECK CONFIG";
            } else {
                schedule_entity_refresh(async_state, ha_client, "DEVICES SYNCED", "DEVICES SYNCED");
                scene_state.status = "SYNCING DEVICES";
            }
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
                if (!ha_client.configured()) {
                    scene_state.status = "CHECK CONFIG";
                    needs_redraw = true;
                    return false;
                }
                EntityActionRequest request{
                    .entity_id = entity.entity_id,
                    .text = {},
                };
                if (entity.kind == hadisplay::EntityKind::Light) {
                    request.type = EntityActionType::ToggleLight;
                } else if (entity.kind == hadisplay::EntityKind::Switch) {
                    request.type = EntityActionType::ToggleSwitch;
                } else {
                    request.type = EntityActionType::SetClimateMode;
                    request.text = entity.is_on ? "off" : "heat";
                }
                if (schedule_entity_action(async_state, ha_client, std::move(request))) {
                    scene_state.status = busy_status;
                } else {
                    scene_state.status = "HA REQUEST ACTIVE";
                }
                needs_redraw = true;
            }
            return false;
        case hadisplay::ButtonId::DashboardOpenDetail:
            scene_state.detail_entity_index = action_button.value;
            scene_state.view_mode = hadisplay::ViewMode::Detail;
            clear_detail_history(scene_state);
            if (action_button.value >= 0 && action_button.value < static_cast<int>(scene_state.entities.size())) {
                const hadisplay::EntityItem& entity = scene_state.entities[static_cast<std::size_t>(action_button.value)];
                if (entity.kind == hadisplay::EntityKind::Sensor && entity.supports_history && ha_client.configured()) {
                    scene_state.detail_history_entity_id = entity.entity_id;
                    scene_state.detail_history_loading = true;
                    scene_state.status = "LOADING HISTORY";
                    schedule_sensor_history(async_state, ha_client, entity.entity_id);
                } else {
                    scene_state.status = "DETAIL VIEW";
                }
            } else {
                scene_state.status = "DETAIL VIEW";
            }
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
            clear_detail_history(scene_state);
            scene_state.status = "SETUP VIEW";
            needs_redraw = true;
            return false;
        case hadisplay::ButtonId::DashboardRefresh:
            if (!ha_client.configured()) {
                scene_state.status = "CHECK CONFIG";
            } else {
                schedule_entity_refresh(async_state, ha_client, "DEVICES SYNCED", "DEVICES SYNCED");
                scene_state.status = "SYNCING DEVICES";
            }
            needs_redraw = true;
            return false;
        case hadisplay::ButtonId::DetailBack:
            scene_state.view_mode = hadisplay::ViewMode::Dashboard;
            clear_detail_history(scene_state);
            scene_state.status = "DASHBOARD";
            needs_redraw = true;
            return false;
        case hadisplay::ButtonId::DetailToggleLight:
            if (scene_state.detail_entity_index >= 0 && scene_state.detail_entity_index < static_cast<int>(scene_state.entities.size())) {
                const int entity_index = scene_state.detail_entity_index;
                const hadisplay::EntityItem& entity = scene_state.entities[static_cast<std::size_t>(entity_index)];
                if (!ha_client.configured()) {
                    scene_state.status = "CHECK CONFIG";
                    needs_redraw = true;
                    return false;
                }
                const std::string busy_status = entity.kind == hadisplay::EntityKind::Climate ? "SETTING HEATING" : "TOGGLING DEVICE";
                EntityActionRequest request{
                    .entity_id = entity.entity_id,
                    .text = {},
                };
                if (entity.kind == hadisplay::EntityKind::Light) {
                    request.type = EntityActionType::ToggleLight;
                } else if (entity.kind == hadisplay::EntityKind::Switch) {
                    request.type = EntityActionType::ToggleSwitch;
                } else {
                    request.type = EntityActionType::SetClimateMode;
                    request.text = entity.is_on ? "off" : "heat";
                }
                if (schedule_entity_action(async_state, ha_client, std::move(request))) {
                    scene_state.status = busy_status;
                } else {
                    scene_state.status = "HA REQUEST ACTIVE";
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
            if (!ha_client.configured()) {
                scene_state.status = "CHECK CONFIG";
                needs_redraw = true;
                return false;
            }

            auto clamp_kelvin = [&](int value) {
                int min_kelvin = entity.min_color_temp_kelvin > 0 ? entity.min_color_temp_kelvin : 2200;
                int max_kelvin = entity.max_color_temp_kelvin > 0 ? entity.max_color_temp_kelvin : 6500;
                if (min_kelvin > max_kelvin) {
                    std::swap(min_kelvin, max_kelvin);
                }
                return std::clamp(value, min_kelvin, max_kelvin);
            };

            EntityActionRequest request{
                .entity_id = entity.entity_id,
                .text = {},
            };
            if (action_button.id == hadisplay::ButtonId::DetailBrightnessDown ||
                action_button.id == hadisplay::ButtonId::DetailBrightnessUp) {
                const int delta = action_button.id == hadisplay::ButtonId::DetailBrightnessUp ? 10 : -10;
                request.type = EntityActionType::SetBrightness;
                request.value1 = std::clamp(entity.brightness_percent + delta, 0, 100);
            } else if (action_button.id == hadisplay::ButtonId::DetailSetRed) {
                request.type = EntityActionType::SetRgb;
                request.value1 = 255;
            } else if (action_button.id == hadisplay::ButtonId::DetailSetGreen) {
                request.type = EntityActionType::SetRgb;
                request.value2 = 255;
            } else if (action_button.id == hadisplay::ButtonId::DetailSetBlue) {
                request.type = EntityActionType::SetRgb;
                request.value3 = 255;
            } else if (action_button.id == hadisplay::ButtonId::DetailSetDaylight) {
                request.type = EntityActionType::SetColorTemperature;
                request.value1 = clamp_kelvin(entity.max_color_temp_kelvin > 0 ? entity.max_color_temp_kelvin : 6500);
            } else if (action_button.id == hadisplay::ButtonId::DetailSetNeutral) {
                const int min_kelvin = entity.min_color_temp_kelvin > 0 ? entity.min_color_temp_kelvin : 2200;
                const int max_kelvin = entity.max_color_temp_kelvin > 0 ? entity.max_color_temp_kelvin : 6500;
                request.type = EntityActionType::SetColorTemperature;
                request.value1 = clamp_kelvin((min_kelvin + max_kelvin) / 2);
            } else {
                request.type = EntityActionType::SetColorTemperature;
                request.value1 = clamp_kelvin(2800);
            }

            if (schedule_entity_action(async_state, ha_client, std::move(request))) {
                scene_state.status = "UPDATING LIGHT";
            } else {
                scene_state.status = "HA REQUEST ACTIVE";
            }
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
    const DisplaySettings display_settings = resolve_display_settings(fb_state, config.display_mode);
    bool config_dirty = false;
    if (!loaded_config.ok) {
        scene_state.status = "CONFIG RESET";
    } else if (config.display_mode == hadisplay::DisplayMode::Color && !display_settings.has_color_panel) {
        std::cerr << "Color mode requested but FBInk reports no color panel; falling back to grayscale.\n";
    }

    ha::Client ha_client({
        .base_url = config.ha_url,
        .token = config.ha_token,
        .weather_entity_id = config.ha_weather_entity,
    });
    AsyncState async_state;
    configure_async_mailbox(async_state);

    if (ha_client.configured()) {
        if (has_selected_entities(config)) {
            scene_state.view_mode = hadisplay::ViewMode::Dashboard;
        } else {
            scene_state.view_mode = hadisplay::ViewMode::Setup;
        }
        if (scene_state.status == "STARTING") {
            scene_state.status = "SYNCING DEVICES";
        }
        schedule_entity_refresh(async_state,
                                ha_client,
                                "DEVICES SYNCED",
                                loaded_config.found ? "CONFIG EMPTY" : "SELECT LIGHTS");
        schedule_weather_refresh(async_state, ha_client);
    } else {
        scene_state.status = "CHECK CONFIG";
        scene_state.view_mode = hadisplay::ViewMode::Setup;
    }

    std::vector<hadisplay::Button> buttons = hadisplay::buttons_for(scene_state);

    const int touchfd = open(kTouchDevice, O_RDONLY | O_NONBLOCK);
    if (touchfd < 0) {
        std::cerr << "Failed to open " << kTouchDevice << ": " << std::strerror(errno) << "\n";
        close_async_mailbox(async_state);
        fbink_close(fbfd);
        return EXIT_FAILURE;
    }

    if (ioctl(touchfd, EVIOCGRAB, 1) < 0) {
        std::cerr << "EVIOCGRAB warning: " << std::strerror(errno) << "\n";
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    clear_screen(fbfd, true, display_settings);

    RenderState render_state{};
    if (!render(fbfd, scene_state, buttons, display_settings, render_state, true)) {
        ioctl(touchfd, EVIOCGRAB, 0);
        close(touchfd);
        close_async_mailbox(async_state);
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

    std::array<struct pollfd, 2> poll_fds{};
    poll_fds[0].fd = touchfd;
    poll_fds[0].events = POLLIN;
    const nfds_t poll_fd_count = async_state.mailbox->wake_read_fd >= 0 ? 2U : 1U;
    if (poll_fd_count == 2U) {
        poll_fds[1].fd = async_state.mailbox->wake_read_fd;
        poll_fds[1].events = POLLIN;
    }

    while (g_running) {
        now = std::chrono::steady_clock::now();
        const int ret = poll(poll_fds.data(),
                             poll_fd_count,
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
            if (poll_fd_count == 2U && (poll_fds[1].revents & POLLIN) != 0) {
                drain_async_wake_fd(*async_state.mailbox);
            }
            struct input_event ev{};
            while ((poll_fds[0].revents & POLLIN) != 0 &&
                   read(touchfd, &ev, sizeof(ev)) == static_cast<ssize_t>(sizeof(ev))) {
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

        if (process_async_completions(async_state, scene_state, config)) {
            needs_redraw = true;
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
            if (ha_client.configured()) {
                schedule_entity_refresh(async_state, ha_client, "DEVICES SYNCED", "DEVICES SYNCED");
            }
            next_light_refresh = now + fast_or_light;
        }
        if (now >= next_device_refresh) {
            const hadisplay::SystemStatus sys_status = device_status.snapshot();
            if (update_system_status(scene_state, sys_status)) {
                needs_redraw = true;
            }
            if (!sys_status.wifi_connected) {
                device_status.try_wifi_recovery();
            }
            next_device_refresh = now + fast_or_device;
        }
        if (now >= next_weather_refresh) {
            if (ha_client.configured()) {
                schedule_weather_refresh(async_state, ha_client);
            }
            next_weather_refresh = now + fast_or_weather;
        }

        if (needs_redraw) {
            buttons = hadisplay::buttons_for(scene_state);
            if (!render(fbfd, scene_state, buttons, display_settings, render_state, force_full_refresh)) {
                break;
            }
            needs_redraw = false;
            force_full_refresh = false;
        }

        if (pending_button >= 0) {
            buttons = hadisplay::buttons_for(scene_state);
            const bool should_exit = handle_button_action(scene_state,
                                                          buttons,
                                                          ha_client,
                                                          device_status,
                                                          config_store,
                                                          config,
                                                          async_state,
                                                          config_dirty,
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
                if (!render(fbfd, scene_state, buttons, display_settings, render_state, force_full_refresh)) {
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

    clear_screen(fbfd, true, display_settings);
    ioctl(touchfd, EVIOCGRAB, 0);
    close(touchfd);
    close_async_mailbox(async_state);
    fbink_close(fbfd);
    std::cerr << "hadisplay exiting.\n";
    return EXIT_SUCCESS;
}
