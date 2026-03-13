#include "app_config.h"

#include "json.h"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <unistd.h>

namespace hadisplay {
namespace {

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
        std::filesystem::current_path() / "hadisplay-config.json",
        std::filesystem::current_path() / ".hadisplay-config.json",
        executable_dir() / "hadisplay-config.json",
        executable_dir() / ".hadisplay-config.json",
        executable_dir().parent_path() / "hadisplay-config.json",
        executable_dir().parent_path() / ".hadisplay-config.json",
    };

    for (const auto& candidate : existing_candidates) {
        if (!candidate.empty() && std::filesystem::exists(candidate)) {
            return candidate;
        }
    }

    return std::filesystem::current_path() / "hadisplay-config.json";
}

}  // namespace

ConfigStore::ConfigStore() : path_(resolve_config_path().string()) {}

ConfigLoadResult ConfigStore::load() const {
    ConfigLoadResult result;
    result.ok = true;
    result.path = path_;

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
            result.config.ha_url = *value;
        }
    }
    if (const json::Value* ha_token = parsed.value.get("ha_token")) {
        if (const std::string* value = ha_token->as_string_if()) {
            result.config.ha_token = *value;
        }
    }
    if (const json::Value* ha_weather_entity = parsed.value.get("ha_weather_entity")) {
        if (const std::string* value = ha_weather_entity->as_string_if()) {
            result.config.ha_weather_entity = *value;
        }
    }
    result.config.display_mode = parse_display_mode(parsed.value.get("display_mode"));

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
    std::set<std::string> deduped;
    for (const std::string& id : config.selected_entity_ids) {
        if (!id.empty() && deduped.insert(id).second) {
            ids.emplace_back(id);
        }
    }

    json::Value::Object root_object{
        {"selected_entity_ids", json::Value(std::move(ids))},
    };
    if (!config.ha_url.empty()) {
        root_object["ha_url"] = json::Value(config.ha_url);
    }
    if (!config.ha_token.empty()) {
        root_object["ha_token"] = json::Value(config.ha_token);
    }
    if (!config.ha_weather_entity.empty()) {
        root_object["ha_weather_entity"] = json::Value(config.ha_weather_entity);
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

}  // namespace hadisplay
