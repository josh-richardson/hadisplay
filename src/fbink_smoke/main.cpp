#include <fbink.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

namespace {

struct Rgba {
    unsigned char r;
    unsigned char g;
    unsigned char b;
    unsigned char a;
};

void fill_rect(std::vector<unsigned char>& buffer,
               int width,
               int height,
               int x,
               int y,
               int rect_width,
               int rect_height,
               Rgba color) {
    const int x0 = std::max(0, x);
    const int y0 = std::max(0, y);
    const int x1 = std::min(width, x + rect_width);
    const int y1 = std::min(height, y + rect_height);

    for (int py = y0; py < y1; ++py) {
        for (int px = x0; px < x1; ++px) {
            const std::size_t offset =
                (static_cast<std::size_t>(py) * static_cast<std::size_t>(width) + static_cast<std::size_t>(px)) * 4U;
            buffer[offset] = color.r;
            buffer[offset + 1U] = color.g;
            buffer[offset + 2U] = color.b;
            buffer[offset + 3U] = color.a;
        }
    }
}

std::vector<unsigned char> make_color_smoke_buffer(int width, int height) {
    std::vector<unsigned char> buffer(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U, 0xFFU);

    const int gutter = std::max(24, width / 32);
    const int header_height = std::max(88, height / 8);
    const int footer_height = std::max(72, height / 10);
    const int body_y = header_height + gutter;
    const int body_height = std::max(120, height - body_y - footer_height - gutter);
    const int patch_width = std::max(80, (width - (gutter * 5)) / 4);

    fill_rect(buffer, width, height, 0, 0, width, height, {250U, 247U, 238U, 255U});
    fill_rect(buffer, width, height, gutter, gutter, width - (gutter * 2), header_height, {31U, 27U, 23U, 255U});
    fill_rect(buffer, width, height, gutter, height - footer_height - gutter, width - (gutter * 2), footer_height, {232U, 226U, 210U, 255U});

    fill_rect(buffer, width, height, gutter, body_y, patch_width, body_height, {184U, 55U, 47U, 255U});
    fill_rect(buffer, width, height, gutter * 2 + patch_width, body_y, patch_width, body_height, {61U, 126U, 72U, 255U});
    fill_rect(buffer, width, height, gutter * 3 + patch_width * 2, body_y, patch_width, body_height, {46U, 103U, 155U, 255U});
    fill_rect(buffer, width, height, gutter * 4 + patch_width * 3, body_y, patch_width, body_height, {213U, 166U, 42U, 255U});

    for (int i = 0; i < 5; ++i) {
        const int x = gutter + i * (patch_width + gutter);
        fill_rect(buffer, width, height, x - (gutter / 2), body_y - (gutter / 2), 4, body_height + gutter, {31U, 27U, 23U, 255U});
    }

    return buffer;
}

}  // namespace

int main() {
    FBInkConfig cfg{};
    cfg.is_quiet = true;
    cfg.is_cleared = true;
    cfg.is_flashing = true;
    cfg.is_centered = true;
    cfg.is_halfway = true;
    cfg.is_padded = true;
    cfg.fontmult = 2;

    const int fbfd = fbink_open();
    if (fbfd < 0) {
        std::cerr << "fbink_open failed.\n";
        return EXIT_FAILURE;
    }

    if (fbink_init(fbfd, &cfg) < 0) {
        std::cerr << "fbink_init failed.\n";
        fbink_close(fbfd);
        return EXIT_FAILURE;
    }

    FBInkState state{};
    fbink_get_state(&cfg, &state);

    if (!state.has_color_panel) {
        const char* message =
            "Hello, FBInk\n"
            "hadisplay grayscale smoke test\n"
            "\n"
            "FBInk reports no color panel.\n";
        if (fbink_print(fbfd, message, &cfg) < 0) {
            std::cerr << "fbink_print failed.\n";
            fbink_close(fbfd);
            return EXIT_FAILURE;
        }
        std::this_thread::sleep_for(std::chrono::seconds(5));
        fbink_close(fbfd);
        return EXIT_SUCCESS;
    }

    const int width = static_cast<int>(state.view_width);
    const int height = static_cast<int>(state.view_height);
    const std::vector<unsigned char> buffer = make_color_smoke_buffer(width, height);

    FBInkConfig draw_cfg{};
    draw_cfg.is_quiet = true;
    draw_cfg.is_flashing = true;
    draw_cfg.is_cleared = true;
    draw_cfg.wfm_mode = WFM_GCC16;

    if (fbink_print_raw_data(fbfd,
                             buffer.data(),
                             width,
                             height,
                             buffer.size(),
                             0,
                             0,
                             &draw_cfg) < 0) {
        std::cerr << "fbink_print_raw_data failed.\n";
        fbink_close(fbfd);
        return EXIT_FAILURE;
    }

    std::cerr << "Rendered color smoke buffer using WFM_GCC16 at " << width << "x" << height << ".\n";
    std::this_thread::sleep_for(std::chrono::seconds(5));

    fbink_close(fbfd);
    return EXIT_SUCCESS;
}
