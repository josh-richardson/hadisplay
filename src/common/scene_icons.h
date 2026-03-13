#pragma once

#include "scene.h"

namespace hadisplay::scene {

void draw_status_chip(RenderBuffer& buffer,
                      int width,
                      int height,
                      const Rect& rect,
                      Color border,
                      Color fill);
void draw_sun_icon(RenderBuffer& buffer,
                   int width,
                   int height,
                   const Rect& rect,
                   Color value);
void draw_wrench_icon(RenderBuffer& buffer,
                      int width,
                      int height,
                      const Rect& rect,
                      Color value);
void draw_cog_icon(RenderBuffer& buffer,
                   int width,
                   int height,
                   const Rect& rect,
                   Color value);
void draw_wifi_icon(RenderBuffer& buffer,
                    int width,
                    int height,
                    const Rect& rect,
                    bool connected,
                    Color value,
                    Color muted);
void draw_cloud_icon(RenderBuffer& buffer,
                     int width,
                     int height,
                     const Rect& rect,
                     bool rainy,
                     Color value,
                     Color muted);
void draw_battery_icon(RenderBuffer& buffer,
                       int width,
                       int height,
                       const Rect& rect,
                       int percent,
                       bool charging,
                       bool available,
                       Color value,
                       Color muted);

}  // namespace hadisplay::scene
