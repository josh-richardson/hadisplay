#include "scene.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <fbink.h>
#include <linux/fb.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

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

hadisplay::SceneState sample_scene_state(int width, int height, bool color_mode) {
    hadisplay::SceneState state{};
    state.width = width;
    state.height = height;
    state.view_mode = hadisplay::ViewMode::Dashboard;
    state.time_label = "19:58";
    state.date_label = "FRI 13 MAR";
    state.weather_available = true;
    state.weather_condition = "sunny";
    state.weather_range_label = "6/12C";
    state.wifi_connected = true;
    state.wifi_label = "ON";
    state.battery_available = true;
    state.battery_charging = true;
    state.battery_percent = 84;
    state.battery_label = "84%";
    state.brightness_available = true;
    state.brightness_percent = 60;
    state.brightness_label = "60%";
    state.status = color_mode ? "MIRROR COLOR MODE" : "MIRROR GRAYSCALE MODE";
    state.entities = {
        {
            .kind = hadisplay::EntityKind::Light,
            .entity_id = "light.office",
            .name = "Office Lamp",
            .kind_label = "LIGHT",
            .state_label = "ON",
            .is_on = true,
            .available = true,
            .selected = true,
            .supports_detail = true,
            .supports_brightness = true,
            .supports_color_temp = true,
            .supports_rgb = true,
            .supports_history = false,
            .has_numeric_value = false,
            .brightness_percent = 72,
            .numeric_value = 0.0,
            .device_class = {},
            .unit_label = {},
            .room_label = "Office",
            .hvac_action = {},
        },
        {
            .kind = hadisplay::EntityKind::Switch,
            .entity_id = "switch.speakers",
            .name = "Speakers",
            .kind_label = "SOCKET",
            .state_label = "OFF",
            .available = true,
            .selected = true,
            .supports_history = false,
            .has_numeric_value = false,
            .numeric_value = 0.0,
            .device_class = {},
            .unit_label = {},
            .room_label = "Living Room",
            .hvac_action = {},
        },
        {
            .kind = hadisplay::EntityKind::Climate,
            .entity_id = "climate.hallway",
            .name = "Hallway Heat",
            .kind_label = "THERMOSTAT",
            .state_label = "HEATING",
            .is_on = true,
            .available = true,
            .selected = true,
            .supports_detail = true,
            .supports_heat_control = true,
            .supports_history = false,
            .has_numeric_value = false,
            .current_temperature = 20,
            .target_temperature = 22,
            .numeric_value = 0.0,
            .device_class = {},
            .unit_label = {},
            .room_label = "Hallway",
            .hvac_action = "heating",
        },
    };
    return state;
}

bool render_and_present(int fbfd,
                        const FBInkState& fb_state,
                        bool color_mode,
                        Display* display,
                        int screen,
                        Window window,
                        GC gc) {
    const hadisplay::SceneState scene_state = sample_scene_state(std::min(1024, static_cast<int>(fb_state.view_width)),
                                                                 std::min(768, static_cast<int>(fb_state.view_height)),
                                                                 color_mode);
    const auto buttons = hadisplay::buttons_for(scene_state);
    const auto buffer = hadisplay::render_scene(scene_state,
                                                buttons,
                                                color_mode ? hadisplay::PixelFormat::RGBA32 : hadisplay::PixelFormat::Gray8);

    FBInkConfig draw_cfg{};
    draw_cfg.is_quiet = true;
    draw_cfg.no_refresh = true;
    if (color_mode) {
        draw_cfg.wfm_mode = WFM_GCC16;
    }

    if (fbink_print_raw_data(fbfd,
                             buffer.pixels.data(),
                             scene_state.width,
                             scene_state.height,
                             buffer.pixels.size(),
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
    bool color_mode = fb_state.has_color_panel;

    Display* display = XOpenDisplay(nullptr);
    if (display == nullptr) {
        std::cerr << "Unable to open X11 display.\n";
        fbink_close(fbfd);
        return EXIT_FAILURE;
    }

    const int screen = DefaultScreen(display);
    const Window root = RootWindow(display, screen);
    const int window_width = std::min(1024, static_cast<int>(fb_state.view_width));
    const int window_height = std::min(768, static_cast<int>(fb_state.view_height));

    XSetWindowAttributes attrs{};
    attrs.background_pixel = WhitePixel(display, screen);
    attrs.event_mask = ExposureMask | KeyPressMask | StructureNotifyMask;

    Window window = XCreateWindow(display,
                                  root,
                                  80,
                                  80,
                                  static_cast<unsigned int>(window_width),
                                  static_cast<unsigned int>(window_height),
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
    if (!render_and_present(fbfd, fb_state, color_mode, display, screen, window, gc)) {
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
                    render_and_present(fbfd, fb_state, color_mode, display, screen, window, gc);
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
                } else if ((key == XK_c || key == XK_C) && fb_state.has_color_panel) {
                    color_mode = !color_mode;
                    render_and_present(fbfd, fb_state, color_mode, display, screen, window, gc);
                }
                break;
            }
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
