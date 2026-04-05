#include "app_config.h"

#include "json.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <unistd.h>

namespace hadisplay {
namespace {

std::string lowercase_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
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

std::string trim_trailing_slashes(std::string value) {
    while (!value.empty() && value.back() == '/') {
        value.pop_back();
    }
    return value;
}

std::vector<std::string> normalize_patterns(const std::vector<std::string>& patterns) {
    std::vector<std::string> normalized;
    std::set<std::string> deduped;
    for (const std::string& pattern : patterns) {
        const std::string candidate = lowercase_ascii(trim(pattern));
        if (!candidate.empty() && deduped.insert(candidate).second) {
            normalized.push_back(candidate);
        }
    }
    return normalized;
}

std::vector<std::string> normalize_ssids(const std::vector<std::string>& ssids) {
    std::vector<std::string> normalized;
    std::set<std::string> deduped;
    for (const std::string& ssid : ssids) {
        const std::string candidate = trim(ssid);
        if (!candidate.empty() && deduped.insert(candidate).second) {
            normalized.push_back(candidate);
        }
    }
    return normalized;
}

DisplayMode parse_display_mode(const json::Value* value) {
    const std::string* mode = value != nullptr ? value->as_string_if() : nullptr;
    if (mode == nullptr) {
        return DisplayMode::Auto;
    }

    if (*mode == "grayscale") {
        return DisplayMode::Grayscale;
    }
    if (*mode == "color") {
        return DisplayMode::Color;
    }
    return DisplayMode::Auto;
}

std::string display_mode_name(DisplayMode mode) {
    switch (mode) {
        case DisplayMode::Grayscale: return "grayscale";
        case DisplayMode::Color: return "color";
        case DisplayMode::Auto: return "auto";
    }
    return "auto";
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

std::filesystem::path resolve_config_path() {
    if (const char* override_path = std::getenv("HADISPLAY_CONFIG_FILE")) {
        return std::filesystem::path(override_path);
    }

    const std::array<std::filesystem::path, 6> existing_candidates{
        std::filesystem::current_path() / ".hadisplay-config.json",
        std::filesystem::current_path() / "hadisplay-config.json",
        executable_dir() / ".hadisplay-config.json",
        executable_dir() / "hadisplay-config.json",
        executable_dir().parent_path() / ".hadisplay-config.json",
        executable_dir().parent_path() / "hadisplay-config.json",
    };

    for (const auto& candidate : existing_candidates) {
        if (!candidate.empty() && std::filesystem::exists(candidate)) {
            return candidate;
        }
    }

    return std::filesystem::current_path() / "hadisplay-config.json";
}

std::vector<std::string> parse_string_array(const json::Value* value, const char* field_name, std::string& error) {
    std::vector<std::string> parsed_values;
    if (value == nullptr) {
        return parsed_values;
    }

    const json::Value::Array* array = value->as_array_if();
    if (array == nullptr) {
        error = std::string(field_name) + " must be an array";
        return {};
    }

    parsed_values.reserve(array->size());
    for (const json::Value& entry : *array) {
        const std::string* string_value = entry.as_string_if();
        if (string_value != nullptr) {
            parsed_values.push_back(*string_value);
        }
    }
    return parsed_values;
}

bool parse_location(const json::Value& value, HaLocationConfig& location, std::string& error) {
    const json::Value::Object* object = value.as_object_if();
    if (object == nullptr) {
        error = "locations entries must be objects";
        return false;
    }

    if (const json::Value* id = value.get("id")) {
        if (const std::string* string_value = id->as_string_if()) {
            location.id = trim(*string_value);
        }
    }
    if (const json::Value* name = value.get("name")) {
        if (const std::string* string_value = name->as_string_if()) {
            location.name = trim(*string_value);
        }
    }
    if (const json::Value* match_ssids = value.get("match_ssids")) {
        location.match_ssids = normalize_ssids(parse_string_array(match_ssids, "match_ssids", error));
        if (!error.empty()) {
            return false;
        }
    } else if (const json::Value* match_ssid = value.get("match_ssid")) {
        if (const std::string* string_value = match_ssid->as_string_if()) {
            location.match_ssids = normalize_ssids({*string_value});
        }
    }
    if (const json::Value* ha_url = value.get("ha_url")) {
        if (const std::string* string_value = ha_url->as_string_if()) {
            location.ha_url = trim_trailing_slashes(trim(*string_value));
        }
    }
    if (const json::Value* ha_token = value.get("ha_token")) {
        if (const std::string* string_value = ha_token->as_string_if()) {
            location.ha_token = trim(*string_value);
        }
    }
    if (const json::Value* ha_weather_entity = value.get("ha_weather_entity")) {
        if (const std::string* string_value = ha_weather_entity->as_string_if()) {
            location.ha_weather_entity = trim(*string_value);
        }
    }
    if (const json::Value* is_default = value.get("default")) {
        if (const bool* bool_value = is_default->as_bool_if()) {
            location.is_default = *bool_value;
        }
    }

    if (location.id.empty()) {
        error = "locations entries must include an id";
        return false;
    }
    return true;
}

const HaLocationConfig* find_location_by_id(const AppConfig& config, const std::string& location_id) {
    for (const HaLocationConfig& location : config.locations) {
        if (location.id == location_id) {
            return &location;
        }
    }
    return nullptr;
}

const HaLocationConfig* find_location_for_ssid(const AppConfig& config, const std::string& ssid) {
    if (ssid.empty()) {
        return nullptr;
    }

    for (const HaLocationConfig& location : config.locations) {
        for (const std::string& match_ssid : location.match_ssids) {
            if (match_ssid == ssid) {
                return &location;
            }
        }
    }
    return nullptr;
}

const HaLocationConfig* find_default_location(const AppConfig& config) {
    if (!config.default_location_id.empty()) {
        if (const HaLocationConfig* by_id = find_location_by_id(config, config.default_location_id)) {
            return by_id;
        }
    }

    for (const HaLocationConfig& location : config.locations) {
        if (location.is_default) {
            return &location;
        }
    }
    return nullptr;
}

json::Value location_to_json(const HaLocationConfig& location) {
    json::Value::Object object{
        {"id", json::Value(location.id)},
    };
    if (!location.name.empty()) {
        object["name"] = json::Value(location.name);
    }
    if (!location.match_ssids.empty()) {
        json::Value::Array match_ssids;
        match_ssids.reserve(location.match_ssids.size());
        for (const std::string& ssid : normalize_ssids(location.match_ssids)) {
            match_ssids.emplace_back(ssid);
        }
        object["match_ssids"] = json::Value(std::move(match_ssids));
    }
    if (!location.ha_url.empty()) {
        object["ha_url"] = json::Value(trim_trailing_slashes(location.ha_url));
    }
    if (!location.ha_token.empty()) {
        object["ha_token"] = json::Value(location.ha_token);
    }
    if (!location.ha_weather_entity.empty()) {
        object["ha_weather_entity"] = json::Value(location.ha_weather_entity);
    }
    if (location.is_default) {
        object["default"] = json::Value(true);
    }
    return json::Value(std::move(object));
}

}  // namespace

ConfigStore::ConfigStore() : path_(resolve_config_path().string()) {}

std::vector<std::string> default_hidden_entity_patterns() {
    return {
        "child lock",
        "child_lock",
        "childlock",
        "child-lock",
        "lock child",
        "parental lock",
        "button lock",
    };
}

ConfigLoadResult ConfigStore::load() const {
    ConfigLoadResult result;
    result.ok = true;
    result.path = path_;
    result.config.hidden_entity_patterns = default_hidden_entity_patterns();

    if (path_.empty()) {
        result.ok = false;
        result.message = "No config path available";
        return result;
    }

    std::ifstream input(path_);
    if (!input) {
        if (errno == ENOENT) {
            result.found = false;
            result.message = "No config file";
            return result;
        }
        result.ok = false;
        result.message = "Unable to open config: " + std::string(std::strerror(errno));
        return result;
    }

    result.found = true;
    std::string body((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const json::ParseResult parsed = json::parse(body);
    if (!parsed.ok) {
        result.ok = false;
        result.message = "Invalid config JSON: " + parsed.error;
        return result;
    }

    const json::Value::Object* root = parsed.value.as_object_if();
    if (root == nullptr) {
        result.ok = false;
        result.message = "Config root must be an object";
        return result;
    }

    if (const json::Value* ha_url = parsed.value.get("ha_url")) {
        if (const std::string* value = ha_url->as_string_if()) {
            result.config.ha_url = trim_trailing_slashes(trim(*value));
        }
    }
    if (const json::Value* ha_token = parsed.value.get("ha_token")) {
        if (const std::string* value = ha_token->as_string_if()) {
            result.config.ha_token = trim(*value);
        }
    }
    if (const json::Value* ha_weather_entity = parsed.value.get("ha_weather_entity")) {
        if (const std::string* value = ha_weather_entity->as_string_if()) {
            result.config.ha_weather_entity = trim(*value);
        }
    }
    if (const json::Value* default_location = parsed.value.get("default_location")) {
        if (const std::string* value = default_location->as_string_if()) {
            result.config.default_location_id = trim(*value);
        }
    }
    result.config.display_mode = parse_display_mode(parsed.value.get("display_mode"));

    if (const json::Value* locations = parsed.value.get("locations")) {
        const json::Value::Array* entries = locations->as_array_if();
        if (entries == nullptr) {
            result.ok = false;
            result.message = "locations must be an array";
            return result;
        }

        std::set<std::string> deduped_location_ids;
        result.config.locations.reserve(entries->size());
        for (const json::Value& entry : *entries) {
            HaLocationConfig location;
            std::string error;
            if (!parse_location(entry, location, error)) {
                result.ok = false;
                result.message = error;
                return result;
            }
            if (!deduped_location_ids.insert(location.id).second) {
                result.ok = false;
                result.message = "Duplicate location id: " + location.id;
                return result;
            }
            result.config.locations.push_back(std::move(location));
        }
    }

    if (const json::Value* hidden_patterns = parsed.value.get("hidden_entity_patterns")) {
        const json::Value::Array* patterns = hidden_patterns->as_array_if();
        if (patterns == nullptr) {
            result.ok = false;
            result.message = "hidden_entity_patterns must be an array";
            return result;
        }

        std::vector<std::string> configured_patterns;
        configured_patterns.reserve(patterns->size());
        for (const json::Value& entry : *patterns) {
            const std::string* value = entry.as_string_if();
            if (value != nullptr) {
                configured_patterns.push_back(*value);
            }
        }
        result.config.hidden_entity_patterns = normalize_patterns(configured_patterns);
    }

    const json::Value* selected = parsed.value.get("selected_entity_ids");
    if (selected == nullptr) {
        selected = parsed.value.get("selected_light_ids");
    }
    if (selected == nullptr) {
        result.message = "Config loaded";
        return result;
    }

    const json::Value::Array* ids = selected->as_array_if();
    if (ids == nullptr) {
        result.ok = false;
        result.message = "selected_light_ids must be an array";
        return result;
    }

    std::set<std::string> deduped;
    for (const json::Value& entry : *ids) {
        const std::string* id = entry.as_string_if();
        if (id == nullptr || id->empty()) {
            continue;
        }
        if (deduped.insert(*id).second) {
            result.config.selected_entity_ids.push_back(*id);
        }
    }

    result.message = "Config loaded";
    return result;
}

bool ConfigStore::save(const AppConfig& config, std::string& error) const {
    if (path_.empty()) {
        error = "No config path available";
        return false;
    }

    json::Value::Array ids;
    json::Value::Array hidden_patterns;
    std::set<std::string> deduped;
    for (const std::string& id : config.selected_entity_ids) {
        if (!id.empty() && deduped.insert(id).second) {
            ids.emplace_back(id);
        }
    }
    for (const std::string& pattern : normalize_patterns(config.hidden_entity_patterns)) {
        hidden_patterns.emplace_back(pattern);
    }

    json::Value::Object root_object{
        {"selected_entity_ids", json::Value(std::move(ids))},
        {"hidden_entity_patterns", json::Value(std::move(hidden_patterns))},
    };
    if (!config.ha_url.empty()) {
        root_object["ha_url"] = json::Value(trim_trailing_slashes(config.ha_url));
    }
    if (!config.ha_token.empty()) {
        root_object["ha_token"] = json::Value(config.ha_token);
    }
    if (!config.ha_weather_entity.empty()) {
        root_object["ha_weather_entity"] = json::Value(config.ha_weather_entity);
    }
    if (!config.default_location_id.empty()) {
        root_object["default_location"] = json::Value(config.default_location_id);
    }
    if (!config.locations.empty()) {
        json::Value::Array locations;
        std::set<std::string> deduped_location_ids;
        for (const HaLocationConfig& location : config.locations) {
            if (!location.id.empty() && deduped_location_ids.insert(location.id).second) {
                locations.emplace_back(location_to_json(location));
            }
        }
        root_object["locations"] = json::Value(std::move(locations));
    }
    root_object["display_mode"] = json::Value(display_mode_name(config.display_mode));

    json::Value root(std::move(root_object));

    std::ofstream output(path_, std::ios::trunc);
    if (!output) {
        error = "Unable to write config: " + std::string(std::strerror(errno));
        return false;
    }

    output << json::stringify(root) << '\n';
    if (!output) {
        error = "Failed to flush config to disk";
        return false;
    }
    return true;
}

const std::string& ConfigStore::path() const {
    return path_;
}

ResolvedHaConfig resolve_ha_config(const AppConfig& config, const std::string& current_ssid) {
    ResolvedHaConfig resolved;

    if (!config.locations.empty()) {
        const std::string normalized_ssid = trim(current_ssid);
        const HaLocationConfig* location = find_location_for_ssid(config, normalized_ssid);
        if (location != nullptr) {
            resolved.match_reason = normalized_ssid.empty()
                ? "location match"
                : "SSID matched \"" + normalized_ssid + "\"";
        } else {
            location = find_default_location(config);
            if (location != nullptr) {
                resolved.match_reason = "default location";
            }
        }

        if (location == nullptr && config.locations.size() == 1U) {
            location = &config.locations.front();
            resolved.match_reason = "single configured location";
        }

        if (location != nullptr) {
            resolved.base_url = trim_trailing_slashes(location->ha_url);
            resolved.token = location->ha_token;
            resolved.weather_entity_id = location->ha_weather_entity;
            resolved.location_id = location->id;
            resolved.location_name = location->name;
            resolved.ok = !resolved.base_url.empty() && !resolved.token.empty();
            if (!resolved.ok) {
                resolved.error = "Location \"" + location->id + "\" is missing ha_url or ha_token";
            }
            return resolved;
        }

        resolved.error = normalized_ssid.empty()
            ? "No Home Assistant location matched and no default_location is configured"
            : "No Home Assistant location matched SSID \"" + normalized_ssid + "\"";
    }

    resolved.base_url = trim_trailing_slashes(config.ha_url);
    resolved.token = config.ha_token;
    resolved.weather_entity_id = config.ha_weather_entity;
    resolved.ok = !resolved.base_url.empty() && !resolved.token.empty();
    if (!resolved.ok && resolved.error.empty()) {
        resolved.error = "Missing HA URL or token in config JSON";
    }
    return resolved;
}

}  // namespace hadisplay
