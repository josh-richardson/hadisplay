#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hadisplay {

enum class KoboDeviceFamily {
    Generic = 0,
    MediaTekHwtcon,
    IMxEpdc,
};

struct TouchAxisRange {
    int minimum = 0;
    int maximum = 0;
    bool valid = false;
};

struct TouchTransform {
    TouchAxisRange x;
    TouchAxisRange y;
    int rotation = 0;
};

struct InputDevice {
    int fd = -1;
    std::string path;
    bool handles_touch = false;
    bool handles_power = false;
    bool grabbed = false;
    std::optional<TouchTransform> touch_transform;
};

struct InputDevices {
    std::vector<InputDevice> devices;
    int touch_index = -1;
    int power_index = -1;
};

struct DevicePlatform {
    std::string device_code;
    std::string firmware_version;
    std::string model_name;
    std::string framebuffer_name;
    KoboDeviceFamily family = KoboDeviceFamily::Generic;
    int framebuffer_rotation = 0;
    bool supports_kernel_suspend = false;
    bool supports_suspend_debug_paths = false;
    bool supports_display_idle_check = false;
    bool supports_hwtcon_powerdown_delay = false;
    int default_touch_max_x = 0;
    int default_touch_max_y = 0;
    std::filesystem::path touch_fallback_device = "/dev/input/event1";
    std::filesystem::path suspend_stats_path = "/sys/kernel/debug/suspend_stats";
    std::filesystem::path wakeup_sources_path = "/sys/kernel/debug/wakeup_sources";
    std::filesystem::path koreader_dir = "/mnt/onboard/.adds/koreader";
    std::filesystem::path koreader_disable_wifi_script = "/mnt/onboard/.adds/koreader/disable-wifi.sh";
    std::filesystem::path koreader_enable_wifi_script = "/mnt/onboard/.adds/koreader/enable-wifi.sh";
    std::filesystem::path koreader_obtain_ip_script = "/mnt/onboard/.adds/koreader/obtain-ip.sh";
};

DevicePlatform probe_device_platform(std::string_view framebuffer_name, int view_width, int view_height);
std::string describe_device_platform(const DevicePlatform& platform);
InputDevices discover_input_devices(const DevicePlatform& platform);
bool map_touch_to_scene(const TouchTransform& transform,
                        int raw_x,
                        int raw_y,
                        int scene_w,
                        int scene_h,
                        int& out_x,
                        int& out_y);

std::string wifi_interface_name();
bool disable_wifi_for_sleep(const DevicePlatform& platform);
bool enable_wifi_after_sleep(const DevicePlatform& platform);
bool obtain_ip_after_sleep(const DevicePlatform& platform);
bool restore_wifi_after_sleep(const DevicePlatform& platform);

}  // namespace hadisplay
