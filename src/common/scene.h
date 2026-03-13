#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hadisplay {

struct Rect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

enum class ButtonId {
    BrightnessToggle = 0,
    DevModeToggle,
    SetupToggleLight,
    SetupPreviousPage,
    SetupNextPage,
    SetupSave,
    SetupRefresh,
    DashboardToggleLight,
    DashboardOpenDetail,
    DashboardPreviousPage,
    DashboardNextPage,
    DashboardConfigure,
    DashboardRefresh,
    DetailBack,
    DetailToggleLight,
    DetailBrightnessDown,
    DetailBrightnessUp,
    DetailSetRed,
    DetailSetGreen,
    DetailSetBlue,
    DetailSetDaylight,
    DetailSetNeutral,
    DetailSetWarm,
    Exit,
};

struct Button {
    ButtonId id = ButtonId::Exit;
    std::string label;
    Rect rect;
    int value = -1;
};

enum class ViewMode {
    Setup = 0,
    Dashboard,
    Detail,
};

enum class DisplayMode {
    Auto = 0,
    Grayscale,
    Color,
};

enum class PixelFormat {
    Gray8 = 0,
    RGBA32,
};

struct Color {
    std::uint8_t r = 0U;
    std::uint8_t g = 0U;
    std::uint8_t b = 0U;
    std::uint8_t a = 0xFFU;
};

struct RenderBuffer {
    PixelFormat format = PixelFormat::Gray8;
    int width = 0;
    int height = 0;
    std::vector<unsigned char> pixels;

    [[nodiscard]] std::size_t bytes_per_pixel() const {
        return format == PixelFormat::RGBA32 ? 4U : 1U;
    }

    [[nodiscard]] std::size_t byte_count() const {
        return pixels.size();
    }

    [[nodiscard]] unsigned char* data() {
        return pixels.data();
    }

    [[nodiscard]] const unsigned char* data() const {
        return pixels.data();
    }
};

enum class EntityKind {
    Light = 0,
    Switch,
    Climate,
};

struct EntityItem {
    EntityKind kind = EntityKind::Light;
    std::string entity_id;
    std::string name;
    std::string kind_label = "LIGHT";
    std::string state_label = "UNKNOWN";
    bool is_on = false;
    bool available = false;
    bool selected = false;
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
};

struct SceneState {
    int width = 1024;
    int height = 768;
    int pressed_button = -1;
    int selected_button = -1;
    ViewMode view_mode = ViewMode::Setup;
    std::vector<EntityItem> entities;
    int setup_page = 0;
    int dashboard_page = 0;
    int detail_entity_index = -1;
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
    bool dev_mode = false;
    bool weather_available = false;
    std::string weather_condition = "cloudy";
    std::string weather_range_label = "--/--";
    std::string status = "STARTING";
};

std::vector<Button> buttons_for(const SceneState& state);
int button_at(const std::vector<Button>& buttons, int x, int y);
RenderBuffer render_scene(const SceneState& state, const std::vector<Button>& buttons, PixelFormat pixel_format);

}  // namespace hadisplay
