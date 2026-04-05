#pragma once

#include <string>

namespace hadisplay {

struct SystemStatus {
    std::string time_label = "--:--";
    std::string date_label = "NO DATE";
    std::string wifi_label = "OFF";
    std::string battery_label = "N/A";
    std::string brightness_label = "N/A";
    bool wifi_connected = false;
    bool battery_available = false;
    int battery_percent = 0;
    bool battery_charging = false;
    int brightness_percent = 0;
    bool brightness_available = false;
};

class DeviceStatus {
  public:
    DeviceStatus() = default;
    ~DeviceStatus();
    DeviceStatus(const DeviceStatus&) = delete;
    DeviceStatus& operator=(const DeviceStatus&) = delete;
    DeviceStatus(DeviceStatus&& other) noexcept;
    DeviceStatus& operator=(DeviceStatus&& other) noexcept;

    [[nodiscard]] SystemStatus snapshot();
    bool cycle_brightness(SystemStatus& out_status);
    void save_and_disable_brightness();
    void restore_brightness();
    void try_wifi_recovery();
    [[nodiscard]] const std::string& last_error() const;

  private:
    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace hadisplay
