#pragma once

#include "scene.h"

#include <string>
#include <vector>

namespace hadisplay {

struct AppConfig {
    std::string ha_url;
    std::string ha_token;
    std::string ha_weather_entity;
    std::vector<std::string> selected_entity_ids;
    DisplayMode display_mode = DisplayMode::Auto;
};

struct ConfigLoadResult {
    bool ok = false;
    bool found = false;
    AppConfig config;
    std::string path;
    std::string message;
};

class ConfigStore {
  public:
    ConfigStore();

    [[nodiscard]] ConfigLoadResult load() const;
    [[nodiscard]] bool save(const AppConfig& config, std::string& error) const;
    [[nodiscard]] const std::string& path() const;

  private:
    std::string path_;
};

}  // namespace hadisplay
