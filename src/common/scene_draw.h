#pragma once

#include "scene.h"

#include <string>

namespace hadisplay::scene {

bool contains(const Rect& rect, int x, int y);
RenderBuffer make_render_buffer(int width, int height, PixelFormat pixel_format, Color fill);
void set_pixel(RenderBuffer& buffer, int width, int height, int x, int y, Color value);
void fill_rect(RenderBuffer& buffer, int width, int height, const Rect& rect, Color value);
void draw_rect(RenderBuffer& buffer, int width, int height, const Rect& rect, Color value);
Rect inset_rect(const Rect& rect, int inset);
void draw_rect_thick(RenderBuffer& buffer,
                     int width,
                     int height,
                     const Rect& rect,
                     int thickness,
                     Color value);
void draw_text(RenderBuffer& buffer,
               int width,
               int height,
               int x,
               int y,
               const std::string& text,
               int scale,
               Color value);
void draw_line(RenderBuffer& buffer,
               int width,
               int height,
               int x0,
               int y0,
               int x1,
               int y1,
               int thickness,
               Color value);
void draw_arc(RenderBuffer& buffer,
              int width,
              int height,
              int cx,
              int cy,
              int radius,
              double start_radians,
              double end_radians,
              int thickness,
              Color value);
int text_width(const std::string& text, int scale);
std::string uppercase_ascii(const std::string& input);
std::string fit_text_to_width(const std::string& text, int scale, int max_width);
void draw_text_centered(RenderBuffer& buffer,
                        int width,
                        int height,
                        const Rect& rect,
                        int y,
                        const std::string& text,
                        int scale,
                        Color value);

}  // namespace hadisplay::scene
