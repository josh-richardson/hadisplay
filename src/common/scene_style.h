#pragma once

#include "scene.h"

namespace hadisplay::scene {

struct Theme {
    Color white;
    Color light;
    Color mid;
    Color dark;
    Color highlight;
    Color warning;
    Color accent_red;
    Color accent_green;
    Color accent_blue;
    Color accent_yellow;
    Color accent_orange;
};

constexpr Theme grayscale_theme() {
    return Theme{
        .white = {0xFFu, 0xFFu, 0xFFu, 0xFFu},
        .light = {0xDDu, 0xDDu, 0xDDu, 0xFFu},
        .mid = {0x77u, 0x77u, 0x77u, 0xFFu},
        .dark = {0x11u, 0x11u, 0x11u, 0xFFu},
        .highlight = {0xDDu, 0xDDu, 0xDDu, 0xFFu},
        .warning = {0xBBu, 0xBBu, 0xBBu, 0xFFu},
        .accent_red = {0x55u, 0x55u, 0x55u, 0xFFu},
        .accent_green = {0x55u, 0x55u, 0x55u, 0xFFu},
        .accent_blue = {0x55u, 0x55u, 0x55u, 0xFFu},
        .accent_yellow = {0x99u, 0x99u, 0x99u, 0xFFu},
        .accent_orange = {0x77u, 0x77u, 0x77u, 0xFFu},
    };
}

constexpr Theme color_theme() {
    return Theme{
        .white = {0xFAu, 0xF7u, 0xEEu, 0xFFu},
        .light = {0xE8u, 0xE2u, 0xD2u, 0xFFu},
        .mid = {0x7Bu, 0x74u, 0x66u, 0xFFu},
        .dark = {0x1Fu, 0x1Bu, 0x17u, 0xFFu},
        .highlight = {0xD8u, 0xEAu, 0xD3u, 0xFFu},
        .warning = {0xF3u, 0xE6u, 0xBFu, 0xFFu},
        .accent_red = {0xB8u, 0x37u, 0x2Fu, 0xFFu},
        .accent_green = {0x3Du, 0x7Eu, 0x48u, 0xFFu},
        .accent_blue = {0x2Eu, 0x67u, 0x9Bu, 0xFFu},
        .accent_yellow = {0xD5u, 0xA6u, 0x2Au, 0xFFu},
        .accent_orange = {0xBEu, 0x74u, 0x2Du, 0xFFu},
    };
}

inline const Theme& theme_for(PixelFormat pixel_format) {
    static constexpr Theme kGray = grayscale_theme();
    static constexpr Theme kColor = color_theme();
    return pixel_format == PixelFormat::RGBA32 ? kColor : kGray;
}

inline Theme& active_theme_storage() {
    static Theme theme = grayscale_theme();
    return theme;
}

inline void set_active_theme(const Theme& theme) {
    active_theme_storage() = theme;
}

inline const Theme& active_theme() {
    return active_theme_storage();
}

}  // namespace hadisplay::scene
