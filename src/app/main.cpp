#include "app_config.h"
#include "ha_client.h"
#include "logger.h"
#include "scene.h"
#include "system_status.h"

#include <fbink.h>
#include <linux/input.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <signal.h>

// MTK hwtcon ioctls for display power-down delay control.
// The "xon off timer" that blocks suspend is this power-down delay timer.
#define HWTCON_IOCTL_MAGIC_NUMBER 'F'
#define HWTCON_SET_PWRDOWN_DELAY _IOW(HWTCON_IOCTL_MAGIC_NUMBER, 0x30, int32_t)
#define HWTCON_GET_PWRDOWN_DELAY _IOR(HWTCON_IOCTL_MAGIC_NUMBER, 0x31, int32_t)

#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <poll.h>
#include <sstream>
#include <set>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace {

constexpr const char* kTouchDevice = "/dev/input/event1";
constexpr const char* kKoreaderDir = "/mnt/onboard/.adds/koreader";
constexpr const char* kKoreaderDisableWifiScript = "/mnt/onboard/.adds/koreader/disable-wifi.sh";
constexpr const char* kKoreaderEnableWifiScript = "/mnt/onboard/.adds/koreader/enable-wifi.sh";
constexpr const char* kKoreaderObtainIpScript = "/mnt/onboard/.adds/koreader/obtain-ip.sh";
constexpr const char* kSuspendStatsPath = "/sys/kernel/debug/suspend_stats";
constexpr const char* kWakeupSourcesPath = "/sys/kernel/debug/wakeup_sources";
constexpr int kTouchMaxX = 1447;
constexpr int kTouchMaxY = 1071;
constexpr int kMaxPartialRefreshes = 12;
constexpr auto kFullRefreshInterval = std::chrono::minutes(5);
constexpr auto kNormalLightPollInterval = std::chrono::minutes(1);
constexpr auto kNormalDevicePollInterval = std::chrono::minutes(5);
constexpr auto kNormalWeatherPollInterval = std::chrono::hours(1);
constexpr auto kDevPollInterval = std::chrono::seconds(10);
constexpr auto kSuspendScreenSettleDelay = std::chrono::seconds(2);
constexpr auto kResumeSettleDelay = std::chrono::milliseconds(100);
constexpr auto kDisplayIdlePollInterval = std::chrono::milliseconds(100);
constexpr auto kDisplayIdleTimeout = std::chrono::seconds(12);
constexpr int kFbinkRetryCount = 4;
constexpr auto kFbinkRetryDelay = std::chrono::milliseconds(50);
constexpr auto kUnexpectedWakeThreshold = std::chrono::seconds(5);
constexpr auto kPostResumePowerIgnoreWindow = std::chrono::seconds(2);
constexpr int kSuspendRetryCount = 3;
constexpr auto kSuspendRetryDelay = std::chrono::milliseconds(500);

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

enum class PowerState {
    Awake = 0,
    Sleeping,
};

struct PowerStateContext {
    PowerState state = PowerState::Awake;
    bool keepalive_paused = false;
    bool wifi_disabled_for_sleep = false;
    bool kernel_suspend_blocked = false;
    std::string kernel_suspend_block_reason;
    std::chrono::steady_clock::time_point ignore_power_until{};
};

struct InputDevice {
    int fd = -1;
    std::string path;
    bool handles_touch = false;
    bool handles_power = false;
    bool grabbed = false;
};

struct InputDevices {
    std::vector<InputDevice> devices;
    int touch_index = -1;
    int power_index = -1;
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
    std::string previous_state;
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

std::string trim(std::string value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

bool file_exists(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

std::string read_trimmed_file(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        return {};
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    return trim(buffer.str());
}

bool write_trimmed_file(const std::filesystem::path& path, const std::string& value) {
    std::ofstream output(path);
    if (!output) {
        return false;
    }

    output << value;
    return output.good();
}

struct SuspendStatsSnapshot {
    bool available = false;
    std::uint64_t success = 0;
    std::uint64_t fail = 0;
    std::string last_failed_dev;
    std::string last_failed_step;
    int last_failed_errno = 0;
};

SuspendStatsSnapshot read_suspend_stats_snapshot() {
    std::ifstream input(kSuspendStatsPath);
    if (!input) {
        return {};
    }

    SuspendStatsSnapshot snapshot;
    std::string line;
    while (std::getline(input, line)) {
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }

        const std::string key = trim(line.substr(0, colon));
        const std::string value = trim(line.substr(colon + 1));
        if (key == "success") {
            snapshot.success = static_cast<std::uint64_t>(std::strtoull(value.c_str(), nullptr, 10));
            snapshot.available = true;
        } else if (key == "fail") {
            snapshot.fail = static_cast<std::uint64_t>(std::strtoull(value.c_str(), nullptr, 10));
            snapshot.available = true;
        } else if (key == "last_failed_dev") {
            snapshot.last_failed_dev = value;
            snapshot.available = true;
        } else if (key == "last_failed_errno") {
            snapshot.last_failed_errno = std::atoi(value.c_str());
            snapshot.available = true;
        } else if (key == "last_failed_step") {
            snapshot.last_failed_step = value;
            snapshot.available = true;
        }
    }
    return snapshot;
}

std::string describe_suspend_failure(const SuspendStatsSnapshot& before,
                                     const SuspendStatsSnapshot& after) {
    if (after.fail > before.fail) {
        std::ostringstream reason;
        reason << "kernel suspend failed";
        if (!after.last_failed_dev.empty()) {
            reason << " at " << after.last_failed_dev;
        }
        if (!after.last_failed_step.empty()) {
            reason << " during " << after.last_failed_step;
        }
        if (after.last_failed_errno != 0) {
            reason << " (errno " << after.last_failed_errno << ")";
        }
        return reason.str();
    }

    if (after.success == before.success) {
        return "kernel suspend did not report a successful suspend cycle";
    }
    return {};
}

struct WakeupSourceState {
    bool seen = false;
    unsigned long long active_since = 0;
};

WakeupSourceState read_named_wakeup_source(std::string_view source_name) {
    std::ifstream input(kWakeupSourcesPath);
    if (!input) {
        return {};
    }

    std::string line;
    std::getline(input, line);  // Header.
    while (std::getline(input, line)) {
        if (!line.starts_with(source_name)) {
            continue;
        }

        std::istringstream fields(line);
        std::string name;
        unsigned long long active_count = 0;
        unsigned long long event_count = 0;
        unsigned long long wakeup_count = 0;
        unsigned long long expire_count = 0;
        unsigned long long active_since = 0;
        if (fields >> name >> active_count >> event_count >> wakeup_count >> expire_count >> active_since) {
            return {
                .seen = true,
                .active_since = active_since,
            };
        }
        return {};
    }

    return {};
}

bool display_suspend_path_idle(std::string* busy_reason = nullptr) {
    const WakeupSourceState hwtcon = read_named_wakeup_source("hwtcon_wakelock");
    const WakeupSourceState cmdq = read_named_wakeup_source("cmdq_wakelock");
    if (!hwtcon.seen && !cmdq.seen) {
        return true;
    }

    std::vector<std::string> busy_sources;
    if (hwtcon.seen && hwtcon.active_since != 0) {
        busy_sources.push_back("hwtcon_wakelock");
    }
    if (cmdq.seen && cmdq.active_since != 0) {
        busy_sources.push_back("cmdq_wakelock");
    }
    if (busy_reason != nullptr) {
        if (busy_sources.empty()) {
            *busy_reason = {};
        } else {
            std::ostringstream reason;
            reason << "display pipeline still busy";
            for (std::size_t i = 0; i < busy_sources.size(); ++i) {
                reason << (i == 0 ? " (" : ", ") << busy_sources[i];
            }
            reason << ")";
            *busy_reason = reason.str();
        }
    }
    return busy_sources.empty();
}

bool wait_for_display_suspend_path_idle(std::chrono::steady_clock::duration timeout,
                                        std::string* failure_reason = nullptr) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    int consecutive_idle_reads = 0;
    std::string last_busy_reason;

    while (std::chrono::steady_clock::now() < deadline) {
        std::string busy_reason;
        if (display_suspend_path_idle(&busy_reason)) {
            ++consecutive_idle_reads;
            if (consecutive_idle_reads >= 2) {
                return true;
            }
        } else {
            consecutive_idle_reads = 0;
            last_busy_reason = busy_reason;
        }
        std::this_thread::sleep_for(kDisplayIdlePollInterval);
    }

    if (failure_reason != nullptr) {
        std::ostringstream reason;
        reason << "timed out waiting for display pipeline to go idle";
        if (!last_busy_reason.empty()) {
            reason << ": " << last_busy_reason;
        }
        *failure_reason = reason.str();
    }
    return false;
}

std::optional<pid_t> keepalive_pid_from_env() {
    const char* raw = std::getenv("HADISPLAY_KEEPALIVE_PID");
    if (raw == nullptr || *raw == '\0') {
        return std::nullopt;
    }

    char* end = nullptr;
    errno = 0;
    const long parsed = std::strtol(raw, &end, 10);
    if (errno != 0 || end == raw || *end != '\0' || parsed <= 0 ||
        parsed > static_cast<long>(std::numeric_limits<pid_t>::max())) {
        return std::nullopt;
    }
    return static_cast<pid_t>(parsed);
}

bool send_signal_to_keepalive(int signal_number, std::string_view action) {
    const std::optional<pid_t> pid = keepalive_pid_from_env();
    if (!pid.has_value()) {
        return false;
    }

    if (kill(*pid, signal_number) == 0) {
        hadisplay::log_info(std::string("Keepalive ") + std::string(action) + " via signal");
        return true;
    }

    hadisplay::log_warn(std::string("Keepalive signal failed: ") + std::strerror(errno));
    return false;
}

bool run_command(const std::string& command, const std::string& description) {
    hadisplay::log_info("Running " + description + ": " + command);
    const int rc = std::system(command.c_str());
    if (rc == 0) {
        return true;
    }

    std::ostringstream error;
    error << description << " failed";
    if (rc == -1) {
        error << ": " << std::strerror(errno);
    } else if (WIFEXITED(rc)) {
        error << " with exit code " << WEXITSTATUS(rc);
    } else if (WIFSIGNALED(rc)) {
        error << " with signal " << WTERMSIG(rc);
    } else {
        error << " with status " << rc;
    }
    hadisplay::log_warn(error.str());
    return false;
}

std::string shell_escape_single_quotes(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size() + 8U);
    for (const char ch : value) {
        if (ch == '\'') {
            escaped += "'\\''";
        } else {
            escaped.push_back(ch);
        }
    }
    return escaped;
}

std::string wifi_interface_name() {
    const char* raw = std::getenv("INTERFACE");
    if (raw != nullptr && *raw != '\0') {
        return raw;
    }
    return "wlan0";
}

bool disable_wifi_for_sleep() {
    if (file_exists(kKoreaderDisableWifiScript)) {
        const std::string command = "cd '" + shell_escape_single_quotes(kKoreaderDir) +
                                    "' && /bin/sh '" + shell_escape_single_quotes(kKoreaderDisableWifiScript) + "'";
        if (run_command(command, "KOReader Wi-Fi disable")) {
            return true;
        }
    }

    const std::string iface = wifi_interface_name();
    const std::string command = "ifconfig '" + shell_escape_single_quotes(iface) + "' down >/dev/null 2>&1";
    return run_command(command, "fallback Wi-Fi disable");
}

bool enable_wifi_after_sleep() {
    if (file_exists(kKoreaderEnableWifiScript)) {
        const std::string command = "cd '" + shell_escape_single_quotes(kKoreaderDir) +
                                    "' && /bin/sh '" + shell_escape_single_quotes(kKoreaderEnableWifiScript) + "'";
        if (run_command(command, "KOReader Wi-Fi enable")) {
            return true;
        }
    }

    const std::string iface = wifi_interface_name();
    const std::string command = "ifconfig '" + shell_escape_single_quotes(iface) + "' up >/dev/null 2>&1";
    return run_command(command, "fallback Wi-Fi enable");
}

bool obtain_ip_after_sleep() {
    if (file_exists(kKoreaderObtainIpScript)) {
        const std::string command = "cd '" + shell_escape_single_quotes(kKoreaderDir) +
                                    "' && /bin/sh '" + shell_escape_single_quotes(kKoreaderObtainIpScript) + "'";
        if (run_command(command, "KOReader obtain IP")) {
            return true;
        }
    }

    const std::string iface = wifi_interface_name();
    const std::string command =
        "if [ -x /sbin/dhcpcd ]; then "
        "/sbin/dhcpcd -d -t 30 -w '" + shell_escape_single_quotes(iface) + "'; "
        "else "
        "udhcpc -S -i '" + shell_escape_single_quotes(iface) + "' -s /etc/udhcpc.d/default.script -b -q; "
        "fi";
    return run_command(command, "fallback obtain IP");
}

bool wait_for_wifi_association(std::chrono::seconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        std::FILE* pipe = popen("wpa_cli status 2>/dev/null", "r");
        if (pipe != nullptr) {
            std::string output;
            std::array<char, 256> buffer{};
            while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
                output += buffer.data();
            }
            const int rc = pclose(pipe);
            if (rc == 0 && output.find("wpa_state=COMPLETED") != std::string::npos) {
                hadisplay::log_info("Wi-Fi association completed");
                return true;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    hadisplay::log_warn("Timed out waiting for Wi-Fi association");
    return false;
}

bool restore_wifi_after_sleep() {
    if (!enable_wifi_after_sleep()) {
        return false;
    }
    if (!wait_for_wifi_association(std::chrono::seconds(15))) {
        return false;
    }
    return obtain_ip_after_sleep();
}

std::vector<unsigned long> query_event_bits(int fd, int event_type, int max_code) {
    const std::size_t bits_per_word = sizeof(unsigned long) * 8U;
    const std::size_t word_count = (static_cast<std::size_t>(max_code) + bits_per_word) / bits_per_word;
    std::vector<unsigned long> bits(word_count, 0UL);
    if (bits.empty()) {
        return bits;
    }

    const int rc = ioctl(fd, EVIOCGBIT(event_type, static_cast<int>(bits.size() * sizeof(unsigned long))), bits.data());
    if (rc < 0) {
        bits.clear();
    }
    return bits;
}

bool bit_is_set(const std::vector<unsigned long>& bits, int code) {
    if (code < 0) {
        return false;
    }
    const std::size_t bits_per_word = sizeof(unsigned long) * 8U;
    const std::size_t word_index = static_cast<std::size_t>(code) / bits_per_word;
    if (word_index >= bits.size()) {
        return false;
    }
    const unsigned long mask = 1UL << (static_cast<unsigned int>(code) % bits_per_word);
    return (bits[word_index] & mask) != 0UL;
}

InputDevices discover_input_devices() {
    InputDevices discovered;
    std::vector<std::filesystem::path> event_paths;

    std::error_code ec;
    for (std::filesystem::directory_iterator it("/dev/input", ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        const auto& entry = *it;
        const std::filesystem::path path = entry.path();
        if (!entry.is_character_file(ec) && !entry.is_regular_file(ec)) {
            continue;
        }
        const std::string filename = path.filename().string();
        if (!filename.starts_with("event")) {
            continue;
        }
        event_paths.push_back(path);
    }

    std::sort(event_paths.begin(), event_paths.end());

    for (const auto& path : event_paths) {
        const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            continue;
        }

        const std::vector<unsigned long> ev_bits = query_event_bits(fd, 0, EV_MAX);
        const bool has_keys = bit_is_set(ev_bits, EV_KEY);
        const bool has_abs = bit_is_set(ev_bits, EV_ABS);
        const std::vector<unsigned long> key_bits = has_keys ? query_event_bits(fd, EV_KEY, KEY_MAX) : std::vector<unsigned long>{};
        const std::vector<unsigned long> abs_bits = has_abs ? query_event_bits(fd, EV_ABS, ABS_MAX) : std::vector<unsigned long>{};

        const bool handles_touch =
            has_abs &&
            (bit_is_set(abs_bits, ABS_MT_POSITION_X) || bit_is_set(abs_bits, ABS_X)) &&
            (bit_is_set(abs_bits, ABS_MT_POSITION_Y) || bit_is_set(abs_bits, ABS_Y));
        const bool handles_power = has_keys && bit_is_set(key_bits, KEY_POWER);

        if (!handles_touch && !handles_power) {
            close(fd);
            continue;
        }

        InputDevice device;
        device.fd = fd;
        device.path = path.string();
        device.handles_touch = handles_touch;
        device.handles_power = handles_power;
        discovered.devices.push_back(std::move(device));
        const int index = static_cast<int>(discovered.devices.size()) - 1;

        if (handles_touch && discovered.touch_index < 0) {
            discovered.touch_index = index;
        }
        if (handles_power && discovered.power_index < 0) {
            discovered.power_index = index;
        }
    }

    if (discovered.touch_index < 0) {
        const int fallback_fd = open(kTouchDevice, O_RDONLY | O_NONBLOCK);
        if (fallback_fd >= 0) {
            discovered.devices.push_back({
                .fd = fallback_fd,
                .path = kTouchDevice,
                .handles_touch = true,
                .handles_power = false,
                .grabbed = false,
            });
            discovered.touch_index = static_cast<int>(discovered.devices.size()) - 1;
        }
    }

    return discovered;
}

bool grab_input_device(InputDevice& device) {
    if (device.fd < 0 || device.grabbed) {
        return device.fd >= 0;
    }

    if (ioctl(device.fd, EVIOCGRAB, 1) < 0) {
        hadisplay::log_warn("EVIOCGRAB warning on " + device.path + ": " + std::strerror(errno));
        return false;
    }

    device.grabbed = true;
    return true;
}

void release_input_device(InputDevice& device) {
    if (device.fd < 0) {
        return;
    }
    if (device.grabbed) {
        ioctl(device.fd, EVIOCGRAB, 0);
        device.grabbed = false;
    }
    close(device.fd);
    device.fd = -1;
}

void close_input_devices(InputDevices& devices) {
    for (auto& device : devices.devices) {
        release_input_device(device);
    }
    devices.devices.clear();
    devices.touch_index = -1;
    devices.power_index = -1;
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
    item.room_label = entity.area_name;
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
    const std::string room_label = scene_state.entities[static_cast<std::size_t>(entity_index)].room_label;
    scene_state.entities[static_cast<std::size_t>(entity_index)] = entity_item_from_state(entity_state, selected);
    if (scene_state.entities[static_cast<std::size_t>(entity_index)].room_label.empty()) {
        scene_state.entities[static_cast<std::size_t>(entity_index)].room_label = room_label;
    }
}

hadisplay::AppConfig config_from_scene(const hadisplay::SceneState& scene_state, const hadisplay::AppConfig& base_config) {
    hadisplay::AppConfig config = base_config;
    config.selected_entity_ids.clear();
    for (const hadisplay::EntityItem& entity : scene_state.entities) {
        if (entity.selected) {
            config.selected_entity_ids.push_back(entity.entity_id);
        }
    }
    config.hidden_entity_patterns = scene_state.hidden_entity_patterns;
    return config;
}

hadisplay::AppConfig selection_config_for_sync(const hadisplay::SceneState& scene_state, const hadisplay::AppConfig& base_config) {
    if (scene_state.entities.empty()) {
        return base_config;
    }
    return config_from_scene(scene_state, base_config);
}

hadisplay::SetupTypeFilter next_setup_type_filter(hadisplay::SetupTypeFilter filter) {
    switch (filter) {
        case hadisplay::SetupTypeFilter::All: return hadisplay::SetupTypeFilter::Lights;
        case hadisplay::SetupTypeFilter::Lights: return hadisplay::SetupTypeFilter::Switches;
        case hadisplay::SetupTypeFilter::Switches: return hadisplay::SetupTypeFilter::Climate;
        case hadisplay::SetupTypeFilter::Climate: return hadisplay::SetupTypeFilter::Sensors;
        case hadisplay::SetupTypeFilter::Sensors: return hadisplay::SetupTypeFilter::All;
    }
    return hadisplay::SetupTypeFilter::All;
}

hadisplay::SetupBrowseMode next_setup_browse_mode(hadisplay::SetupBrowseMode mode) {
    switch (mode) {
        case hadisplay::SetupBrowseMode::List: return hadisplay::SetupBrowseMode::Rooms;
        case hadisplay::SetupBrowseMode::Rooms: return hadisplay::SetupBrowseMode::List;
    }
    return hadisplay::SetupBrowseMode::List;
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
    hadisplay::log_info("Entity sync starting");
    std::thread([mailbox, ha_client, epoch, success_status, empty_selection_status]() mutable {
        auto result = ha_client.list_entities();
        if (result.ok) {
            hadisplay::log_info("Entity sync complete: " + std::to_string(result.entities.size()) + " entities");
        } else {
            hadisplay::log_warn("Entity sync failed: " + result.message);
        }
        post_async_completion(mailbox,
                              EntityRefreshCompletion{
                                  .epoch = epoch,
                                  .list_result = std::move(result),
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

ha::EntityState fetch_entity_state_after_action(const ha::Client& ha_client, const EntityActionRequest& request) {
    ha::EntityState latest = ha_client.fetch_entity_state(request.entity_id);
    if (!latest.ok) {
        return latest;
    }

    const bool should_wait_for_state_change =
        !request.previous_state.empty() &&
        (request.type == EntityActionType::ToggleLight ||
         request.type == EntityActionType::ToggleSwitch ||
         request.type == EntityActionType::SetClimateMode);

    if (!should_wait_for_state_change || latest.state != request.previous_state) {
        return latest;
    }

    for (int attempt = 0; attempt < 6; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(75));
        latest = ha_client.fetch_entity_state(request.entity_id);
        if (!latest.ok) {
            break;
        }
        if (latest.state != request.previous_state) {
            break;
        }
    }

    return latest;
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
            updated_state = fetch_entity_state_after_action(ha_client, request);
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
                hadisplay::log_warn("Entity refresh failed: " + refresh.list_result.message);
                scene_state.status = concise_ha_error(refresh.list_result.message);
                needs_redraw = true;
                continue;
            }
            hadisplay::log_info("Entity refresh: " + std::to_string(refresh.list_result.entities.size()) + " entities");
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
            hadisplay::log_warn("Entity action failed for " + action.entity_id + ": " + action.request_result.message);
            scene_state.status = concise_ha_error(action.request_result.message);
            needs_redraw = true;
            continue;
        }
        if (!action.updated_state.ok) {
            hadisplay::log_warn("Entity state fetch failed for " + action.entity_id + ": " + action.updated_state.message);
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

template <typename Fn>
bool retry_fbink_call(const char* operation_name, Fn&& fn) {
    for (int attempt = 0; attempt < kFbinkRetryCount; ++attempt) {
        errno = 0;
        if (fn() >= 0) {
            return true;
        }

        if (errno != EINTR || attempt + 1 >= kFbinkRetryCount) {
            std::cerr << operation_name << " failed.\n";
            return false;
        }

        std::this_thread::sleep_for(kFbinkRetryDelay);
    }

    std::cerr << operation_name << " failed.\n";
    return false;
}

bool wait_for_fbink_update_completion(int fbfd, std::string_view context) {
    const int rc = fbink_wait_for_complete(fbfd, LAST_MARKER);
    if (rc != 0 && rc != -ENOSYS && rc != -EINVAL) {
        hadisplay::log_warn("FBInk wait for marker completion failed during " + std::string(context) +
                            ": rc=" + std::to_string(rc));
        return false;
    }
    return true;
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

    if (!retry_fbink_call("fbink_print_raw_data", [&]() {
            return fbink_print_raw_data(fbfd,
                                        const_cast<unsigned char*>(buffer.pixels.data()),
                                        state.width,
                                        state.height,
                                        buffer.pixels.size(),
                                        0,
                                        0,
                                        &cfg);
        })) {
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
    (void)retry_fbink_call("fbink_cls", [&]() {
        return fbink_cls(fbfd, &cfg, nullptr, false);
    });
}

bool render_sleep_screen(int fbfd,
                         const DisplaySettings& display_settings,
                         std::string_view detail_line) {
    FBInkConfig cfg{};
    cfg.is_quiet = true;
    cfg.is_cleared = true;
    cfg.is_flashing = true;
    cfg.is_centered = true;
    cfg.is_halfway = true;
    cfg.is_padded = true;
    cfg.fontmult = 2;
    if (display_settings.effective_mode == hadisplay::DisplayMode::Color) {
        cfg.wfm_mode = WFM_GCC16;
    }

    std::string message = "Sleeping\nPress power to wake";
    if (!detail_line.empty()) {
        message += "\n\n";
        message += detail_line;
    }

    if (!retry_fbink_call("fbink_print", [&]() {
            return fbink_print(fbfd, message.c_str(), &cfg);
        })) {
        return false;
    }
    return true;
}

bool save_dirty_config(hadisplay::ConfigStore& config_store,
                       const hadisplay::SceneState& scene_state,
                       const hadisplay::AppConfig& config,
                       bool& config_dirty) {
    if (!config_dirty) {
        return true;
    }

    std::string error;
    if (!config_store.save(config_from_scene(scene_state, config), error)) {
        hadisplay::log_warn("Failed to save config before sleep: " + error);
        return false;
    }

    config_dirty = false;
    return true;
}

void pause_runtime_services(PowerStateContext& power_state) {
    if (!power_state.keepalive_paused) {
        power_state.keepalive_paused = send_signal_to_keepalive(SIGSTOP, "paused");
    }
    if (!power_state.wifi_disabled_for_sleep) {
        power_state.wifi_disabled_for_sleep = disable_wifi_for_sleep();
    }
}

void resume_runtime_services(PowerStateContext& power_state, hadisplay::DeviceStatus& device_status) {
    if (power_state.wifi_disabled_for_sleep) {
        if (!restore_wifi_after_sleep()) {
            hadisplay::log_warn("Wi-Fi restore after sleep failed");
        }
        power_state.wifi_disabled_for_sleep = false;
    }
    if (power_state.keepalive_paused) {
        send_signal_to_keepalive(SIGCONT, "resumed");
        power_state.keepalive_paused = false;
    }
    (void)device_status;
}

bool kernel_suspend_supported(const PowerStateContext& power_state,
                              const hadisplay::SystemStatus& system_status) {
    if (system_status.battery_charging) {
        hadisplay::log_info("Kernel suspend skipped while charging");
        return false;
    }
    if (power_state.kernel_suspend_blocked) {
        hadisplay::log_warn("Kernel suspend disabled for this session: " + power_state.kernel_suspend_block_reason);
        return false;
    }
    if (!file_exists("/sys/power/state") || !file_exists("/sys/power/state-extended")) {
        hadisplay::log_warn("Kernel suspend unavailable: missing sysfs power controls");
        return false;
    }

    const std::string power_states = read_trimmed_file("/sys/power/state");
    if (power_states.find("mem") == std::string::npos) {
        hadisplay::log_warn("Kernel suspend unavailable: /sys/power/state does not advertise mem");
        return false;
    }
    return true;
}

struct SuspendResult {
    bool ok = false;
    std::chrono::steady_clock::duration sleep_duration{};
    std::string failure_reason;
};

SuspendResult suspend_to_ram(int fbfd) {
    if (!wait_for_fbink_update_completion(fbfd, "pre-suspend")) {
        return {
            .ok = false,
            .sleep_duration = {},
            .failure_reason = "fbink update completion wait failed before suspend",
        };
    }

    if (!write_trimmed_file("/sys/power/state-extended", "1")) {
        hadisplay::log_warn("Failed to write /sys/power/state-extended=1");
        return {};
    }

    std::this_thread::sleep_for(kSuspendScreenSettleDelay);
    (void)wait_for_fbink_update_completion(fbfd, "state-extended suspend settle");
    std::string display_idle_failure;
    if (!wait_for_display_suspend_path_idle(kDisplayIdleTimeout, &display_idle_failure)) {
        hadisplay::log_warn("Kernel suspend skipped: " + display_idle_failure);
        write_trimmed_file("/sys/power/state-extended", "0");
        return {
            .ok = false,
            .sleep_duration = {},
            .failure_reason = display_idle_failure,
        };
    }

    // Clear the hwtcon power-down delay timer. The "xon off timer pending"
    // suspend failure is caused by this timer still running after a display
    // update. Setting it to 0 forces immediate power-down, clearing the
    // timer before we attempt suspend.
    int32_t saved_pwrdown_delay = -1;
    if (ioctl(fbfd, HWTCON_GET_PWRDOWN_DELAY, &saved_pwrdown_delay) < 0) {
        hadisplay::log_warn("HWTCON_GET_PWRDOWN_DELAY failed: " + std::string(strerror(errno)));
        saved_pwrdown_delay = 500; // default per driver docs
    } else {
        hadisplay::log_info("hwtcon power-down delay was " + std::to_string(saved_pwrdown_delay) + "ms");
    }
    int32_t zero_delay = 0;
    if (ioctl(fbfd, HWTCON_SET_PWRDOWN_DELAY, &zero_delay) < 0) {
        hadisplay::log_warn("HWTCON_SET_PWRDOWN_DELAY(0) failed: " + std::string(strerror(errno)));
    } else {
        hadisplay::log_info("Set hwtcon power-down delay to 0ms for suspend");
        // Wait for the power-down to actually happen now that the delay is 0.
        // The old timer (default 500ms) may have already been counting down,
        // but with delay=0 the controller should power down immediately once
        // any in-flight update completes.
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
    }

    ::sync();

    // Retry suspend up to kSuspendRetryCount times.
    SuspendResult result;
    for (int attempt = 0; attempt < kSuspendRetryCount; ++attempt) {
        const SuspendStatsSnapshot stats_before = read_suspend_stats_snapshot();
        const auto suspend_started_at = std::chrono::steady_clock::now();

        if (!write_trimmed_file("/sys/power/state", "mem")) {
            hadisplay::log_warn("Failed to write /sys/power/state=mem");
            write_trimmed_file("/sys/power/state-extended", "0");
            return {};
        }
        const auto resumed_at = std::chrono::steady_clock::now();
        const auto sleep_duration = resumed_at - suspend_started_at;
        const SuspendStatsSnapshot stats_after = read_suspend_stats_snapshot();

        if (stats_before.available && stats_after.available) {
            const std::string failure_reason = describe_suspend_failure(stats_before, stats_after);
            if (!failure_reason.empty()) {
                hadisplay::log_warn("Kernel suspend rejected (attempt " +
                                    std::to_string(attempt + 1) + "/" +
                                    std::to_string(kSuspendRetryCount) + "): " + failure_reason);
                if (attempt + 1 < kSuspendRetryCount) {
                    // Wait and let the display controller finish powering down
                    std::this_thread::sleep_for(kSuspendRetryDelay);
                    (void)wait_for_fbink_update_completion(fbfd, "suspend retry");
                    continue;
                }
                // Final attempt failed
                result = {
                    .ok = false,
                    .sleep_duration = sleep_duration,
                    .failure_reason = failure_reason,
                };
                break;
            }
        }

        // Suspend succeeded
        hadisplay::log_info("Kernel suspend cycle completed after " +
                            std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(sleep_duration).count()) +
                            "ms (attempt " + std::to_string(attempt + 1) + ")");
        result = {
            .ok = true,
            .sleep_duration = sleep_duration,
            .failure_reason = {},
        };
        break;
    }

    if (!write_trimmed_file("/sys/power/state-extended", "0")) {
        hadisplay::log_warn("Failed to write /sys/power/state-extended=0 after resume");
    }

    // Restore the original power-down delay after resume.
    if (saved_pwrdown_delay >= 0) {
        if (ioctl(fbfd, HWTCON_SET_PWRDOWN_DELAY, &saved_pwrdown_delay) < 0) {
            hadisplay::log_warn("HWTCON_SET_PWRDOWN_DELAY restore failed: " + std::string(strerror(errno)));
        } else {
            hadisplay::log_info("Restored hwtcon power-down delay to " + std::to_string(saved_pwrdown_delay) + "ms");
        }
    }

    std::this_thread::sleep_for(kResumeSettleDelay);
    return result;
}

void refresh_input_grabs(InputDevices& devices) {
    for (auto& device : devices.devices) {
        if (device.fd >= 0) {
            grab_input_device(device);
        }
    }
}

bool handle_button_action(hadisplay::SceneState& scene_state,
                          std::vector<hadisplay::Button>& buttons,
                          const ha::Client& ha_client,
                          hadisplay::DeviceStatus& device_status,
                          hadisplay::ConfigStore& config_store,
                          hadisplay::AppConfig& config,
                          AsyncState& async_state,
                          bool& config_dirty,
                          const hadisplay::Button& action_button,
                          bool& needs_redraw,
                          bool& force_full_refresh) {
    int selected_button_index = -1;
    for (std::size_t i = 0; i < buttons.size(); ++i) {
        if (buttons[i].id == action_button.id &&
            buttons[i].value == action_button.value &&
            buttons[i].label == action_button.label) {
            selected_button_index = static_cast<int>(i);
            break;
        }
    }

    scene_state.selected_button = selected_button_index;

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
        case hadisplay::ButtonId::SetupOpenRoom:
            scene_state.setup_room_label = action_button.label;
            scene_state.setup_page = 0;
            scene_state.status = "ROOM DEVICES";
            needs_redraw = true;
            return false;
        case hadisplay::ButtonId::SetupCycleBrowseMode:
            if (scene_state.setup_browse_mode == hadisplay::SetupBrowseMode::Rooms && !scene_state.setup_room_label.empty()) {
                scene_state.setup_room_label.clear();
                scene_state.setup_page = 0;
                scene_state.status = "ROOM LIST";
                needs_redraw = true;
                return false;
            }
            scene_state.setup_browse_mode = next_setup_browse_mode(scene_state.setup_browse_mode);
            scene_state.setup_room_label.clear();
            scene_state.setup_page = 0;
            scene_state.status = scene_state.setup_browse_mode == hadisplay::SetupBrowseMode::Rooms ? "ROOM VIEW" : "LIST VIEW";
            needs_redraw = true;
            return false;
        case hadisplay::ButtonId::SetupCycleTypeFilter:
            scene_state.setup_type_filter = next_setup_type_filter(scene_state.setup_type_filter);
            scene_state.setup_page = 0;
            scene_state.status = "TYPE FILTER UPDATED";
            needs_redraw = true;
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
                    .previous_state = entity.is_on ? "on" : "off",
                };
                if (entity.kind == hadisplay::EntityKind::Light) {
                    request.type = EntityActionType::ToggleLight;
                } else if (entity.kind == hadisplay::EntityKind::Switch) {
                    request.type = EntityActionType::ToggleSwitch;
                } else {
                    request.type = EntityActionType::SetClimateMode;
                    request.previous_state = entity.is_on ? "heat" : "off";
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
                    .previous_state = entity.is_on ? "on" : "off",
                };
                if (entity.kind == hadisplay::EntityKind::Light) {
                    request.type = EntityActionType::ToggleLight;
                } else if (entity.kind == hadisplay::EntityKind::Switch) {
                    request.type = EntityActionType::ToggleSwitch;
                } else {
                    request.type = EntityActionType::SetClimateMode;
                    request.previous_state = entity.is_on ? "heat" : "off";
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
                .previous_state = {},
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
    hadisplay::log_init({.path = "hadisplay.log"});

    FBInkConfig fbink_cfg{};
    fbink_cfg.is_quiet = true;

    const int fbfd = fbink_open();
    if (fbfd < 0) {
        hadisplay::log_error("fbink_open failed");
        std::cerr << "fbink_open failed.\n";
        return EXIT_FAILURE;
    }

    if (fbink_init(fbfd, &fbink_cfg) < 0) {
        hadisplay::log_error("fbink_init failed");
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
    scene_state.hidden_entity_patterns = config.hidden_entity_patterns;
    if (!loaded_config.ok) {
        hadisplay::log_warn("Config load failed, reset to defaults");
        scene_state.status = "CONFIG RESET";
    } else if (config.display_mode == hadisplay::DisplayMode::Color && !display_settings.has_color_panel) {
        hadisplay::log_warn("Color mode requested but no color panel; falling back to grayscale");
        std::cerr << "Color mode requested but FBInk reports no color panel; falling back to grayscale.\n";
    }

    {
        std::ostringstream startup;
        startup << "Starting hadisplay: screen=" << scene_state.width << "x" << scene_state.height
                << " display=" << (display_settings.effective_mode == hadisplay::DisplayMode::Color ? "color" : "grayscale")
                << " entities=" << config.selected_entity_ids.size()
                << " wifi=" << scene_state.wifi_label
                << " battery=" << scene_state.battery_label;
        hadisplay::log_info(startup.str());
    }

    ha::Client ha_client({
        .base_url = config.ha_url,
        .token = config.ha_token,
        .weather_entity_id = config.ha_weather_entity,
    });
    AsyncState async_state;
    configure_async_mailbox(async_state);

    if (ha_client.configured()) {
        hadisplay::log_info("HA client configured");
        if (has_selected_entities(config)) {
            scene_state.view_mode = hadisplay::ViewMode::Dashboard;
        } else {
            scene_state.view_mode = hadisplay::ViewMode::Setup;
        }
        if (scene_state.status == "STARTING") {
            scene_state.status = "SYNCING DEVICES";
        }
        // Async HTTP work is started after the first render — see below.
    } else {
        scene_state.status = "CHECK CONFIG";
        scene_state.view_mode = hadisplay::ViewMode::Setup;
    }

    std::vector<hadisplay::Button> buttons = hadisplay::buttons_for(scene_state);

    InputDevices input_devices = discover_input_devices();
    if (input_devices.touch_index < 0) {
        std::cerr << "Failed to discover a touch input device.\n";
        close_async_mailbox(async_state);
        close_input_devices(input_devices);
        fbink_close(fbfd);
        return EXIT_FAILURE;
    }

    for (auto& device : input_devices.devices) {
        grab_input_device(device);
    }

    if (input_devices.power_index >= 0) {
        hadisplay::log_info("Discovered power button on " + input_devices.devices[static_cast<std::size_t>(input_devices.power_index)].path);
    } else {
        hadisplay::log_warn("No power button input discovered; suspend/resume will be unavailable");
    }

    {
        struct sigaction sa{};
        sa.sa_handler = signal_handler;
        sa.sa_flags = SA_RESTART;  // auto-restart interrupted syscalls (e.g. fb ioctls)
        sigemptyset(&sa.sa_mask);
        sigaction(SIGINT, &sa, nullptr);
        sigaction(SIGTERM, &sa, nullptr);
    }

    clear_screen(fbfd, true, display_settings);

    RenderState render_state{};
    if (!render(fbfd, scene_state, buttons, display_settings, render_state, true)) {
        close_input_devices(input_devices);
        close_async_mailbox(async_state);
        fbink_close(fbfd);
        return EXIT_FAILURE;
    }

    std::cerr << "hadisplay running. Screen: " << scene_state.width << "x"
              << scene_state.height << ". Input devices ready.\n";

    if (ha_client.configured()) {
        schedule_entity_refresh(async_state,
                                ha_client,
                                "DEVICES SYNCED",
                                loaded_config.found ? "CONFIG EMPTY" : "SELECT LIGHTS");
        schedule_weather_refresh(async_state, ha_client);
    }

    TouchState touch{};
    PowerStateContext power_state{};
    bool needs_redraw = false;
    bool force_full_refresh = false;
    bool power_button_pressed = false;
    bool pending_power_toggle = false;
    std::optional<hadisplay::Button> pending_button;
    auto now = std::chrono::steady_clock::now();
    auto next_clock_refresh = next_minute_deadline();
    auto next_light_refresh = now + kNormalLightPollInterval;
    auto next_device_refresh = now + kNormalDevicePollInterval;
    auto next_weather_refresh = now + kNormalWeatherPollInterval;

    while (g_running) {
        now = std::chrono::steady_clock::now();
        std::vector<struct pollfd> poll_fds;
        poll_fds.reserve(input_devices.devices.size() + (async_state.mailbox->wake_read_fd >= 0 ? 1U : 0U));
        for (const auto& device : input_devices.devices) {
            if (device.fd >= 0) {
                poll_fds.push_back({.fd = device.fd, .events = POLLIN, .revents = 0});
            }
        }

        const std::optional<std::size_t> async_poll_index = async_state.mailbox->wake_read_fd >= 0
            ? std::optional<std::size_t>(poll_fds.size())
            : std::nullopt;
        if (async_poll_index.has_value()) {
            poll_fds.push_back({.fd = async_state.mailbox->wake_read_fd, .events = POLLIN, .revents = 0});
        }

        const int timeout_ms = power_state.state == PowerState::Sleeping
            ? -1
            : poll_timeout_until(now,
                                 render_state.last_full_refresh + kFullRefreshInterval,
                                 next_clock_refresh,
                                 next_light_refresh,
                                 next_device_refresh,
                                 next_weather_refresh);
        const int ret = poll(poll_fds.data(),
                             static_cast<nfds_t>(poll_fds.size()),
                             timeout_ms);
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
            if (async_poll_index.has_value() && (poll_fds[*async_poll_index].revents & POLLIN) != 0) {
                drain_async_wake_fd(*async_state.mailbox);
            }
            for (std::size_t i = 0; i < input_devices.devices.size() && i < poll_fds.size(); ++i) {
                if ((poll_fds[i].revents & POLLIN) == 0) {
                    continue;
                }

                InputDevice& device = input_devices.devices[i];
                struct input_event ev{};
                while (read(device.fd, &ev, sizeof(ev)) == static_cast<ssize_t>(sizeof(ev))) {
                    if (device.handles_power && ev.type == EV_KEY && ev.code == KEY_POWER) {
                        if (std::chrono::steady_clock::now() < power_state.ignore_power_until) {
                            power_button_pressed = false;
                            continue;
                        }
                        if (ev.value == 1) {
                            power_button_pressed = true;
                        } else if (ev.value == 0 && power_button_pressed) {
                            power_button_pressed = false;
                            pending_power_toggle = true;
                        }
                    }

                    if (!device.handles_touch || power_state.state != PowerState::Awake) {
                        if (ev.type == EV_KEY && ev.code == BTN_TOUCH && ev.value == 0) {
                            touch.touching = false;
                            touch.x = -1;
                            touch.y = -1;
                            scene_state.pressed_button = -1;
                        }
                        continue;
                    }

                    if (ev.type == EV_ABS) {
                        if (ev.code == ABS_MT_POSITION_X || ev.code == ABS_X) {
                            touch.x = ev.value;
                        } else if (ev.code == ABS_MT_POSITION_Y || ev.code == ABS_Y) {
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
                                    pending_button = buttons[static_cast<std::size_t>(released_on)];
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
        }

        if (process_async_completions(async_state, scene_state, config) &&
            power_state.state == PowerState::Awake) {
            needs_redraw = true;
        }

        if (pending_power_toggle && input_devices.power_index >= 0) {
            pending_power_toggle = false;
            pending_button.reset();
            touch = {};
            scene_state.pressed_button = -1;
            scene_state.selected_button = -1;

            if (power_state.state == PowerState::Sleeping) {
                power_state.state = PowerState::Awake;
                resume_runtime_services(power_state, device_status);
                refresh_input_grabs(input_devices);
                apply_system_status(scene_state, device_status.snapshot());
                update_clock_status(scene_state);
                scene_state.status = "AWAKE";
                force_full_refresh = true;
                needs_redraw = true;
                now = std::chrono::steady_clock::now();
                next_clock_refresh = scene_state.dev_mode ? now + kDevPollInterval : next_minute_deadline();
                next_light_refresh = now + std::chrono::seconds(5);
                next_device_refresh = now + std::chrono::seconds(2);
                next_weather_refresh = now + std::chrono::seconds(5);
            } else {
                const hadisplay::SystemStatus sleep_status = device_status.snapshot();
                apply_system_status(scene_state, sleep_status);
                update_clock_status(scene_state);
                save_dirty_config(config_store, scene_state, config, config_dirty);

                pause_runtime_services(power_state);
                const bool will_kernel_suspend = kernel_suspend_supported(power_state, sleep_status);

                // Render the sleep screen AFTER pausing services but only if
                // we won't attempt kernel suspend. The full refresh leaves the
                // hwtcon display controller with a pending "xon off" timer that
                // blocks suspend. For kernel suspend, we skip the sleep screen
                // so the display pipeline is idle when we write to /sys/power/state.
                if (!will_kernel_suspend) {
                    const std::string sleep_detail = sleep_status.battery_charging ? "Charging via USB" : "";
                    if (!render_sleep_screen(fbfd, display_settings, sleep_detail)) {
                        resume_runtime_services(power_state, device_status);
                        break;
                    }
                }

                if (will_kernel_suspend) {
                    const SuspendResult suspend_result = suspend_to_ram(fbfd);
                    if (suspend_result.ok) {
                        refresh_input_grabs(input_devices);
                        touch = {};
                        power_button_pressed = false;
                        power_state.ignore_power_until = std::chrono::steady_clock::now() + kPostResumePowerIgnoreWindow;
                        if (suspend_result.sleep_duration < kUnexpectedWakeThreshold) {
                            power_state.state = PowerState::Sleeping;
                            scene_state.status = "SLEEPING";
                            needs_redraw = false;
                            force_full_refresh = false;
                            hadisplay::log_warn("Unexpected wake detected; staying asleep");
                        } else {
                            power_state.state = PowerState::Awake;
                            resume_runtime_services(power_state, device_status);
                            apply_system_status(scene_state, device_status.snapshot());
                            update_clock_status(scene_state);
                            scene_state.status = "AWAKE";
                            force_full_refresh = true;
                            needs_redraw = true;
                            now = std::chrono::steady_clock::now();
                            next_clock_refresh = scene_state.dev_mode ? now + kDevPollInterval : next_minute_deadline();
                            next_light_refresh = now + std::chrono::seconds(5);
                            next_device_refresh = now + std::chrono::seconds(2);
                            next_weather_refresh = now + std::chrono::seconds(5);
                        }
                    } else {
                        if (!suspend_result.failure_reason.empty()) {
                            power_state.kernel_suspend_blocked = true;
                            power_state.kernel_suspend_block_reason = suspend_result.failure_reason;
                            hadisplay::log_warn("Blocking further kernel suspend attempts: " +
                                                power_state.kernel_suspend_block_reason);
                        }
                        power_state.state = PowerState::Sleeping;
                        scene_state.status = "SLEEPING";
                        needs_redraw = false;
                        force_full_refresh = false;
                        hadisplay::log_info("Entered software sleep mode after failed kernel suspend");
                    }
                } else {
                    power_state.state = PowerState::Sleeping;
                    scene_state.status = "SLEEPING";
                    needs_redraw = false;
                    force_full_refresh = false;
                    hadisplay::log_info("Entered software sleep mode");
                }
            }
        }

        if (power_state.state == PowerState::Awake) {
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
        }

        if (power_state.state == PowerState::Awake && needs_redraw) {
            buttons = hadisplay::buttons_for(scene_state);
            if (!render(fbfd, scene_state, buttons, display_settings, render_state, force_full_refresh)) {
                break;
            }
            needs_redraw = false;
            force_full_refresh = false;
        }

        if (power_state.state == PowerState::Awake && pending_button.has_value()) {
            buttons = hadisplay::buttons_for(scene_state);
            const bool should_exit = handle_button_action(scene_state,
                                                          buttons,
                                                          ha_client,
                                                          device_status,
                                                          config_store,
                                                          config,
                                                          async_state,
                                                          config_dirty,
                                                          *pending_button,
                                                          needs_redraw,
                                                          force_full_refresh);
            pending_button.reset();
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

    resume_runtime_services(power_state, device_status);

    if (config_dirty) {
        std::string error;
        if (!config_store.save(config_from_scene(scene_state, config), error)) {
            std::cerr << "Failed to save config on exit: " << error << "\n";
        }
    }

    hadisplay::log_info("Shutting down");
    clear_screen(fbfd, true, display_settings);
    close_input_devices(input_devices);
    close_async_mailbox(async_state);
    fbink_close(fbfd);
    std::cerr << "hadisplay exiting.\n";
    hadisplay::log_shutdown();
    return EXIT_SUCCESS;
}
