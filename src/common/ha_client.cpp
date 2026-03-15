#include "ha_client.h"

#include "json.h"

#include <curl/curl.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>
#include <unistd.h>

namespace ha {
namespace {

namespace json = hadisplay::json;

constexpr std::string_view kDefaultWeatherEntityId = "weather.home";
constexpr long kConnectTimeoutMs = 1500L;
constexpr long kRequestTimeoutMs = 3500L;

struct HttpResult {
    bool ok = false;
    long http_code = 0;
    std::string body;
    std::string message;
};

size_t append_response(void* contents, size_t size, size_t nmemb, void* userp) {
    const size_t total_size = size * nmemb;
    auto* response = static_cast<std::string*>(userp);
    response->append(static_cast<const char*>(contents), total_size);
    return total_size;
}

std::string trim(std::string value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

std::filesystem::path executable_dir() {
    std::array<char, 4096> buffer{};
    const ssize_t size = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (size <= 0) {
        return {};
    }
    buffer[static_cast<std::size_t>(size)] = '\0';
    return std::filesystem::path(buffer.data()).parent_path();
}

std::filesystem::path find_env_file() {
    if (const char* override_path = std::getenv("HADISPLAY_ENV_FILE")) {
        return std::filesystem::path(override_path);
    }

    std::array<std::filesystem::path, 6> candidates{
        std::filesystem::current_path() / ".env",
        std::filesystem::current_path().parent_path() / ".env",
        executable_dir() / ".env",
        executable_dir().parent_path() / ".env",
        executable_dir().parent_path().parent_path() / ".env",
        executable_dir().parent_path().parent_path().parent_path() / ".env",
    };

    for (const auto& candidate : candidates) {
        if (!candidate.empty() && std::filesystem::exists(candidate)) {
            return candidate;
        }
    }

    return {};
}

bool parse_env_file(const std::filesystem::path& path,
                    std::string& base_url,
                    std::string& token,
                    std::string& weather_entity,
                    std::string& error) {
    std::ifstream input(path);
    if (!input) {
        error = "Unable to open " + path.string() + ": " + std::strerror(errno);
        return false;
    }

    std::string line;
    while (std::getline(input, line)) {
        line = trim(line);
        if (line.empty() || line.starts_with('#')) {
            continue;
        }

        const auto equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }

        const std::string key = trim(line.substr(0, equals));
        const std::string value = trim(line.substr(equals + 1));
        if (key == "HA_URL") {
            base_url = value;
        } else if (key == "HA_TOKEN") {
            token = value;
        } else if (key == "HA_WEATHER_ENTITY") {
            weather_entity = value;
        }
    }

    if (base_url.empty() || token.empty()) {
        error = "Missing HA_URL or HA_TOKEN in " + path.string();
        return false;
    }

    while (!base_url.empty() && base_url.back() == '/') {
        base_url.pop_back();
    }

    return true;
}

void normalize_base_url(std::string& base_url) {
    while (!base_url.empty() && base_url.back() == '/') {
        base_url.pop_back();
    }
}

HttpResult http_request_json(const std::string& method,
                             const std::string& url,
                             const std::string& token,
                             const std::string* payload) {
    static const int curl_init = []() { return curl_global_init(CURL_GLOBAL_DEFAULT); }();
    if (curl_init != CURLE_OK) {
        return {.ok = false, .http_code = 0, .body = {}, .message = "curl_global_init failed"};
    }

    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        return {.ok = false, .http_code = 0, .body = {}, .message = "curl_easy_init failed"};
    }

    std::string response;
    struct curl_slist* headers = nullptr;
    const std::string auth_header = "Authorization: Bearer " + token;
    headers = curl_slist_append(headers, auth_header.c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, kConnectTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, kRequestTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_response);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    if (method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload != nullptr ? payload->c_str() : "");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, payload != nullptr ? static_cast<long>(payload->size()) : 0L);
    } else {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    }

    const CURLcode code = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (code != CURLE_OK) {
        return {.ok = false, .http_code = http_code, .body = std::move(response), .message = curl_easy_strerror(code)};
    }

    if (http_code < 200 || http_code >= 300) {
        std::ostringstream oss;
        oss << "HA returned HTTP " << http_code;
        return {.ok = false, .http_code = http_code, .body = std::move(response), .message = oss.str()};
    }

    return {.ok = true, .http_code = http_code, .body = std::move(response), .message = "OK"};
}

std::string lowercase_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

const json::Value::Object* object_if(const json::Value* value) {
    return value != nullptr ? value->as_object_if() : nullptr;
}

const json::Value::Array* array_if(const json::Value* value) {
    return value != nullptr ? value->as_array_if() : nullptr;
}

std::string string_or(const json::Value* value, const std::string& fallback = {}) {
    const std::string* string = value != nullptr ? value->as_string_if() : nullptr;
    return string != nullptr ? *string : fallback;
}

bool number_as_int(const json::Value* value, int& out) {
    const double* number = value != nullptr ? value->as_number_if() : nullptr;
    if (number == nullptr) {
        return false;
    }
    out = static_cast<int>(*number >= 0.0 ? std::floor(*number + 0.5) : std::ceil(*number - 0.5));
    return true;
}

bool parse_double_string(const std::string& text, double& out) {
    if (text.empty()) {
        return false;
    }
    char* end = nullptr;
    out = std::strtod(text.c_str(), &end);
    return end != nullptr && *end == '\0' && std::isfinite(out) != 0;
}

bool parse_rgb(const json::Value* value, int& red, int& green, int& blue) {
    const json::Value::Array* rgb = array_if(value);
    if (rgb == nullptr || rgb->size() < 3) {
        return false;
    }

    int components[3]{};
    for (int i = 0; i < 3; ++i) {
        if (!number_as_int(&(*rgb)[static_cast<std::size_t>(i)], components[i])) {
            return false;
        }
    }

    red = std::clamp(components[0], 0, 255);
    green = std::clamp(components[1], 0, 255);
    blue = std::clamp(components[2], 0, 255);
    return true;
}

int mired_to_kelvin(int mired) {
    if (mired <= 0) {
        return 0;
    }
    return static_cast<int>(1000000.0 / static_cast<double>(mired) + 0.5);
}

bool supports_rgb_mode(const std::string& mode) {
    return mode == "rgb" || mode == "rgbw" || mode == "rgbww" || mode == "hs" || mode == "xy";
}

std::string friendly_name_or_entity_id(const std::string& entity_id, const json::Value* attributes_value) {
    std::string friendly_name = string_or(attributes_value != nullptr ? attributes_value->get("friendly_name") : nullptr);
    if (!friendly_name.empty()) {
        return friendly_name;
    }

    const std::size_t dot = entity_id.find('.');
    return dot == std::string::npos ? entity_id : entity_id.substr(dot + 1);
}

LightState parse_light_state(const json::Value& value) {
    LightState light;
    if (value.as_object_if() == nullptr) {
        light.message = "Light payload was not an object";
        return light;
    }

    light.entity_id = string_or(value.get("entity_id"));
    light.state = lowercase_ascii(string_or(value.get("state")));
    if (light.entity_id.empty() || !light.entity_id.starts_with("light.")) {
        light.message = "Not a light entity";
        return light;
    }

    const json::Value* attributes_value = value.get("attributes");
    const json::Value::Object* attributes = object_if(attributes_value);
    light.friendly_name = friendly_name_or_entity_id(light.entity_id, attributes_value);

    light.available = light.state != "unavailable";
    light.ok = true;

    int brightness = 0;
    if (attributes != nullptr && number_as_int(attributes_value->get("brightness"), brightness)) {
        light.supports_brightness = true;
        light.brightness_percent = std::clamp(static_cast<int>((brightness * 100.0 / 255.0) + 0.5), 0, 100);
    } else if (light.state == "on") {
        light.brightness_percent = 100;
    }

    if (attributes != nullptr) {
        if (const json::Value* supported_modes_value = attributes_value->get("supported_color_modes")) {
            if (const json::Value::Array* supported_modes = supported_modes_value->as_array_if()) {
                for (const json::Value& entry : *supported_modes) {
                    const std::string mode = lowercase_ascii(string_or(&entry));
                    if (mode == "brightness") {
                        light.supports_brightness = true;
                    }
                    if (mode.find("color_temp") != std::string::npos) {
                        light.supports_color_temp = true;
                        light.supports_brightness = true;
                    }
                    if (supports_rgb_mode(mode)) {
                        light.supports_rgb = true;
                        light.supports_brightness = true;
                    }
                }
            }
        }

        const std::string color_mode = lowercase_ascii(string_or(attributes_value->get("color_mode")));
        if (color_mode.find("color_temp") != std::string::npos) {
            light.supports_color_temp = true;
        }
        if (supports_rgb_mode(color_mode)) {
            light.supports_rgb = true;
        }

        int kelvin = 0;
        if (number_as_int(attributes_value->get("color_temp_kelvin"), kelvin)) {
            light.color_temp_kelvin = kelvin;
            light.supports_color_temp = true;
        } else {
            int mired = 0;
            if (number_as_int(attributes_value->get("color_temp"), mired)) {
                light.color_temp_kelvin = mired_to_kelvin(mired);
                light.supports_color_temp = light.color_temp_kelvin > 0;
            }
        }

        int min_kelvin = 0;
        int max_kelvin = 0;
        if (number_as_int(attributes_value->get("min_color_temp_kelvin"), min_kelvin) &&
            number_as_int(attributes_value->get("max_color_temp_kelvin"), max_kelvin)) {
            light.min_color_temp_kelvin = min_kelvin;
            light.max_color_temp_kelvin = max_kelvin;
            light.supports_color_temp = true;
        } else {
            int min_mired = 0;
            int max_mired = 0;
            if (number_as_int(attributes_value->get("min_mireds"), min_mired)) {
                light.max_color_temp_kelvin = mired_to_kelvin(min_mired);
            }
            if (number_as_int(attributes_value->get("max_mireds"), max_mired)) {
                light.min_color_temp_kelvin = mired_to_kelvin(max_mired);
            }
            if (light.min_color_temp_kelvin > 0 && light.max_color_temp_kelvin > 0) {
                light.supports_color_temp = true;
            }
        }

        // Some color-temperature-only lights expose a derived rgb_color for their
        // current white point. Keep the value for display, but do not treat it as
        // proof that the light actually supports RGB control.
        parse_rgb(attributes_value->get("rgb_color"), light.rgb_red, light.rgb_green, light.rgb_blue);
    }

    if (light.supports_color_temp && light.min_color_temp_kelvin > light.max_color_temp_kelvin) {
        std::swap(light.min_color_temp_kelvin, light.max_color_temp_kelvin);
    }

    if (light.supports_brightness && light.brightness_percent == 0 && light.state == "on") {
        light.brightness_percent = 100;
    }

    light.message = "Light refreshed";
    return light;
}

EntityState entity_state_from_light(const LightState& light) {
    EntityState entity;
    entity.ok = light.ok;
    entity.kind = EntityKind::Light;
    entity.entity_id = light.entity_id;
    entity.friendly_name = light.friendly_name;
    entity.state = light.state;
    entity.message = light.message;
    entity.available = light.available;
    entity.is_on = light.state == "on";
    entity.supports_detail = light.supports_brightness || light.supports_color_temp || light.supports_rgb;
    entity.supports_brightness = light.supports_brightness;
    entity.supports_color_temp = light.supports_color_temp;
    entity.supports_rgb = light.supports_rgb;
    entity.brightness_percent = light.brightness_percent;
    entity.color_temp_kelvin = light.color_temp_kelvin;
    entity.min_color_temp_kelvin = light.min_color_temp_kelvin;
    entity.max_color_temp_kelvin = light.max_color_temp_kelvin;
    entity.rgb_red = light.rgb_red;
    entity.rgb_green = light.rgb_green;
    entity.rgb_blue = light.rgb_blue;
    return entity;
}

EntityState parse_switch_state(const json::Value& value) {
    EntityState entity;
    entity.kind = EntityKind::Switch;
    if (value.as_object_if() == nullptr) {
        entity.message = "Switch payload was not an object";
        return entity;
    }

    entity.entity_id = string_or(value.get("entity_id"));
    entity.state = lowercase_ascii(string_or(value.get("state")));
    if (entity.entity_id.empty() || !entity.entity_id.starts_with("switch.")) {
        entity.message = "Not a switch entity";
        return entity;
    }

    const json::Value* attributes_value = value.get("attributes");
    entity.friendly_name = friendly_name_or_entity_id(entity.entity_id, attributes_value);
    entity.available = entity.state != "unavailable";
    entity.is_on = entity.state == "on";
    entity.supports_detail = false;
    entity.ok = true;
    entity.message = "Switch refreshed";
    return entity;
}

EntityState parse_climate_state(const json::Value& value) {
    EntityState entity;
    entity.kind = EntityKind::Climate;
    if (value.as_object_if() == nullptr) {
        entity.message = "Climate payload was not an object";
        return entity;
    }

    entity.entity_id = string_or(value.get("entity_id"));
    entity.state = lowercase_ascii(string_or(value.get("state")));
    if (entity.entity_id.empty() || !entity.entity_id.starts_with("climate.")) {
        entity.message = "Not a climate entity";
        return entity;
    }

    const json::Value* attributes_value = value.get("attributes");
    entity.friendly_name = friendly_name_or_entity_id(entity.entity_id, attributes_value);
    entity.available = entity.state != "unavailable";
    entity.supports_detail = true;
    entity.supports_heat_control = false;
    entity.is_on = entity.state != "off";
    entity.hvac_action = lowercase_ascii(string_or(attributes_value != nullptr ? attributes_value->get("hvac_action") : nullptr));
    number_as_int(attributes_value != nullptr ? attributes_value->get("current_temperature") : nullptr, entity.current_temperature);
    if (!number_as_int(attributes_value != nullptr ? attributes_value->get("temperature") : nullptr, entity.target_temperature)) {
        number_as_int(attributes_value != nullptr ? attributes_value->get("target_temp_high") : nullptr, entity.target_temperature);
    }

    const json::Value::Array* hvac_modes = attributes_value != nullptr ? array_if(attributes_value->get("hvac_modes")) : nullptr;
    if (hvac_modes != nullptr) {
        for (const json::Value& mode_value : *hvac_modes) {
            const std::string mode = lowercase_ascii(string_or(&mode_value));
            if (mode == "heat" || mode == "off") {
                entity.supports_heat_control = true;
            }
        }
    } else {
        entity.supports_heat_control = true;
    }

    entity.ok = true;
    entity.message = "Climate refreshed";
    return entity;
}

EntityState parse_sensor_state(const json::Value& value) {
    EntityState entity;
    entity.kind = EntityKind::Sensor;
    if (value.as_object_if() == nullptr) {
        entity.message = "Sensor payload was not an object";
        return entity;
    }

    entity.entity_id = string_or(value.get("entity_id"));
    entity.state = trim(string_or(value.get("state")));
    if (entity.entity_id.empty() || !entity.entity_id.starts_with("sensor.")) {
        entity.message = "Not a sensor entity";
        return entity;
    }

    const json::Value* attributes_value = value.get("attributes");
    entity.friendly_name = friendly_name_or_entity_id(entity.entity_id, attributes_value);
    entity.available = entity.state != "unavailable" && entity.state != "unknown";
    entity.device_class = lowercase_ascii(string_or(attributes_value != nullptr ? attributes_value->get("device_class") : nullptr));
    entity.unit_of_measurement = string_or(attributes_value != nullptr ? attributes_value->get("unit_of_measurement") : nullptr);
    entity.state_class = lowercase_ascii(string_or(attributes_value != nullptr ? attributes_value->get("state_class") : nullptr));
    entity.supports_detail = true;
    entity.has_numeric_value = parse_double_string(entity.state, entity.numeric_value);
    entity.ok = true;
    entity.message = "Sensor refreshed";
    return entity;
}

EntityState parse_entity_state(const json::Value& value) {
    const std::string entity_id = string_or(value.get("entity_id"));
    if (entity_id.starts_with("light.")) {
        return entity_state_from_light(parse_light_state(value));
    }
    if (entity_id.starts_with("switch.")) {
        return parse_switch_state(value);
    }
    if (entity_id.starts_with("climate.")) {
        return parse_climate_state(value);
    }
    if (entity_id.starts_with("sensor.")) {
        return parse_sensor_state(value);
    }
    EntityState entity;
    entity.entity_id = entity_id;
    entity.message = "Unsupported entity type";
    return entity;
}

Result post_service(const std::string& base_url,
                    const std::string& token,
                    const std::string& service,
                    const json::Value::Object& payload) {
    const std::string body = json::stringify(json::Value(payload));
    const HttpResult result = http_request_json("POST", base_url + "/api/services/" + service, token, &body);
    if (!result.ok) {
        return {.ok = false, .message = result.message};
    }
    return {.ok = true, .message = "OK"};
}

const json::Value* first_forecast_payload(const json::Value& root) {
    const json::Value::Object* object = root.as_object_if();
    if (object == nullptr) {
        return nullptr;
    }
    for (const auto& [_, value] : *object) {
        if (value.get("forecast") != nullptr) {
            return &value;
        }
    }
    return nullptr;
}

std::string weather_unit_from_state(const json::Value& state) {
    const json::Value* attributes = state.get("attributes");
    const std::string temperature_unit = string_or(attributes != nullptr ? attributes->get("temperature_unit") : nullptr);
    if (temperature_unit.find('F') != std::string::npos) {
        return "F";
    }
    return "C";
}

bool find_first_weather_entity(const json::Value& root, std::string& entity_id) {
    const json::Value::Array* states = root.as_array_if();
    if (states == nullptr) {
        return false;
    }

    for (const json::Value& entry : *states) {
        const std::string candidate = string_or(entry.get("entity_id"));
        if (candidate.starts_with("weather.")) {
            entity_id = candidate;
            return true;
        }
    }
    return false;
}

json::ParseResult parse_http_json(const HttpResult& result) {
    if (!result.ok) {
        return {.ok = false, .value = {}, .error = result.message};
    }
    const json::ParseResult parsed = json::parse(result.body);
    if (!parsed.ok) {
        return {.ok = false, .value = {}, .error = "Invalid JSON: " + parsed.error};
    }
    return parsed;
}

}  // namespace

Client::Client(ClientConfig config) {
    base_url_ = std::move(config.base_url);
    token_ = std::move(config.token);
    weather_entity_id_ = std::move(config.weather_entity_id);

    std::string env_base_url;
    std::string env_token;
    std::string env_weather_entity;
    std::string env_error;
    const auto env_path = find_env_file();
    if (!env_path.empty()) {
        parse_env_file(env_path, env_base_url, env_token, env_weather_entity, env_error);
    }

    if (base_url_.empty()) {
        base_url_ = std::move(env_base_url);
    }
    if (token_.empty()) {
        token_ = std::move(env_token);
    }
    if (weather_entity_id_.empty()) {
        weather_entity_id_ = std::move(env_weather_entity);
    }

    normalize_base_url(base_url_);
    if (base_url_.empty() || token_.empty()) {
        configuration_error_ = "Missing HA URL or token in config/.env";
    }
}

bool Client::configured() const {
    return configuration_error_.empty();
}

const std::string& Client::configuration_error() const {
    return configuration_error_;
}

EntityListResult Client::list_entities() const {
    if (!configured()) {
        return {.ok = false, .entities = {}, .message = configuration_error_};
    }

    const HttpResult result = http_request_json("GET", base_url_ + "/api/states", token_, nullptr);
    const json::ParseResult parsed = parse_http_json(result);
    if (!parsed.ok) {
        return {.ok = false, .entities = {}, .message = parsed.error};
    }

    const json::Value::Array* states = parsed.value.as_array_if();
    if (states == nullptr) {
        return {.ok = false, .entities = {}, .message = "HA states payload was not an array"};
    }

    std::vector<EntityState> entities;
    entities.reserve(states->size());
    for (const json::Value& entry : *states) {
        const std::string entity_id = string_or(entry.get("entity_id"));
        if (!entity_id.starts_with("light.") &&
            !entity_id.starts_with("switch.") &&
            !entity_id.starts_with("climate.") &&
            !entity_id.starts_with("sensor.")) {
            continue;
        }

        EntityState entity = parse_entity_state(entry);
        if (entity.ok) {
            entities.push_back(std::move(entity));
        }
    }

    std::sort(entities.begin(), entities.end(), [](const EntityState& left, const EntityState& right) {
        if (left.kind != right.kind) {
            return static_cast<int>(left.kind) < static_cast<int>(right.kind);
        }
        return lowercase_ascii(left.friendly_name) < lowercase_ascii(right.friendly_name);
    });

    return {.ok = true, .entities = std::move(entities), .message = "Entities listed"};
}

EntityState Client::fetch_entity_state(const std::string& entity_id) const {
    if (!configured()) {
        EntityState entity;
        entity.ok = false;
        entity.entity_id = entity_id;
        entity.message = configuration_error_;
        return entity;
    }

    const HttpResult result = http_request_json("GET", base_url_ + "/api/states/" + entity_id, token_, nullptr);
    const json::ParseResult parsed = parse_http_json(result);
    if (!parsed.ok) {
        EntityState entity;
        entity.ok = false;
        entity.entity_id = entity_id;
        entity.message = parsed.error;
        return entity;
    }

    EntityState entity = parse_entity_state(parsed.value);
    if (!entity.ok) {
        entity.entity_id = entity_id;
        entity.message = entity.message.empty() ? "Invalid entity response" : entity.message;
    }
    return entity;
}

LightListResult Client::list_lights() const {
    const EntityListResult entities = list_entities();
    if (!entities.ok) {
        return {.ok = false, .lights = {}, .message = entities.message};
    }
    std::vector<LightState> lights;
    lights.reserve(entities.entities.size());
    for (const EntityState& entity : entities.entities) {
        if (entity.kind != EntityKind::Light) {
            continue;
        }
        lights.push_back({
            .ok = entity.ok,
            .entity_id = entity.entity_id,
            .friendly_name = entity.friendly_name,
            .state = entity.state,
            .message = entity.message,
            .available = entity.available,
            .supports_brightness = entity.supports_brightness,
            .supports_color_temp = entity.supports_color_temp,
            .supports_rgb = entity.supports_rgb,
            .brightness_percent = entity.brightness_percent,
            .color_temp_kelvin = entity.color_temp_kelvin,
            .min_color_temp_kelvin = entity.min_color_temp_kelvin,
            .max_color_temp_kelvin = entity.max_color_temp_kelvin,
            .rgb_red = entity.rgb_red,
            .rgb_green = entity.rgb_green,
            .rgb_blue = entity.rgb_blue,
        });
    }
    return {.ok = true, .lights = std::move(lights), .message = "Lights listed"};
}

LightState Client::fetch_light_state(const std::string& entity_id) const {
    const EntityState entity = fetch_entity_state(entity_id);
    if (!entity.ok || entity.kind != EntityKind::Light) {
        return {.ok = false, .entity_id = entity_id, .friendly_name = {}, .state = {}, .message = entity.message.empty() ? "Invalid light response" : entity.message};
    }
    return {
        .ok = entity.ok,
        .entity_id = entity.entity_id,
        .friendly_name = entity.friendly_name,
        .state = entity.state,
        .message = entity.message,
        .available = entity.available,
        .supports_brightness = entity.supports_brightness,
        .supports_color_temp = entity.supports_color_temp,
        .supports_rgb = entity.supports_rgb,
        .brightness_percent = entity.brightness_percent,
        .color_temp_kelvin = entity.color_temp_kelvin,
        .min_color_temp_kelvin = entity.min_color_temp_kelvin,
        .max_color_temp_kelvin = entity.max_color_temp_kelvin,
        .rgb_red = entity.rgb_red,
        .rgb_green = entity.rgb_green,
        .rgb_blue = entity.rgb_blue,
    };
}

SensorHistoryResult Client::fetch_sensor_history(const std::string& entity_id) const {
    if (!configured()) {
        return {.ok = false, .entity_id = entity_id, .values = {}, .min_value = 0.0, .max_value = 0.0, .message = configuration_error_};
    }
    if (!entity_id.starts_with("sensor.")) {
        return {.ok = false, .entity_id = entity_id, .values = {}, .min_value = 0.0, .max_value = 0.0, .message = "History is only supported for sensor entities"};
    }

    const HttpResult result = http_request_json("GET",
                                                base_url_ + "/api/history/period?filter_entity_id=" + entity_id + "&minimal_response&no_attributes",
                                                token_,
                                                nullptr);
    const json::ParseResult parsed = parse_http_json(result);
    if (!parsed.ok) {
        return {.ok = false, .entity_id = entity_id, .values = {}, .min_value = 0.0, .max_value = 0.0, .message = parsed.error};
    }

    const json::Value::Array* history_groups = parsed.value.as_array_if();
    if (history_groups == nullptr) {
        return {.ok = false, .entity_id = entity_id, .values = {}, .min_value = 0.0, .max_value = 0.0, .message = "History payload was not an array"};
    }

    const json::Value::Array* history = nullptr;
    for (const json::Value& group : *history_groups) {
        const json::Value::Array* series = group.as_array_if();
        if (series != nullptr) {
            history = series;
            break;
        }
    }
    if (history == nullptr) {
        return {.ok = false, .entity_id = entity_id, .values = {}, .min_value = 0.0, .max_value = 0.0, .message = "No history series returned"};
    }

    SensorHistoryResult sensor_history{
        .ok = true,
        .entity_id = entity_id,
        .values = {},
        .min_value = 0.0,
        .max_value = 0.0,
        .message = "Sensor history fetched",
    };

    bool have_numeric_sample = false;
    for (const json::Value& point : *history) {
        double value = 0.0;
        if (!parse_double_string(trim(string_or(point.get("state"))), value)) {
            continue;
        }
        sensor_history.values.push_back(value);
        if (!have_numeric_sample) {
            sensor_history.min_value = value;
            sensor_history.max_value = value;
            have_numeric_sample = true;
            continue;
        }
        sensor_history.min_value = std::min(sensor_history.min_value, value);
        sensor_history.max_value = std::max(sensor_history.max_value, value);
    }

    if (!have_numeric_sample) {
        return {.ok = false, .entity_id = entity_id, .values = {}, .min_value = 0.0, .max_value = 0.0, .message = "No numeric sensor history returned"};
    }

    return sensor_history;
}

Result Client::toggle_light(const std::string& entity_id) const {
    if (!configured()) {
        return {.ok = false, .message = configuration_error_};
    }

    return post_service(base_url_,
                        token_,
                        "light/toggle",
                        {{"entity_id", entity_id}});
}

Result Client::toggle_switch(const std::string& entity_id) const {
    if (!configured()) {
        return {.ok = false, .message = configuration_error_};
    }

    return post_service(base_url_,
                        token_,
                        "switch/toggle",
                        {{"entity_id", entity_id}});
}

Result Client::set_light_brightness(const std::string& entity_id, int brightness_percent) const {
    if (!configured()) {
        return {.ok = false, .message = configuration_error_};
    }

    return post_service(base_url_,
                        token_,
                        "light/turn_on",
                        {
                            {"entity_id", entity_id},
                            {"brightness_pct", std::clamp(brightness_percent, 0, 100)},
                        });
}

Result Client::set_light_color_temperature(const std::string& entity_id, int kelvin) const {
    if (!configured()) {
        return {.ok = false, .message = configuration_error_};
    }

    return post_service(base_url_,
                        token_,
                        "light/turn_on",
                        {
                            {"entity_id", entity_id},
                            {"color_temp_kelvin", kelvin},
                        });
}

Result Client::set_light_rgb(const std::string& entity_id, int red, int green, int blue) const {
    if (!configured()) {
        return {.ok = false, .message = configuration_error_};
    }

    return post_service(base_url_,
                        token_,
                        "light/turn_on",
                        {
                            {"entity_id", entity_id},
                            {"rgb_color", json::Value::Array{
                                std::clamp(red, 0, 255),
                                std::clamp(green, 0, 255),
                                std::clamp(blue, 0, 255),
                            }},
                        });
}

Result Client::set_climate_hvac_mode(const std::string& entity_id, const std::string& hvac_mode) const {
    if (!configured()) {
        return {.ok = false, .message = configuration_error_};
    }

    return post_service(base_url_,
                        token_,
                        "climate/set_hvac_mode",
                        {
                            {"entity_id", entity_id},
                            {"hvac_mode", hvac_mode},
                        });
}

WeatherState Client::fetch_weather_state() const {
    if (!configured()) {
        return {.ok = false, .condition = {}, .temperature_high = 0, .temperature_low = 0, .temperature_unit = "C", .message = configuration_error_};
    }

    if (weather_entity_id_.empty()) {
        weather_entity_id_ = std::string{kDefaultWeatherEntityId};
        const HttpResult default_result = http_request_json("GET",
                                                            base_url_ + "/api/states/" + weather_entity_id_,
                                                            token_,
                                                            nullptr);
        if (!default_result.ok) {
            const HttpResult states_result = http_request_json("GET", base_url_ + "/api/states", token_, nullptr);
            const json::ParseResult states_json = parse_http_json(states_result);
            if (states_json.ok) {
                find_first_weather_entity(states_json.value, weather_entity_id_);
            }
        }
    }

    if (weather_entity_id_.empty()) {
        return {.ok = false, .condition = {}, .temperature_high = 0, .temperature_low = 0, .temperature_unit = "C", .message = "No weather entity found"};
    }

    const HttpResult state_result = http_request_json("GET",
                                                      base_url_ + "/api/states/" + weather_entity_id_,
                                                      token_,
                                                      nullptr);
    const json::ParseResult parsed_state = parse_http_json(state_result);
    if (!parsed_state.ok) {
        return {.ok = false, .condition = {}, .temperature_high = 0, .temperature_low = 0, .temperature_unit = "C", .message = parsed_state.error};
    }

    WeatherState weather;
    weather.condition = lowercase_ascii(string_or(parsed_state.value.get("state")));
    weather.temperature_unit = weather_unit_from_state(parsed_state.value);

    const std::string payload = json::stringify(json::Value(json::Value::Object{
        {"entity_id", weather_entity_id_},
        {"type", "daily"},
    }));
    const HttpResult forecast_result = http_request_json("POST",
                                                         base_url_ + "/api/services/weather/get_forecasts?return_response",
                                                         token_,
                                                         &payload);
    const json::ParseResult parsed_forecast = parse_http_json(forecast_result);
    if (parsed_forecast.ok) {
        const json::Value* payload_value = first_forecast_payload(parsed_forecast.value);
        const json::Value::Array* forecast = payload_value != nullptr ? array_if(payload_value->get("forecast")) : nullptr;
        if (forecast != nullptr && !forecast->empty()) {
            int high = 0;
            int low = 0;
            const json::Value& first_day = forecast->front();
            if (number_as_int(first_day.get("temperature"), high)) {
                weather.temperature_high = high;
            }
            if (number_as_int(first_day.get("templow"), low)) {
                weather.temperature_low = low;
            }
        }
    }

    if (weather.temperature_high == 0 && weather.temperature_low == 0) {
        int current_temp = 0;
        const json::Value* attributes = parsed_state.value.get("attributes");
        if (number_as_int(attributes != nullptr ? attributes->get("temperature") : nullptr, current_temp)) {
            weather.temperature_high = current_temp;
            weather.temperature_low = current_temp;
        }
    }

    if (weather.condition.empty()) {
        return {.ok = false, .condition = {}, .temperature_high = 0, .temperature_low = 0, .temperature_unit = weather.temperature_unit, .message = "Invalid weather response"};
    }

    weather.ok = true;
    weather.message = "Weather refreshed";
    return weather;
}

}  // namespace ha
