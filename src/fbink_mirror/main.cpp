#include "ha_client.h"
#include "scene.h"
#include "system_status.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <fbink.h>
#include <linux/fb.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

std::string uppercase_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return value;
}

std::string concise_ha_error(const std::string& message) {
    const std::string lower = [] (std::string value) {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return value;
    }(message);

    if (lower.find(".env") != std::string::npos || lower.find("ha_url") != std::string::npos ||
        lower.find("ha_token") != std::string::npos) {
        return "CHECK .ENV";
    }
    if (lower.find("timed out") != std::string::npos || lower.find("couldn't connect") != std::string::npos ||
        lower.find("could not resolve host") != std::string::npos || lower.find("failed to connect") != std::string::npos) {
        return "HA UNREACHABLE";
    }
    return "HA REQUEST FAILED";
}

std::string entity_state_label(const std::string& state) {
    if (state == "on") {
        return "ON";
    }
    if (state == "off") {
        return "OFF";
    }
    if (state == "unavailable") {
        return "UNAVAILABLE";
    }
    if (state == "unknown") {
        return "UNKNOWN";
    }
    return uppercase_ascii(state);
}

void apply_entity_state(hadisplay::SceneState& scene_state, const ha::EntityState& entity_state) {
    if (!entity_state.friendly_name.empty()) {
        scene_state.entity_name = entity_state.friendly_name;
    }
    scene_state.entity_state = entity_state_label(entity_state.state);
}

void apply_system_status(hadisplay::SceneState& scene_state, const hadisplay::SystemStatus& status) {
    scene_state.time_label = status.time_label;
    scene_state.date_label = status.date_label;
    scene_state.wifi_label = status.wifi_label;
    scene_state.wifi_connected = status.wifi_connected;
    scene_state.battery_label = status.battery_label;
    scene_state.battery_available = status.battery_available;
    scene_state.battery_percent = status.battery_percent;
    scene_state.battery_charging = status.battery_charging;
    scene_state.brightness_label = status.brightness_label;
    scene_state.brightness_percent = status.brightness_percent;
    scene_state.brightness_available = status.brightness_available;
}

void apply_weather_state(hadisplay::SceneState& scene_state, const ha::WeatherState& weather) {
    scene_state.weather_available = weather.ok;
    scene_state.weather_condition = weather.condition;
    if (weather.ok) {
        scene_state.weather_range_label = std::to_string(weather.temperature_low) + "/" +
                                         std::to_string(weather.temperature_high) +
                                         weather.temperature_unit;
    } else {
        scene_state.weather_range_label = "--/--";
    }
}

unsigned char scale_component(std::uint32_t value, std::uint32_t bits) {
    if (bits == 0U) {
        return 0U;
    }
    const std::uint32_t max_value = (1U << bits) - 1U;
    return static_cast<unsigned char>((value * 255U + (max_value / 2U)) / max_value);
}

unsigned long pack_x11_pixel(const Visual* visual, unsigned char r, unsigned char g, unsigned char b) {
    const auto pack = [](unsigned char channel, unsigned long mask) -> unsigned long {
        if (mask == 0UL) {
            return 0UL;
        }
        unsigned int shift = 0U;
        while (((mask >> shift) & 0x1UL) == 0UL) {
            ++shift;
        }
        unsigned int bits = 0U;
        unsigned long shifted_mask = mask >> shift;
        while ((shifted_mask & 0x1UL) != 0UL) {
            ++bits;
            shifted_mask >>= 1U;
        }
        const unsigned long max_value = (1UL << bits) - 1UL;
        const unsigned long scaled = (static_cast<unsigned long>(channel) * max_value + 127UL) / 255UL;
        return scaled << shift;
    };

    return pack(r, visual->red_mask) | pack(g, visual->green_mask) | pack(b, visual->blue_mask);
}

void decode_pixel(const FBInkDump& dump,
                  const fb_var_screeninfo& var_info,
                  int x,
                  int y,
                  unsigned char& r,
                  unsigned char& g,
                  unsigned char& b) {
    const auto* row = dump.data + (static_cast<std::size_t>(y) * dump.stride);

    if (dump.bpp == 4U) {
        const unsigned char packed = row[x / 2];
        const unsigned char nibble = (x % 2 == 0) ? static_cast<unsigned char>((packed >> 4) & 0x0F)
                                                  : static_cast<unsigned char>(packed & 0x0F);
        r = g = b = static_cast<unsigned char>(nibble * 17U);
        return;
    }

    if (dump.bpp == 8U && var_info.red.length == 0U && var_info.green.length == 0U && var_info.blue.length == 0U) {
        r = g = b = row[x];
        return;
    }

    const int bytes_per_pixel = static_cast<int>(dump.bpp / 8U);
    std::uint32_t pixel = 0U;
    for (int i = 0; i < bytes_per_pixel; ++i) {
        pixel |= static_cast<std::uint32_t>(row[x * bytes_per_pixel + i]) << (8 * i);
    }

    if (var_info.red.length == 0U && var_info.green.length == 0U && var_info.blue.length == 0U) {
        r = g = b = static_cast<unsigned char>(pixel & 0xFFU);
        return;
    }

    r = scale_component((pixel >> var_info.red.offset) & ((1U << var_info.red.length) - 1U), var_info.red.length);
    g = scale_component((pixel >> var_info.green.offset) & ((1U << var_info.green.length) - 1U), var_info.green.length);
    b = scale_component((pixel >> var_info.blue.offset) & ((1U << var_info.blue.length) - 1U), var_info.blue.length);
}

bool present_dump(Display* display, int screen, Window window, GC gc, const FBInkDump& dump, const fb_var_screeninfo& var_info) {
    const int width = dump.area.width;
    const int height = dump.area.height;
    char* image_data = static_cast<char*>(std::malloc(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U));
    if (image_data == nullptr) {
        return false;
    }

    XImage* image = XCreateImage(display,
                                 DefaultVisual(display, screen),
                                 static_cast<unsigned int>(DefaultDepth(display, screen)),
                                 ZPixmap,
                                 0,
                                 image_data,
                                 static_cast<unsigned int>(width),
                                 static_cast<unsigned int>(height),
                                 32,
                                 0);
    if (image == nullptr) {
        std::free(image_data);
        return false;
    }

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            unsigned char r = 0U;
            unsigned char g = 0U;
            unsigned char b = 0U;
            decode_pixel(dump, var_info, x, y, r, g, b);
            XPutPixel(image, x, y, pack_x11_pixel(DefaultVisual(display, screen), r, g, b));
        }
    }

    XPutImage(display, window, gc, image, 0, 0, 0, 0, static_cast<unsigned int>(width), static_cast<unsigned int>(height));
    XFlush(display);
    XDestroyImage(image);
    return true;
}

bool render_and_present(int fbfd,
                        const hadisplay::SceneState& scene_state,
                        const std::vector<hadisplay::Button>& buttons,
                        Display* display,
                        int screen,
                        Window window,
                        GC gc) {
    const auto buffer = hadisplay::render_scene(scene_state, buttons);

    FBInkConfig draw_cfg{};
    draw_cfg.is_quiet = true;
    draw_cfg.no_refresh = true;

    if (fbink_print_raw_data(fbfd,
                             const_cast<unsigned char*>(buffer.data()),
                             scene_state.width,
                             scene_state.height,
                             buffer.size(),
                             0,
                             0,
                             &draw_cfg) < 0) {
        std::cerr << "fbink_print_raw_data failed.\n";
        return false;
    }

    FBInkDump dump{};
    FBInkRect rect{
        .left = 0,
        .top = 0,
        .width = static_cast<unsigned short>(scene_state.width),
        .height = static_cast<unsigned short>(scene_state.height),
    };
    if (fbink_rect_dump(fbfd, &rect, &dump) < 0) {
        std::cerr << "fbink_rect_dump failed.\n";
        return false;
    }

    fb_var_screeninfo var_info{};
    fb_fix_screeninfo fix_info{};
    fbink_get_fb_info(&var_info, &fix_info);

    const bool ok = present_dump(display, screen, window, gc, dump, var_info);
    fbink_free_dump_data(&dump);
    if (!ok) {
        std::cerr << "X11 image presentation failed.\n";
    }
    return ok;
}

}  // namespace

int main() {
    FBInkConfig fbink_cfg{};
    fbink_cfg.is_quiet = true;

    const int fbfd = fbink_open();
    if (fbfd < 0) {
        std::cerr << "fbink_open failed.\n";
        return EXIT_FAILURE;
    }

    if (fbink_init(fbfd, &fbink_cfg) < 0) {
        std::cerr << "fbink_init failed.\n";
        fbink_close(fbfd);
        return EXIT_FAILURE;
    }

    FBInkState fb_state{};
    fbink_get_state(&fbink_cfg, &fb_state);

    hadisplay::SceneState scene_state{};
    scene_state.width = std::min(1024, static_cast<int>(fb_state.view_width));
    scene_state.height = std::min(768, static_cast<int>(fb_state.view_height));
    scene_state.device_name = "HOST FBINK MIRROR";
    hadisplay::DeviceStatus device_status;
    apply_system_status(scene_state, device_status.snapshot());

    ha::Client ha_client;
    apply_weather_state(scene_state, ha_client.fetch_weather_state());
    if (ha_client.configured()) {
        const ha::EntityState entity_state = ha_client.fetch_josh_light_state();
        if (entity_state.ok) {
            apply_entity_state(scene_state, entity_state);
            scene_state.status = "STATE SYNCED";
        } else {
            scene_state.status = concise_ha_error(entity_state.message);
        }
    } else {
        scene_state.entity_state = "CONFIG";
        scene_state.status = "CHECK .ENV";
    }

    const auto buttons = hadisplay::buttons_for(scene_state.width, scene_state.height);

    Display* display = XOpenDisplay(nullptr);
    if (display == nullptr) {
        std::cerr << "Unable to open X11 display.\n";
        fbink_close(fbfd);
        return EXIT_FAILURE;
    }

    const int screen = DefaultScreen(display);
    const Window root = RootWindow(display, screen);

    XSetWindowAttributes attrs{};
    attrs.background_pixel = WhitePixel(display, screen);
    attrs.event_mask = ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask | StructureNotifyMask;

    Window window = XCreateWindow(display,
                                  root,
                                  80,
                                  80,
                                  static_cast<unsigned int>(scene_state.width),
                                  static_cast<unsigned int>(scene_state.height),
                                  0,
                                  CopyFromParent,
                                  InputOutput,
                                  CopyFromParent,
                                  CWBackPixel | CWEventMask,
                                  &attrs);
    XStoreName(display, window, "hadisplay hello_fbink_mirror_x11");
    Atom wm_delete = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, window, &wm_delete, 1);
    XMapWindow(display, window);

    GC gc = XCreateGC(display, window, 0, nullptr);

    if (!render_and_present(fbfd, scene_state, buttons, display, screen, window, gc)) {
        XFreeGC(display, gc);
        XDestroyWindow(display, window);
        XCloseDisplay(display);
        fbink_close(fbfd);
        return EXIT_FAILURE;
    }

    bool running = true;
    while (running) {
        XEvent event{};
        XNextEvent(display, &event);
        switch (event.type) {
            case Expose:
                if (event.xexpose.count == 0) {
                    render_and_present(fbfd, scene_state, buttons, display, screen, window, gc);
                }
                break;
            case ClientMessage:
                if (static_cast<Atom>(event.xclient.data.l[0]) == wm_delete) {
                    running = false;
                }
                break;
            case KeyPress: {
                const KeySym key = XLookupKeysym(&event.xkey, 0);
                if (key == XK_Escape || key == XK_q) {
                    running = false;
                }
                break;
            }
            case ButtonPress:
                if (event.xbutton.button == Button1) {
                    scene_state.pressed_button = hadisplay::button_at(buttons, event.xbutton.x, event.xbutton.y);
                    render_and_present(fbfd, scene_state, buttons, display, screen, window, gc);
                }
                break;
            case ButtonRelease:
                if (event.xbutton.button == Button1) {
                    const int released_on = hadisplay::button_at(buttons, event.xbutton.x, event.xbutton.y);
                    if (scene_state.pressed_button >= 0 && scene_state.pressed_button == released_on) {
                        scene_state.selected_button = released_on;
                        if (buttons[static_cast<std::size_t>(released_on)].id == hadisplay::ButtonId::ToggleLight) {
                            scene_state.status = "TOGGLING LIGHT";
                            scene_state.pressed_button = -1;
                            render_and_present(fbfd, scene_state, buttons, display, screen, window, gc);
                            const auto result = ha_client.toggle_josh_light();
                            if (!result.ok) {
                                scene_state.status = concise_ha_error(result.message);
                            } else {
                                const auto entity_state = ha_client.fetch_josh_light_state();
                                if (entity_state.ok) {
                                    apply_entity_state(scene_state, entity_state);
                                    scene_state.status = "LIGHT UPDATED";
                                } else {
                                    scene_state.status = concise_ha_error(entity_state.message);
                                }
                            }
                        } else if (buttons[static_cast<std::size_t>(released_on)].id == hadisplay::ButtonId::BrightnessToggle) {
                            hadisplay::SystemStatus system_status;
                            if (device_status.cycle_brightness(system_status)) {
                                apply_system_status(scene_state, system_status);
                            }
                        } else if (buttons[static_cast<std::size_t>(released_on)].id == hadisplay::ButtonId::DevModeToggle) {
                            scene_state.dev_mode = !scene_state.dev_mode;
                        } else if (buttons[static_cast<std::size_t>(released_on)].id == hadisplay::ButtonId::RefreshState) {
                            const auto entity_state = ha_client.fetch_josh_light_state();
                            if (entity_state.ok) {
                                apply_entity_state(scene_state, entity_state);
                                scene_state.status = "STATE SYNCED";
                            } else {
                                scene_state.status = concise_ha_error(entity_state.message);
                            }
                        } else if (buttons[static_cast<std::size_t>(released_on)].id == hadisplay::ButtonId::FullRefresh) {
                            scene_state.status = "SCREEN REFRESHED";
                        } else if (buttons[static_cast<std::size_t>(released_on)].id == hadisplay::ButtonId::Exit) {
                            running = false;
                        }
                    }
                    scene_state.pressed_button = -1;
                    render_and_present(fbfd, scene_state, buttons, display, screen, window, gc);
                }
                break;
            default:
                break;
        }
    }

    XFreeGC(display, gc);
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    fbink_close(fbfd);
    return EXIT_SUCCESS;
}
