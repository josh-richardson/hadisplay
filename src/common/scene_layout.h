#pragma once

#include "scene.h"

namespace hadisplay::scene {

struct SceneLayout {
    Rect system_bar;
    Rect outer;
    Rect header;
    Rect body;
    Rect footer;
    Rect power_button;
    Rect brightness_button;
    Rect dev_button;
    Rect wifi_button;
    Rect battery_button;
    Rect clock_rect;
    Rect weather_rect;
};

SceneLayout make_scene_layout(int width, int height, bool compact_ui = false);
Rect grid_cell(const Rect& bounds, int columns, int rows, int column, int row, int gutter);

}  // namespace hadisplay::scene
