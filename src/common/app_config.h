#pragma once

#include "scene.h"

#include <string>
#include <vector>

namespace hadisplay {

std::vector<std::string> default_hidden_entity_patterns();

struct HaLocationConfig {
    std::string id;
    std::string name;
    std::vector<std::string> match_ssids;
    std::string ha_url;
    std::string ha_token;
    std::string ha_weather_entity;
    bool is_default = false;
};

struct AppConfig {
    std::string ha_url;
    std::string ha_token;
    std::string ha_weather_entity;
    std::string default_location_id;
    std::vector<HaLocationConfig> locations;
    std::vector<std::string> selected_entity_ids;
    std::vector<std::string> hidden_entity_patterns = default_hidden_entity_patterns();
    DisplayMode display_mode = DisplayMode::Auto;
};

struct ResolvedHaConfig {
    bool ok = false;
    std::string base_url;
    std::string token;
    std::string weather_entity_id;
    std::string location_id;
    std::string location_name;
    std::string match_reason;
    std::string error;
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

[[nodiscard]] ResolvedHaConfig resolve_ha_config(const AppConfig& config, const std::string& current_ssid);

}  // namespace hadisplay
