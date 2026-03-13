#include <fbink.h>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <chrono>

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

    const char* message =
        "Hello, FBInk\n"
        "hadisplay linuxfb smoke test\n"
        "\n"
        "This target validates the display boundary.\n";

    if (fbink_print(fbfd, message, &cfg) < 0) {
        std::cerr << "fbink_print failed.\n";
        fbink_close(fbfd);
        return EXIT_FAILURE;
    }

    std::this_thread::sleep_for(std::chrono::seconds(5));

    fbink_close(fbfd);
    return EXIT_SUCCESS;
}
