#pragma once

#include <string>
#include <vector>

namespace ha {

struct ClientConfig {
    std::string base_url;
    std::string token;
    std::string weather_entity_id;
};

struct Result {
    bool ok = false;
    std::string message;
};

enum class EntityKind {
    Light = 0,
    Switch,
    Climate,
    Sensor,
};

struct EntityState {
    bool ok = false;
    EntityKind kind = EntityKind::Light;
    std::string entity_id;
    std::string friendly_name;
    std::string state;
    std::string message;
    bool available = false;
    bool is_on = false;
    bool supports_detail = false;
    bool supports_brightness = false;
    bool supports_color_temp = false;
    bool supports_rgb = false;
    bool supports_heat_control = false;
    int brightness_percent = 0;
    int color_temp_kelvin = 0;
    int min_color_temp_kelvin = 0;
    int max_color_temp_kelvin = 0;
    int rgb_red = 255;
    int rgb_green = 255;
    int rgb_blue = 255;
    int current_temperature = 0;
    int target_temperature = 0;
    std::string hvac_action;
    std::string device_class;
    std::string unit_of_measurement;
    std::string state_class;
    bool has_numeric_value = false;
    double numeric_value = 0.0;
};

struct EntityListResult {
    bool ok = false;
    std::vector<EntityState> entities;
    std::string message;
};

struct LightState {
    bool ok = false;
    std::string entity_id;
    std::string friendly_name;
    std::string state;
    std::string message;
    bool available = false;
    bool supports_brightness = false;
    bool supports_color_temp = false;
    bool supports_rgb = false;
    int brightness_percent = 0;
    int color_temp_kelvin = 0;
    int min_color_temp_kelvin = 0;
    int max_color_temp_kelvin = 0;
    int rgb_red = 255;
    int rgb_green = 255;
    int rgb_blue = 255;
};

struct LightListResult {
    bool ok = false;
    std::vector<LightState> lights;
    std::string message;
};

struct WeatherState {
    bool ok = false;
    std::string condition;
    int temperature_high = 0;
    int temperature_low = 0;
    std::string temperature_unit = "C";
    std::string message;
};

struct SensorHistoryResult {
    bool ok = false;
    std::string entity_id;
    std::vector<double> values;
    double min_value = 0.0;
    double max_value = 0.0;
    std::string message;
};

class Client {
  public:
    explicit Client(ClientConfig config = {});

    [[nodiscard]] bool configured() const;
    [[nodiscard]] const std::string& configuration_error() const;
    [[nodiscard]] EntityListResult list_entities() const;
    [[nodiscard]] EntityState fetch_entity_state(const std::string& entity_id) const;
    [[nodiscard]] LightListResult list_lights() const;
    [[nodiscard]] LightState fetch_light_state(const std::string& entity_id) const;
    [[nodiscard]] Result toggle_light(const std::string& entity_id) const;
    [[nodiscard]] Result toggle_switch(const std::string& entity_id) const;
    [[nodiscard]] Result set_light_brightness(const std::string& entity_id, int brightness_percent) const;
    [[nodiscard]] Result set_light_color_temperature(const std::string& entity_id, int kelvin) const;
    [[nodiscard]] Result set_light_rgb(const std::string& entity_id, int red, int green, int blue) const;
    [[nodiscard]] Result set_climate_hvac_mode(const std::string& entity_id, const std::string& hvac_mode) const;
    [[nodiscard]] SensorHistoryResult fetch_sensor_history(const std::string& entity_id) const;
    [[nodiscard]] WeatherState fetch_weather_state() const;

  private:
    std::string base_url_;
    std::string token_;
    std::string configuration_error_;
    mutable std::string weather_entity_id_;
};

}  // namespace ha
