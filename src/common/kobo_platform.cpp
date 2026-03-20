#include "kobo_platform.h"

#include "logger.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <linux/input.h>
#include <optional>
#include <poll.h>
#include <sstream>
#include <string_view>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace hadisplay {
namespace {

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
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool path_exists(const std::filesystem::path& path) {
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

std::vector<std::string> split_csv(std::string_view line) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= line.size()) {
        const std::size_t comma = line.find(',', start);
        const std::size_t end = comma == std::string_view::npos ? line.size() : comma;
        parts.emplace_back(line.substr(start, end - start));
        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1;
    }
    return parts;
}

std::string parse_device_code(std::string_view version_line) {
    const std::vector<std::string> parts = split_csv(version_line);
    if (parts.empty()) {
        return {};
    }

    std::string digits;
    for (const char ch : parts.back()) {
        if (std::isdigit(static_cast<unsigned char>(ch))) {
            digits.push_back(ch);
        }
    }
    if (digits.size() >= 3) {
        return digits.substr(digits.size() - 3);
    }
    return digits;
}

std::string parse_firmware_version(std::string_view version_line) {
    const std::vector<std::string> parts = split_csv(version_line);
    if (parts.size() >= 3) {
        return parts[2];
    }
    return {};
}

std::string detect_model_name() {
    const std::filesystem::path model_path("/proc/device-tree/model");
    if (!path_exists(model_path)) {
        return {};
    }

    std::ifstream input(model_path, std::ios::binary);
    if (!input) {
        return {};
    }

    std::string value((std::istreambuf_iterator<char>(input)),
                      std::istreambuf_iterator<char>());
    value.erase(std::find(value.begin(), value.end(), '\0'), value.end());
    return trim(value);
}

int detect_framebuffer_rotation() {
    const std::string rotation = read_trimmed_file("/sys/class/graphics/fb0/rotate");
    if (rotation.empty()) {
        return 0;
    }
    return std::atoi(rotation.c_str());
}

DevicePlatform fallback_platform(std::string_view framebuffer_name, int view_width, int view_height) {
    DevicePlatform platform;
    platform.framebuffer_name = std::string(framebuffer_name);
    platform.framebuffer_rotation = detect_framebuffer_rotation();
    platform.default_touch_max_x = std::max(0, view_height - 1);
    platform.default_touch_max_y = std::max(0, view_width - 1);
    return platform;
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

bool run_command(const std::string& command, const std::string& description) {
    log_info("Running " + description + ": " + command);
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
    log_warn(error.str());
    return false;
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
                log_info("Wi-Fi association completed");
                return true;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    log_warn("Timed out waiting for Wi-Fi association");
    return false;
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

std::optional<input_absinfo> query_abs_info(int fd, int code) {
    input_absinfo info{};
    if (ioctl(fd, EVIOCGABS(code), &info) < 0) {
        return std::nullopt;
    }
    return info;
}

TouchAxisRange touch_axis_range_from_abs(int fd, int preferred_code, int fallback_code, int fallback_maximum) {
    if (const auto info = query_abs_info(fd, preferred_code); info.has_value()) {
        return {
            .minimum = info->minimum,
            .maximum = info->maximum,
            .valid = info->maximum >= info->minimum,
        };
    }
    if (const auto info = query_abs_info(fd, fallback_code); info.has_value()) {
        return {
            .minimum = info->minimum,
            .maximum = info->maximum,
            .valid = info->maximum >= info->minimum,
        };
    }
    return {
        .minimum = 0,
        .maximum = fallback_maximum,
        .valid = fallback_maximum > 0,
    };
}

TouchTransform build_touch_transform(int fd, const DevicePlatform& platform) {
    return {
        .x = touch_axis_range_from_abs(fd, ABS_MT_POSITION_X, ABS_X, platform.default_touch_max_x),
        .y = touch_axis_range_from_abs(fd, ABS_MT_POSITION_Y, ABS_Y, platform.default_touch_max_y),
        .rotation = platform.framebuffer_rotation,
    };
}

int normalize_axis_value(int raw_value, const TouchAxisRange& axis, int size, bool invert) {
    if (!axis.valid || size <= 0) {
        return 0;
    }

    const int clamped = std::clamp(raw_value, axis.minimum, axis.maximum);
    const long long offset = static_cast<long long>(clamped) - static_cast<long long>(axis.minimum);
    const long long span = static_cast<long long>(axis.maximum) - static_cast<long long>(axis.minimum) + 1LL;
    if (span <= 0) {
        return 0;
    }

    long long scaled = (offset * static_cast<long long>(size)) / span;
    if (scaled >= static_cast<long long>(size)) {
        scaled = static_cast<long long>(size) - 1LL;
    }
    int value = static_cast<int>(std::clamp<long long>(scaled, 0, static_cast<long long>(size - 1)));
    if (invert) {
        value = (size - 1) - value;
    }
    return value;
}

std::string family_label(KoboDeviceFamily family) {
    switch (family) {
        case KoboDeviceFamily::MediaTekHwtcon:
            return "mediatek-hwtcon";
        case KoboDeviceFamily::IMxEpdc:
            return "imx-epdc";
        case KoboDeviceFamily::Generic:
            break;
    }
    return "generic";
}

}  // namespace

DevicePlatform probe_device_platform(std::string_view framebuffer_name, int view_width, int view_height) {
    DevicePlatform platform = fallback_platform(framebuffer_name, view_width, view_height);

    const std::string version_line = read_trimmed_file("/mnt/onboard/.kobo/version");
    if (!version_line.empty()) {
        platform.device_code = parse_device_code(version_line);
        platform.firmware_version = parse_firmware_version(version_line);
    }

    platform.model_name = detect_model_name();
    const std::string fb_name = lowercase_ascii(platform.framebuffer_name);
    const std::string model_name = lowercase_ascii(platform.model_name);

    if (fb_name.find("hwtcon") != std::string::npos || model_name.find("mediatek") != std::string::npos) {
        platform.family = KoboDeviceFamily::MediaTekHwtcon;
        platform.supports_kernel_suspend = path_exists("/sys/power/state") && path_exists("/sys/power/state-extended");
        platform.supports_suspend_debug_paths = path_exists(platform.suspend_stats_path) && path_exists(platform.wakeup_sources_path);
        platform.supports_display_idle_check = platform.supports_suspend_debug_paths;
        platform.supports_hwtcon_powerdown_delay = true;
    } else if (fb_name.find("epdc") != std::string::npos ||
               model_name.find("imx") != std::string::npos ||
               model_name.find("freescale") != std::string::npos ||
               model_name.find("ntx") != std::string::npos) {
        platform.family = KoboDeviceFamily::IMxEpdc;
    }

    return platform;
}

std::string describe_device_platform(const DevicePlatform& platform) {
    std::ostringstream out;
    out << "device=" << (platform.device_code.empty() ? "unknown" : platform.device_code)
        << " family=" << family_label(platform.family)
        << " fb=" << (platform.framebuffer_name.empty() ? "unknown" : platform.framebuffer_name)
        << " rotate=" << platform.framebuffer_rotation;
    if (!platform.firmware_version.empty()) {
        out << " firmware=" << platform.firmware_version;
    }
    if (!platform.model_name.empty()) {
        out << " model=\"" << platform.model_name << "\"";
    }
    return out.str();
}

InputDevices discover_input_devices(const DevicePlatform& platform) {
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
        if (handles_touch) {
            device.touch_transform = build_touch_transform(fd, platform);
        }
        discovered.devices.push_back(std::move(device));
        const int index = static_cast<int>(discovered.devices.size()) - 1;

        if (handles_touch && discovered.touch_index < 0) {
            discovered.touch_index = index;
        }
        if (handles_power) {
            if (discovered.power_index < 0) {
                discovered.power_index = index;
            } else {
                const InputDevice& current_power = discovered.devices[static_cast<std::size_t>(discovered.power_index)];
                if (current_power.handles_touch && !handles_touch) {
                    discovered.power_index = index;
                }
            }
        }
    }

    if (discovered.touch_index < 0) {
        const int fallback_fd = open(platform.touch_fallback_device.c_str(), O_RDONLY | O_NONBLOCK);
        if (fallback_fd >= 0) {
            InputDevice device;
            device.fd = fallback_fd;
            device.path = platform.touch_fallback_device.string();
            device.handles_touch = true;
            device.touch_transform = build_touch_transform(fallback_fd, platform);
            discovered.devices.push_back(std::move(device));
            discovered.touch_index = static_cast<int>(discovered.devices.size()) - 1;
        }
    }

    return discovered;
}

bool map_touch_to_scene(const TouchTransform& transform,
                        int raw_x,
                        int raw_y,
                        int scene_w,
                        int scene_h,
                        int& out_x,
                        int& out_y) {
    if (!transform.x.valid || !transform.y.valid || scene_w <= 0 || scene_h <= 0) {
        out_x = 0;
        out_y = 0;
        return false;
    }

    switch (transform.rotation) {
        case 1:
            out_x = normalize_axis_value(raw_y, transform.y, scene_w, false);
            out_y = normalize_axis_value(raw_x, transform.x, scene_h, true);
            return true;
        case 2:
            out_x = normalize_axis_value(raw_x, transform.x, scene_w, true);
            out_y = normalize_axis_value(raw_y, transform.y, scene_h, true);
            return true;
        case 3:
            out_x = normalize_axis_value(raw_y, transform.y, scene_w, true);
            out_y = normalize_axis_value(raw_x, transform.x, scene_h, false);
            return true;
        case 0:
        default:
            out_x = normalize_axis_value(raw_x, transform.x, scene_w, false);
            out_y = normalize_axis_value(raw_y, transform.y, scene_h, false);
            return true;
    }
}

std::string wifi_interface_name() {
    const char* raw = std::getenv("INTERFACE");
    if (raw != nullptr && *raw != '\0') {
        return raw;
    }
    return "wlan0";
}

bool disable_wifi_for_sleep(const DevicePlatform& platform) {
    if (path_exists(platform.koreader_disable_wifi_script)) {
        const std::string command = "cd '" + shell_escape_single_quotes(platform.koreader_dir.string()) +
                                    "' && /bin/sh '" + shell_escape_single_quotes(platform.koreader_disable_wifi_script.string()) + "'";
        if (run_command(command, "KOReader Wi-Fi disable")) {
            return true;
        }
    }

    const std::string iface = wifi_interface_name();
    const std::string command = "ifconfig '" + shell_escape_single_quotes(iface) + "' down >/dev/null 2>&1";
    return run_command(command, "fallback Wi-Fi disable");
}

bool enable_wifi_after_sleep(const DevicePlatform& platform) {
    if (path_exists(platform.koreader_enable_wifi_script)) {
        const std::string command = "cd '" + shell_escape_single_quotes(platform.koreader_dir.string()) +
                                    "' && /bin/sh '" + shell_escape_single_quotes(platform.koreader_enable_wifi_script.string()) + "'";
        if (run_command(command, "KOReader Wi-Fi enable")) {
            return true;
        }
    }

    const std::string iface = wifi_interface_name();
    const std::string command = "ifconfig '" + shell_escape_single_quotes(iface) + "' up >/dev/null 2>&1";
    return run_command(command, "fallback Wi-Fi enable");
}

bool obtain_ip_after_sleep(const DevicePlatform& platform) {
    if (path_exists(platform.koreader_obtain_ip_script)) {
        const std::string command = "cd '" + shell_escape_single_quotes(platform.koreader_dir.string()) +
                                    "' && /bin/sh '" + shell_escape_single_quotes(platform.koreader_obtain_ip_script.string()) + "'";
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

bool restore_wifi_after_sleep(const DevicePlatform& platform) {
    if (!enable_wifi_after_sleep(platform)) {
        return false;
    }
    if (!wait_for_wifi_association(std::chrono::seconds(15))) {
        return false;
    }
    return obtain_ip_after_sleep(platform);
}

}  // namespace hadisplay
