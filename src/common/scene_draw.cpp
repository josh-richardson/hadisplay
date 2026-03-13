#include "scene_draw.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>

namespace hadisplay::scene {
namespace {

using Glyph = std::array<unsigned char, 7>;

unsigned char grayscale_value(Color color) {
    const unsigned int weighted = static_cast<unsigned int>(color.r) * 54U +
                                  static_cast<unsigned int>(color.g) * 183U +
                                  static_cast<unsigned int>(color.b) * 19U;
    return static_cast<unsigned char>(weighted / 256U);
}

const Glyph& glyph_for(char c) {
    static const Glyph blank{0, 0, 0, 0, 0, 0, 0};
    static const Glyph space{0, 0, 0, 0, 0, 0, 0};
    static const Glyph colon{0, 4, 4, 0, 4, 4, 0};
    static const Glyph dash{0, 0, 0x1F, 0, 0, 0, 0};
    static const Glyph plus{0, 0x04, 0x04, 0x1F, 0x04, 0x04, 0};
    static const Glyph period{0, 0, 0, 0, 0, 0x0C, 0x0C};
    static const Glyph percent{0x19, 0x19, 0x02, 0x04, 0x08, 0x13, 0x13};
    static const Glyph slash{1, 2, 4, 8, 16, 0, 0};

    static const Glyph zero{0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E};
    static const Glyph one{0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E};
    static const Glyph two{0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F};
    static const Glyph three{0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E};
    static const Glyph four{0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02};
    static const Glyph five{0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E};
    static const Glyph six{0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E};
    static const Glyph seven{0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08};
    static const Glyph eight{0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E};
    static const Glyph nine{0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x1C};

    static const Glyph a{0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
    static const Glyph b{0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E};
    static const Glyph c_g{0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E};
    static const Glyph d{0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C};
    static const Glyph e{0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F};
    static const Glyph f{0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10};
    static const Glyph g{0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F};
    static const Glyph h{0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
    static const Glyph i{0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E};
    static const Glyph j{0x01, 0x01, 0x01, 0x01, 0x11, 0x11, 0x0E};
    static const Glyph k{0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11};
    static const Glyph l{0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F};
    static const Glyph m{0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11};
    static const Glyph n{0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11};
    static const Glyph o{0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
    static const Glyph p{0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10};
    static const Glyph q{0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D};
    static const Glyph r{0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11};
    static const Glyph s{0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E};
    static const Glyph t{0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
    static const Glyph u{0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
    static const Glyph v{0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04};
    static const Glyph w{0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A};
    static const Glyph x{0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11};
    static const Glyph y{0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04};
    static const Glyph z{0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F};

    switch (static_cast<unsigned char>(std::toupper(static_cast<unsigned char>(c)))) {
        case ' ': return space;
        case ':': return colon;
        case '-': return dash;
        case '+': return plus;
        case '.': return period;
        case '%': return percent;
        case '/': return slash;
        case '0': return zero;
        case '1': return one;
        case '2': return two;
        case '3': return three;
        case '4': return four;
        case '5': return five;
        case '6': return six;
        case '7': return seven;
        case '8': return eight;
        case '9': return nine;
        case 'A': return a;
        case 'B': return b;
        case 'C': return c_g;
        case 'D': return d;
        case 'E': return e;
        case 'F': return f;
        case 'G': return g;
        case 'H': return h;
        case 'I': return i;
        case 'J': return j;
        case 'K': return k;
        case 'L': return l;
        case 'M': return m;
        case 'N': return n;
        case 'O': return o;
        case 'P': return p;
        case 'Q': return q;
        case 'R': return r;
        case 'S': return s;
        case 'T': return t;
        case 'U': return u;
        case 'V': return v;
        case 'W': return w;
        case 'X': return x;
        case 'Y': return y;
        case 'Z': return z;
        default: return blank;
    }
}

std::size_t pixel_index(const RenderBuffer& buffer, int x, int y) {
    const std::size_t index = static_cast<std::size_t>(y) * static_cast<std::size_t>(buffer.width) + static_cast<std::size_t>(x);
    return buffer.format == PixelFormat::RGBA32 ? index * 4U : index;
}

}  // namespace

bool contains(const Rect& rect, int x, int y) {
    return x >= rect.x && x < (rect.x + rect.width) && y >= rect.y && y < (rect.y + rect.height);
}

void set_pixel(std::vector<unsigned char>& buffer, int width, int height, int x, int y, unsigned char value) {
    if (x < 0 || y < 0 || x >= width || y >= height) {
        return;
    }
    buffer[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)] = value;
}

void fill_rect(std::vector<unsigned char>& buffer, int width, int height, const Rect& rect, unsigned char value) {
    const int x0 = std::max(0, rect.x);
    const int y0 = std::max(0, rect.y);
    const int x1 = std::min(width, rect.x + rect.width);
    const int y1 = std::min(height, rect.y + rect.height);
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            set_pixel(buffer, width, height, x, y, value);
        }
    }
}

void draw_rect(std::vector<unsigned char>& buffer, int width, int height, const Rect& rect, unsigned char value) {
    for (int x = rect.x; x < rect.x + rect.width; ++x) {
        set_pixel(buffer, width, height, x, rect.y, value);
        set_pixel(buffer, width, height, x, rect.y + rect.height - 1, value);
    }
    for (int y = rect.y; y < rect.y + rect.height; ++y) {
        set_pixel(buffer, width, height, rect.x, y, value);
        set_pixel(buffer, width, height, rect.x + rect.width - 1, y, value);
    }
}

Rect inset_rect(const Rect& rect, int inset) {
    return {
        rect.x + inset,
        rect.y + inset,
        std::max(0, rect.width - (inset * 2)),
        std::max(0, rect.height - (inset * 2)),
    };
}

void draw_rect_thick(std::vector<unsigned char>& buffer,
                     int width,
                     int height,
                     const Rect& rect,
                     int thickness,
                     unsigned char value) {
    for (int i = 0; i < thickness; ++i) {
        draw_rect(buffer, width, height, inset_rect(rect, i), value);
    }
}

void draw_text(std::vector<unsigned char>& buffer,
               int width,
               int height,
               int x,
               int y,
               const std::string& text,
               int scale,
               unsigned char value) {
    int cursor_x = x;
    for (const char ch : text) {
        const Glyph& glyph = glyph_for(ch);
        for (int row = 0; row < 7; ++row) {
            for (int col = 0; col < 5; ++col) {
                if ((glyph[static_cast<std::size_t>(row)] >> (4 - col)) & 0x1u) {
                    fill_rect(buffer,
                              width,
                              height,
                              {cursor_x + (col * scale), y + (row * scale), scale, scale},
                              value);
                }
            }
        }
        cursor_x += 6 * scale;
    }
}

void draw_line(std::vector<unsigned char>& buffer,
               int width,
               int height,
               int x0,
               int y0,
               int x1,
               int y1,
               int thickness,
               unsigned char value) {
    const int dx = std::abs(x1 - x0);
    const int sx = x0 < x1 ? 1 : -1;
    const int dy = -std::abs(y1 - y0);
    const int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    while (true) {
        fill_rect(buffer, width, height, {x0 - (thickness / 2), y0 - (thickness / 2), thickness, thickness}, value);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        const int e2 = err * 2;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void draw_arc(std::vector<unsigned char>& buffer,
              int width,
              int height,
              int cx,
              int cy,
              int radius,
              double start_radians,
              double end_radians,
              int thickness,
              unsigned char value) {
    const double sweep = std::max(0.0, end_radians - start_radians);
    const int steps = std::max(12, static_cast<int>(std::ceil(radius * sweep)));

    int prev_x = cx + static_cast<int>(std::lround(std::cos(start_radians) * radius));
    int prev_y = cy + static_cast<int>(std::lround(std::sin(start_radians) * radius));
    for (int i = 1; i <= steps; ++i) {
        const double t = start_radians + (sweep * static_cast<double>(i) / static_cast<double>(steps));
        const int x = cx + static_cast<int>(std::lround(std::cos(t) * radius));
        const int y = cy + static_cast<int>(std::lround(std::sin(t) * radius));
        draw_line(buffer, width, height, prev_x, prev_y, x, y, thickness, value);
        prev_x = x;
        prev_y = y;
    }
}

int text_width(const std::string& text, int scale) {
    if (text.empty()) {
        return 0;
    }
    return static_cast<int>(text.size()) * 6 * scale;
}

std::string uppercase_ascii(const std::string& input) {
    std::string out = input;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return out;
}

std::string fit_text_to_width(const std::string& text, int scale, int max_width) {
    if (text.empty() || max_width <= 0) {
        return {};
    }
    if (text_width(text, scale) <= max_width) {
        return text;
    }
    if (max_width <= text_width("...", scale)) {
        return {};
    }

    std::string trimmed = text;
    while (!trimmed.empty() && text_width(trimmed + "...", scale) > max_width) {
        trimmed.pop_back();
    }
    return trimmed.empty() ? std::string{} : trimmed + "...";
}

void draw_text_centered(std::vector<unsigned char>& buffer,
                        int width,
                        int height,
                        const Rect& rect,
                        int y,
                        const std::string& text,
                        int scale,
                        unsigned char value) {
    const std::string fitted = fit_text_to_width(text, scale, rect.width - (scale * 4));
    const int x = rect.x + std::max(0, (rect.width - text_width(fitted, scale)) / 2);
    draw_text(buffer, width, height, x, y, fitted, scale, value);
}

RenderBuffer make_render_buffer(int width, int height, PixelFormat pixel_format, Color fill) {
    const std::size_t bytes_per_pixel = pixel_format == PixelFormat::RGBA32 ? 4U : 1U;
    RenderBuffer buffer{
        .format = pixel_format,
        .width = width,
        .height = height,
        .pixels = std::vector<unsigned char>(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * bytes_per_pixel, 0U),
    };
    if (pixel_format == PixelFormat::RGBA32) {
        for (std::size_t i = 0; i < buffer.pixels.size(); i += 4U) {
            buffer.pixels[i] = fill.r;
            buffer.pixels[i + 1U] = fill.g;
            buffer.pixels[i + 2U] = fill.b;
            buffer.pixels[i + 3U] = fill.a;
        }
    } else {
        std::fill(buffer.pixels.begin(), buffer.pixels.end(), grayscale_value(fill));
    }
    return buffer;
}

void set_pixel(RenderBuffer& buffer, int width, int height, int x, int y, Color value) {
    if (x < 0 || y < 0 || x >= width || y >= height) {
        return;
    }
    const std::size_t index = pixel_index(buffer, x, y);
    if (buffer.format == PixelFormat::RGBA32) {
        buffer.pixels[index] = value.r;
        buffer.pixels[index + 1U] = value.g;
        buffer.pixels[index + 2U] = value.b;
        buffer.pixels[index + 3U] = value.a;
    } else {
        buffer.pixels[index] = grayscale_value(value);
    }
}

void fill_rect(RenderBuffer& buffer, int width, int height, const Rect& rect, Color value) {
    const int x0 = std::max(0, rect.x);
    const int y0 = std::max(0, rect.y);
    const int x1 = std::min(width, rect.x + rect.width);
    const int y1 = std::min(height, rect.y + rect.height);
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            set_pixel(buffer, width, height, x, y, value);
        }
    }
}

void draw_rect(RenderBuffer& buffer, int width, int height, const Rect& rect, Color value) {
    for (int x = rect.x; x < rect.x + rect.width; ++x) {
        set_pixel(buffer, width, height, x, rect.y, value);
        set_pixel(buffer, width, height, x, rect.y + rect.height - 1, value);
    }
    for (int y = rect.y; y < rect.y + rect.height; ++y) {
        set_pixel(buffer, width, height, rect.x, y, value);
        set_pixel(buffer, width, height, rect.x + rect.width - 1, y, value);
    }
}

void draw_rect_thick(RenderBuffer& buffer,
                     int width,
                     int height,
                     const Rect& rect,
                     int thickness,
                     Color value) {
    for (int i = 0; i < thickness; ++i) {
        draw_rect(buffer, width, height, inset_rect(rect, i), value);
    }
}

void draw_text(RenderBuffer& buffer,
               int width,
               int height,
               int x,
               int y,
               const std::string& text,
               int scale,
               Color value) {
    int cursor_x = x;
    for (const char ch : text) {
        const Glyph& glyph = glyph_for(ch);
        for (int row = 0; row < 7; ++row) {
            for (int col = 0; col < 5; ++col) {
                if ((glyph[static_cast<std::size_t>(row)] >> (4 - col)) & 0x1u) {
                    fill_rect(buffer,
                              width,
                              height,
                              {cursor_x + (col * scale), y + (row * scale), scale, scale},
                              value);
                }
            }
        }
        cursor_x += 6 * scale;
    }
}

void draw_line(RenderBuffer& buffer,
               int width,
               int height,
               int x0,
               int y0,
               int x1,
               int y1,
               int thickness,
               Color value) {
    const int dx = std::abs(x1 - x0);
    const int sx = x0 < x1 ? 1 : -1;
    const int dy = -std::abs(y1 - y0);
    const int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    while (true) {
        fill_rect(buffer, width, height, {x0 - (thickness / 2), y0 - (thickness / 2), thickness, thickness}, value);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        const int e2 = err * 2;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void draw_arc(RenderBuffer& buffer,
              int width,
              int height,
              int cx,
              int cy,
              int radius,
              double start_radians,
              double end_radians,
              int thickness,
              Color value) {
    const double sweep = std::max(0.0, end_radians - start_radians);
    const int steps = std::max(12, static_cast<int>(std::ceil(radius * sweep)));

    int prev_x = cx + static_cast<int>(std::lround(std::cos(start_radians) * radius));
    int prev_y = cy + static_cast<int>(std::lround(std::sin(start_radians) * radius));
    for (int i = 1; i <= steps; ++i) {
        const double t = start_radians + (sweep * static_cast<double>(i) / static_cast<double>(steps));
        const int x = cx + static_cast<int>(std::lround(std::cos(t) * radius));
        const int y = cy + static_cast<int>(std::lround(std::sin(t) * radius));
        draw_line(buffer, width, height, prev_x, prev_y, x, y, thickness, value);
        prev_x = x;
        prev_y = y;
    }
}

void draw_text_centered(RenderBuffer& buffer,
                        int width,
                        int height,
                        const Rect& rect,
                        int y,
                        const std::string& text,
                        int scale,
                        Color value) {
    const std::string fitted = fit_text_to_width(text, scale, rect.width - (scale * 4));
    const int x = rect.x + std::max(0, (rect.width - text_width(fitted, scale)) / 2);
    draw_text(buffer, width, height, x, y, fitted, scale, value);
}

RenderBuffer colorize_gray_buffer(const std::vector<unsigned char>& gray, int width, int height) {
    RenderBuffer buffer = make_render_buffer(width, height, PixelFormat::RGBA32, {0U, 0U, 0U, 0xFFU});
    const std::size_t pixel_count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    for (std::size_t i = 0; i < pixel_count; ++i) {
        const unsigned char value = gray[i];
        const std::size_t offset = i * 4U;
        buffer.pixels[offset] = value;
        buffer.pixels[offset + 1U] = value;
        buffer.pixels[offset + 2U] = value;
        buffer.pixels[offset + 3U] = 0xFFU;
    }
    return buffer;
}

}  // namespace hadisplay::scene
