#pragma once

#include "scene.h"

#include <string>
#include <vector>

namespace hadisplay::scene {

bool contains(const Rect& rect, int x, int y);
void set_pixel(std::vector<unsigned char>& buffer, int width, int height, int x, int y, unsigned char value);
void fill_rect(std::vector<unsigned char>& buffer, int width, int height, const Rect& rect, unsigned char value);
void draw_rect(std::vector<unsigned char>& buffer, int width, int height, const Rect& rect, unsigned char value);
Rect inset_rect(const Rect& rect, int inset);
void draw_rect_thick(std::vector<unsigned char>& buffer,
                     int width,
                     int height,
                     const Rect& rect,
                     int thickness,
                     unsigned char value);
void draw_text(std::vector<unsigned char>& buffer,
               int width,
               int height,
               int x,
               int y,
               const std::string& text,
               int scale,
               unsigned char value);
void draw_line(std::vector<unsigned char>& buffer,
               int width,
               int height,
               int x0,
               int y0,
               int x1,
               int y1,
               int thickness,
               unsigned char value);
void draw_arc(std::vector<unsigned char>& buffer,
              int width,
              int height,
              int cx,
              int cy,
              int radius,
              double start_radians,
              double end_radians,
              int thickness,
              unsigned char value);
int text_width(const std::string& text, int scale);
std::string uppercase_ascii(const std::string& input);
std::string fit_text_to_width(const std::string& text, int scale, int max_width);
void draw_text_centered(std::vector<unsigned char>& buffer,
                        int width,
                        int height,
                        const Rect& rect,
                        int y,
                        const std::string& text,
                        int scale,
                        unsigned char value);

}  // namespace hadisplay::scene
