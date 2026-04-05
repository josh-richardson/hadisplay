#include "system_status.h"

#include "logger.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string_view>
#include <system_error>
#include <vector>

namespace hadisplay {
namespace {

constexpr std::array<int, 5> kBrightnessSteps{0, 5, 10, 50, 100};

std::string trim(std::string value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

std::string lowercase_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        if (ch >= 'A' && ch <= 'Z') {
            return static_cast<char>(ch - 'A' + 'a');
        }
        return static_cast<char>(ch);
    });
    return value;
}

std::string uppercase_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        if (ch >= 'a' && ch <= 'z') {
            return static_cast<char>(ch - 'a' + 'A');
        }
        return static_cast<char>(ch);
    });
    return value;
}

bool ends_with(std::string_view value, std::string_view suffix) {
    return value.size() >= suffix.size() &&
           value.substr(value.size() - suffix.size(), suffix.size()) == suffix;
}

std::optional<int> parse_int(std::string_view value) {
    int parsed = 0;
    const auto* begin = value.data();
    const auto* end = value.data() + value.size();
    const auto [ptr, ec] = std::from_chars(begin, end, parsed);
    if (ec != std::errc() || ptr != end) {
        return std::nullopt;
    }
    return parsed;
}

std::string read_trimmed(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        return {};
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    return trim(buffer.str());
}

bool write_string(const std::filesystem::path& path, const std::string& value) {
    std::ofstream output(path);
    if (!output) {
        return false;
    }

    output << value;
    return output.good();
}

bool path_is_directory(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_directory(path, ec);
}

bool path_exists(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

bool is_wireless_interface_dir(const std::filesystem::path& path) {
    const std::string name = lowercase_ascii(path.filename().string());
    if (path_exists(path / "wireless")) {
        return true;
    }
    return name.starts_with("wl") || name.starts_with("wlan");
}

bool looks_like_frontlight_name(const std::string& name) {
    const std::string lower = lowercase_ascii(name);
    return lower.find("frontlight") != std::string::npos ||
           lower.find("backlight") != std::string::npos ||
           lower.find("-bl") != std::string::npos ||
           lower.find("_bl") != std::string::npos ||
           lower.find("_fl") != std::string::npos ||
           lower.find("-fl") != std::string::npos;
}

std::string format_time_label() {
    const std::time_t now = std::time(nullptr);
    std::tm local_tm{};
    localtime_r(&now, &local_tm);

    std::ostringstream out;
    out << std::put_time(&local_tm, "%H:%M");
    return out.str();
}

std::string format_date_label() {
    const std::time_t now = std::time(nullptr);
    std::tm local_tm{};
    localtime_r(&now, &local_tm);

    std::ostringstream out;
    out << std::put_time(&local_tm, "%a %d %b");
    return uppercase_ascii(out.str());
}

std::string make_brightness_label(int percent) {
    std::ostringstream out;
    out << std::clamp(percent, 0, 100);
    return out.str();
}

int next_brightness_step(int current_percent) {
    for (const int step : kBrightnessSteps) {
        if (step > current_percent) {
            return step;
        }
    }
    return 0;
}

}  // namespace

struct DeviceStatus::Impl {
    struct BrightnessChannel {
        std::filesystem::path brightness_path;
        std::filesystem::path max_brightness_path;
    };

    std::vector<BrightnessChannel> brightness_channels;
    bool brightness_discovered = false;
    std::filesystem::path battery_supply_path;
    bool battery_discovered = false;
    std::string wifi_interface;
    bool wifi_discovered = false;
    bool last_wifi_connected = true;
    bool wifi_state_logged = false;
    std::string last_error;

    void ensure_brightness_channels() {
        if (brightness_discovered) {
            return;
        }

        brightness_discovered = true;
        auto discover_from_root = [this](const std::filesystem::path& root, bool name_filter) {
            if (!path_is_directory(root)) {
                return;
            }

            std::error_code ec;
            for (std::filesystem::directory_iterator it(root, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
                const auto& entry = *it;
                if (!entry.is_directory(ec)) {
                    continue;
                }

                const auto dir = entry.path();
                if (name_filter && !looks_like_frontlight_name(dir.filename().string())) {
                    continue;
                }

                const auto brightness_path = dir / "brightness";
                const auto max_brightness_path = dir / "max_brightness";
                if (path_exists(brightness_path) && path_exists(max_brightness_path)) {
                    brightness_channels.push_back({brightness_path, max_brightness_path});
                }
            }
        };

        discover_from_root("/sys/class/backlight", false);
        if (brightness_channels.empty()) {
            discover_from_root("/sys/class/leds", true);
        }

        std::sort(brightness_channels.begin(),
                  brightness_channels.end(),
                  [](const BrightnessChannel& lhs, const BrightnessChannel& rhs) {
                      return lhs.brightness_path < rhs.brightness_path;
                  });

        if (brightness_channels.size() > 1) {
            std::vector<std::string> names;
            names.reserve(brightness_channels.size());
            for (const auto& channel : brightness_channels) {
                names.push_back(lowercase_ascii(channel.brightness_path.parent_path().filename().string()));
            }

            auto has_primary_sibling = [&names](const std::string& name) {
                if (name.size() < 2) {
                    return false;
                }
                const char suffix = name.back();
                if (suffix != 'a' && suffix != 'b') {
                    return false;
                }
                const std::string base = name.substr(0, name.size() - 1);
                return std::find(names.begin(), names.end(), base) != names.end();
            };

            auto score_channel = [this, &has_primary_sibling](const BrightnessChannel& channel) {
                const std::string name = lowercase_ascii(channel.brightness_path.parent_path().filename().string());
                const int max_brightness = parse_int(read_trimmed(channel.max_brightness_path)).value_or(0);

                int score = 0;
                if (ends_with(name, "_led") || ends_with(name, "-led") || ends_with(name, "led")) {
                    score += 80;
                }
                if (name.find("lm3630") != std::string::npos) {
                    score += 40;
                }
                if (name.find("mxc_msp430") != std::string::npos) {
                    score += 30;
                }
                if (max_brightness > 0 && max_brightness <= 100) {
                    score += 20;
                }
                if (has_primary_sibling(name)) {
                    score -= 200;
                }
                return score;
            };

            const auto best = std::max_element(brightness_channels.begin(),
                                               brightness_channels.end(),
                                               [&score_channel](const BrightnessChannel& lhs, const BrightnessChannel& rhs) {
                                                   return score_channel(lhs) < score_channel(rhs);
                                               });
            if (best != brightness_channels.end()) {
                brightness_channels = {*best};
            }
        }
    }

    void ensure_battery_supply() {
        if (battery_discovered) {
            return;
        }

        battery_discovered = true;
        const std::filesystem::path root("/sys/class/power_supply");
        if (!path_is_directory(root)) {
            return;
        }

        std::error_code ec;
        int best_score = std::numeric_limits<int>::min();
        for (std::filesystem::directory_iterator it(root, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
            const auto& entry = *it;
            if (!entry.is_directory(ec)) {
                continue;
            }

            const auto dir = entry.path();
            const std::string type = lowercase_ascii(read_trimmed(dir / "type"));
            const std::string name = lowercase_ascii(dir.filename().string());
            if (type == "battery" || name == "battery" || name.find("bat") != std::string::npos) {
                int score = 0;
                if (type == "battery") {
                    score += 100;
                }
                if (name == "battery" || name.find("bat") != std::string::npos) {
                    score += 50;
                }
                if (name.find("charger") != std::string::npos) {
                    score -= 200;
                }
                if (path_exists(dir / "capacity")) {
                    score += 100;
                }
                if (path_exists(dir / "charge_now") || path_exists(dir / "energy_now")) {
                    score += 25;
                }
                if (path_exists(dir / "status")) {
                    score += 10;
                }

                if (score > best_score) {
                    best_score = score;
                    battery_supply_path = dir;
                }
            }
        }
    }

    void ensure_wifi_interface() {
        if (wifi_discovered) {
            return;
        }

        wifi_discovered = true;

        if (const char* interface_env = std::getenv("INTERFACE")) {
            std::filesystem::path env_path = std::filesystem::path("/sys/class/net") / interface_env;
            if (is_wireless_interface_dir(env_path)) {
                wifi_interface = interface_env;
                return;
            }
        }

        std::ifstream wireless("/proc/net/wireless");
        std::string line;
        while (std::getline(wireless, line)) {
            const auto colon = line.find(':');
            if (colon == std::string::npos) {
                continue;
            }

            const std::string iface = trim(line.substr(0, colon));
            if (!iface.empty()) {
                wifi_interface = iface;
                return;
            }
        }

        const std::filesystem::path root("/sys/class/net");
        if (!path_is_directory(root)) {
            return;
        }

        std::error_code ec;
        for (std::filesystem::directory_iterator it(root, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
            const auto& entry = *it;
            if (!entry.is_directory(ec)) {
                continue;
            }

            if (is_wireless_interface_dir(entry.path())) {
                wifi_interface = entry.path().filename().string();
                return;
            }
        }
    }

    int read_brightness_percent() {
        ensure_brightness_channels();
        if (brightness_channels.empty()) {
            return -1;
        }

        double total_percent = 0.0;
        int samples = 0;
        for (const auto& channel : brightness_channels) {
            const auto brightness = parse_int(read_trimmed(channel.brightness_path));
            const auto max_brightness = parse_int(read_trimmed(channel.max_brightness_path));
            if (!brightness.has_value() || !max_brightness.has_value() || *max_brightness <= 0) {
                continue;
            }

            total_percent += (static_cast<double>(*brightness) * 100.0) / static_cast<double>(*max_brightness);
            ++samples;
        }

        if (samples == 0) {
            return -1;
        }

        return std::clamp(static_cast<int>(std::lround(total_percent / static_cast<double>(samples))), 0, 100);
    }

    std::string read_wifi_label() {
        ensure_wifi_interface();
        if (wifi_interface.empty()) {
            return "OFF";
        }

        std::ifstream wireless("/proc/net/wireless");
        std::string line;
        while (std::getline(wireless, line)) {
            const auto colon = line.find(':');
            if (colon == std::string::npos) {
                continue;
            }

            const std::string iface = trim(line.substr(0, colon));
            if (iface == wifi_interface) {
                return "ON";
            }
        }

        const std::string operstate = lowercase_ascii(read_trimmed(std::filesystem::path("/sys/class/net") / wifi_interface / "operstate"));
        if (operstate == "up" || operstate == "unknown" || operstate == "dormant") {
            return "ON";
        }
        return "OFF";
    }

    void attempt_wifi_recovery() {
        ensure_wifi_interface();
        const std::string iface = wifi_interface.empty() ? "wlan0" : wifi_interface;

        log_warn("WiFi disconnected on " + iface + ", attempting wpa_cli reconnect");
        const std::string reconnect_cmd = "wpa_cli -i " + iface + " reconnect >/dev/null 2>&1";
        const int rc = std::system(reconnect_cmd.c_str());
        if (rc == 0) {
            log_info("wpa_cli reconnect returned success");
        } else {
            log_warn("wpa_cli reconnect returned " + std::to_string(rc));
        }
    }

    std::string read_battery_label() {
        ensure_battery_supply();
        if (battery_supply_path.empty()) {
            return "N/A";
        }

        std::optional<int> capacity = parse_int(read_trimmed(battery_supply_path / "capacity"));
        if (!capacity.has_value()) {
            const auto now = parse_int(read_trimmed(battery_supply_path / "charge_now"));
            const auto full = parse_int(read_trimmed(battery_supply_path / "charge_full"));
            if (now.has_value() && full.has_value() && *full > 0) {
                capacity = std::clamp(static_cast<int>(std::lround((static_cast<double>(*now) * 100.0) / static_cast<double>(*full))), 0, 100);
            }
        }
        if (!capacity.has_value()) {
            const auto now = parse_int(read_trimmed(battery_supply_path / "energy_now"));
            const auto full = parse_int(read_trimmed(battery_supply_path / "energy_full"));
            if (now.has_value() && full.has_value() && *full > 0) {
                capacity = std::clamp(static_cast<int>(std::lround((static_cast<double>(*now) * 100.0) / static_cast<double>(*full))), 0, 100);
            }
        }

        if (!capacity.has_value()) {
            return "N/A";
        }

        std::ostringstream out;
        out << *capacity << '%';
        return out.str();
    }

    bool read_battery_charging() {
        ensure_battery_supply();
        if (battery_supply_path.empty()) {
            return false;
        }

        const std::string status = lowercase_ascii(read_trimmed(battery_supply_path / "status"));
        return status == "charging" || status == "full";
    }

    bool write_brightness_percent(int percent) {
        ensure_brightness_channels();
        if (brightness_channels.empty()) {
            last_error = "Brightness control unavailable";
            return false;
        }

        bool ok = true;
        for (const auto& channel : brightness_channels) {
            const auto max_brightness = parse_int(read_trimmed(channel.max_brightness_path));
            if (!max_brightness.has_value() || *max_brightness <= 0) {
                ok = false;
                continue;
            }

            const int target_value = std::clamp(static_cast<int>(std::lround((static_cast<double>(*max_brightness) * percent) / 100.0)),
                                                0,
                                                *max_brightness);
            if (!write_string(channel.brightness_path, std::to_string(target_value))) {
                ok = false;
            }
        }

        if (!ok) {
            last_error = "Failed to write brightness";
        } else {
            last_error.clear();
        }
        return ok;
    }

    struct SavedBrightness {
        std::filesystem::path path;
        std::string value;
    };
    std::vector<SavedBrightness> saved_brightness;
    bool brightness_saved = false;

    void save_and_disable_all_brightness() {
        saved_brightness.clear();
        brightness_saved = false;

        // Read ALL backlight channels first, then zero them.  Reading and
        // zeroing in a single pass can cascade: writing 0 to the master
        // device (lm3630a_led) may reset sub-channels (lm3630a_leda/b)
        // before we read them.
        const std::filesystem::path root("/sys/class/backlight");
        if (!path_is_directory(root)) {
            return;
        }

        std::error_code ec;
        for (std::filesystem::directory_iterator it(root, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
            const auto brightness_path = it->path() / "brightness";
            if (!path_exists(brightness_path)) {
                continue;
            }
            const std::string current = read_trimmed(brightness_path);
            if (!current.empty()) {
                saved_brightness.push_back({brightness_path, current});
                log_info("Backlight save: " + it->path().filename().string() + " = " + current);
            }
        }

        // Now zero all channels.
        for (const auto& entry : saved_brightness) {
            write_string(entry.path, "0");
        }
        brightness_saved = true;
    }

    void restore_all_brightness() {
        if (!brightness_saved) {
            return;
        }
        // After kernel suspend the backlight controller may come up in an
        // undefined power state (bl_power != 0).  Reset bl_power to
        // FB_BLANK_UNBLANK (0) before writing brightness so the value
        // actually takes effect.
        for (const auto& entry : saved_brightness) {
            const auto bl_power_path = entry.path.parent_path() / "bl_power";
            if (path_exists(bl_power_path)) {
                write_string(bl_power_path, "0");
            }
            write_string(entry.path, entry.value);
            log_info("Backlight restore: " + entry.path.parent_path().filename().string() + " = " + entry.value);
        }
        saved_brightness.clear();
        brightness_saved = false;
    }
};

SystemStatus DeviceStatus::snapshot() {
    if (impl_ == nullptr) {
        impl_ = new Impl();
    }

    SystemStatus status;
    status.time_label = format_time_label();
    status.date_label = format_date_label();
    status.wifi_label = impl_->read_wifi_label();
    status.wifi_connected = status.wifi_label == "ON";
    if (impl_->wifi_state_logged && status.wifi_connected != impl_->last_wifi_connected) {
        if (status.wifi_connected) {
            log_info("WiFi connected");
        } else {
            log_warn("WiFi lost");
        }
    }
    impl_->last_wifi_connected = status.wifi_connected;
    impl_->wifi_state_logged = true;
    status.battery_label = impl_->read_battery_label();
    status.battery_available = status.battery_label != "N/A";
    status.battery_charging = impl_->read_battery_charging();
    if (status.battery_available) {
        const std::string percent_text = status.battery_label.substr(0, status.battery_label.find('%'));
        status.battery_percent = parse_int(percent_text).value_or(0);
    }

    const int brightness_percent = impl_->read_brightness_percent();
    if (brightness_percent >= 0) {
        status.brightness_available = true;
        status.brightness_percent = brightness_percent;
        status.brightness_label = make_brightness_label(brightness_percent);
    }

    return status;
}

bool DeviceStatus::cycle_brightness(SystemStatus& out_status) {
    if (impl_ == nullptr) {
        impl_ = new Impl();
    }

    const int current_percent = std::max(0, impl_->read_brightness_percent());
    const int next_percent = next_brightness_step(current_percent);
    const bool ok = impl_->write_brightness_percent(next_percent);
    out_status = snapshot();
    if (ok && out_status.brightness_available) {
        out_status.brightness_percent = next_percent;
        out_status.brightness_label = make_brightness_label(next_percent);
    }
    return ok;
}

void DeviceStatus::save_and_disable_brightness() {
    if (impl_ == nullptr) {
        impl_ = new Impl();
    }
    impl_->save_and_disable_all_brightness();
}

void DeviceStatus::restore_brightness() {
    if (impl_ == nullptr) {
        return;
    }
    impl_->restore_all_brightness();
}

void DeviceStatus::try_wifi_recovery() {
    if (impl_ == nullptr) {
        impl_ = new Impl();
    }
    impl_->attempt_wifi_recovery();
}

DeviceStatus::~DeviceStatus() {
    delete impl_;
}

DeviceStatus::DeviceStatus(DeviceStatus&& other) noexcept : impl_(other.impl_) {
    other.impl_ = nullptr;
}

DeviceStatus& DeviceStatus::operator=(DeviceStatus&& other) noexcept {
    if (this != &other) {
        delete impl_;
        impl_ = other.impl_;
        other.impl_ = nullptr;
    }
    return *this;
}

const std::string& DeviceStatus::last_error() const {
    static const std::string empty;
    if (impl_ == nullptr) {
        return empty;
    }
    return impl_->last_error;
}

}  // namespace hadisplay
