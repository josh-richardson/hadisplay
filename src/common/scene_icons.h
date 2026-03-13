#pragma once

#include "scene.h"

#include <vector>

namespace hadisplay::scene {

void draw_status_chip(std::vector<unsigned char>& buffer,
                      int width,
                      int height,
                      const Rect& rect,
                      unsigned char border,
                      unsigned char fill);
void draw_sun_icon(std::vector<unsigned char>& buffer,
                   int width,
                   int height,
                   const Rect& rect,
                   unsigned char value);
void draw_wrench_icon(std::vector<unsigned char>& buffer,
                      int width,
                      int height,
                      const Rect& rect,
                      unsigned char value);
void draw_cog_icon(std::vector<unsigned char>& buffer,
                   int width,
                   int height,
                   const Rect& rect,
                   unsigned char value);
void draw_wifi_icon(std::vector<unsigned char>& buffer,
                    int width,
                    int height,
                    const Rect& rect,
                    bool connected,
                    unsigned char value,
                    unsigned char muted);
void draw_cloud_icon(std::vector<unsigned char>& buffer,
                     int width,
                     int height,
                     const Rect& rect,
                     bool rainy,
                     unsigned char value,
                     unsigned char muted);
void draw_battery_icon(std::vector<unsigned char>& buffer,
                       int width,
                       int height,
                       const Rect& rect,
                       int percent,
                       bool charging,
                       bool available,
                       unsigned char value,
                       unsigned char muted);

}  // namespace hadisplay::scene
