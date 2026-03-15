cmake_minimum_required(VERSION 3.24)
project(hadisplay LANGUAGES CXX)

include(ExternalProject)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(HADISPLAY_FBINK_GIT_TAG "ede0a8d0a107768609fde9a68aab5f5c583be398")

option(HADISPLAY_FETCH_FBINK "Fetch and build FBInk automatically" ON)
option(HADISPLAY_BUILD_APP "Build the main hadisplay application" ON)
option(HADISPLAY_BUILD_FBINK_SMOKE "Build the FBInk framebuffer smoke target" ON)
option(HADISPLAY_BUILD_FBINK_MIRROR_X11 "Build the FBInk-to-X11 mirror target" ON)

set(HADISPLAY_NEEDS_CURL OFF)
set(HADISPLAY_NEEDS_X11 OFF)
set(HADISPLAY_NEEDS_FBINK OFF)

if(HADISPLAY_BUILD_APP)
    set(HADISPLAY_NEEDS_CURL ON)
    set(HADISPLAY_NEEDS_FBINK ON)
endif()

if(HADISPLAY_BUILD_FBINK_MIRROR_X11)
    set(HADISPLAY_NEEDS_CURL ON)
    set(HADISPLAY_NEEDS_X11 ON)
    set(HADISPLAY_NEEDS_FBINK ON)
endif()

if(HADISPLAY_BUILD_FBINK_SMOKE)
    set(HADISPLAY_NEEDS_FBINK ON)
endif()

if(HADISPLAY_NEEDS_CURL)
    find_package(CURL QUIET)
endif()

if(HADISPLAY_BUILD_APP)
    find_package(Threads REQUIRED)
endif()

if(HADISPLAY_NEEDS_X11)
    find_package(X11 REQUIRED)
endif()

if(HADISPLAY_NEEDS_FBINK)
    if(NOT HADISPLAY_FETCH_FBINK)
        message(FATAL_ERROR "FBInk targets were requested, but HADISPLAY_FETCH_FBINK is OFF.")
    endif()

    set(FBINK_SOURCE_DIR "${CMAKE_BINARY_DIR}/_deps/FBInk-src")
    set(FBINK_BUILD_COMMAND_PREFIX "")
    set(FBINK_PLATFORM_ARGS "LINUX=true")
    find_program(HADISPLAY_GIT NAMES git REQUIRED)

    get_filename_component(HADISPLAY_C_COMPILER_DIR "${CMAKE_C_COMPILER}" DIRECTORY)
    get_filename_component(HADISPLAY_C_COMPILER_NAME "${CMAKE_C_COMPILER}" NAME)
    if(HADISPLAY_C_COMPILER_NAME MATCHES "^(.+)-gcc$")
        set(FBINK_CROSS_TC "${CMAKE_MATCH_1}")
        set(FBINK_BUILD_COMMAND_PREFIX "env PATH='${HADISPLAY_C_COMPILER_DIR}:$ENV{PATH}' CROSS_TC='${FBINK_CROSS_TC}' ")
        if(FBINK_CROSS_TC MATCHES "kobo")
            set(FBINK_PLATFORM_ARGS "KOBO=true")
        endif()
    endif()

    ExternalProject_Add(fbink_external
        GIT_REPOSITORY https://github.com/NiLuJe/FBInk.git
        GIT_TAG "${HADISPLAY_FBINK_GIT_TAG}"
        GIT_SHALLOW TRUE
        UPDATE_DISCONNECTED TRUE
        SOURCE_DIR "${FBINK_SOURCE_DIR}"
        CONFIGURE_COMMAND ""
        BUILD_IN_SOURCE TRUE
        BUILD_COMMAND /bin/sh -lc "${FBINK_BUILD_COMMAND_PREFIX}${HADISPLAY_GIT} submodule update --init --recursive && ${FBINK_BUILD_COMMAND_PREFIX}${CMAKE_MAKE_PROGRAM} clean >/dev/null 2>&1 || true && ${FBINK_BUILD_COMMAND_PREFIX}${CMAKE_MAKE_PROGRAM} staticlib ${FBINK_PLATFORM_ARGS} MINIMAL=true DRAW=true FONTS=true IMAGE=true"
        INSTALL_COMMAND ""
        BUILD_BYPRODUCTS "${FBINK_SOURCE_DIR}/Release/libfbink.a"
    )

    file(MAKE_DIRECTORY "${FBINK_SOURCE_DIR}")
    add_library(fbink STATIC IMPORTED GLOBAL)
    set_target_properties(fbink PROPERTIES
        IMPORTED_LOCATION "${FBINK_SOURCE_DIR}/Release/libfbink.a"
        INTERFACE_INCLUDE_DIRECTORIES "${FBINK_SOURCE_DIR}"
    )
    add_dependencies(fbink fbink_external)

    if(FBINK_PLATFORM_ARGS STREQUAL "KOBO=true")
        file(MAKE_DIRECTORY "${FBINK_SOURCE_DIR}/libi2c-staged/include")
        add_library(fbink_i2c STATIC IMPORTED GLOBAL)
        set_target_properties(fbink_i2c PROPERTIES
            IMPORTED_LOCATION "${FBINK_SOURCE_DIR}/libi2c-staged/lib/libi2c.a"
            INTERFACE_INCLUDE_DIRECTORIES "${FBINK_SOURCE_DIR}/libi2c-staged/include"
        )
        add_dependencies(fbink_i2c fbink_external)
    endif()

    if(HADISPLAY_BUILD_FBINK_SMOKE)
        add_executable(hello_fbink_linuxfb
            src/fbink_smoke/main.cpp
        )
        target_include_directories(hello_fbink_linuxfb PRIVATE src/common)
        target_link_libraries(hello_fbink_linuxfb PRIVATE fbink m)
        if(TARGET fbink_i2c)
            target_link_libraries(hello_fbink_linuxfb PRIVATE fbink_i2c)
        endif()
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_link_options(hello_fbink_linuxfb PRIVATE -static-libstdc++ -static-libgcc)
        endif()
        target_compile_options(hello_fbink_linuxfb PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    if(HADISPLAY_BUILD_APP)
        add_executable(hadisplay
            src/common/app_config.cpp
            src/common/ha_client.cpp
            src/common/json.cpp
            src/common/scene.cpp
            src/common/scene_draw.cpp
            src/common/scene_icons.cpp
            src/common/scene_layout.cpp
            src/common/system_status.cpp
            src/app/main.cpp
        )
        target_include_directories(hadisplay PRIVATE src/common)
        target_link_libraries(hadisplay PRIVATE fbink m Threads::Threads)
        if(TARGET fbink_i2c)
            target_link_libraries(hadisplay PRIVATE fbink_i2c)
        endif()
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_link_options(hadisplay PRIVATE -static-libstdc++ -static-libgcc)
        endif()
        target_compile_options(hadisplay PRIVATE -Wall -Wextra -Wpedantic)

        # libcurl: use find_package if available (host builds), otherwise
        # fall back to manual header/link flags (cross-compile for Kobo where
        # curl headers aren't in the sysroot but the .so is on the device).
        if(CURL_FOUND)
            target_link_libraries(hadisplay PRIVATE CURL::libcurl)
        else()
            # Cross-compile: use host curl headers (ABI-stable) and link
            # dynamically against the libcurl.so on the device.
            # Use vendored curl headers (cmake/curl/) to avoid pulling in
            # host system headers that break cross-compilation.
            # ha_client.cpp does #include <curl/curl.h>, so we add cmake/
            # as include root.
            target_include_directories(hadisplay PRIVATE "${CMAKE_SOURCE_DIR}/cmake")
            target_link_directories(hadisplay PRIVATE "${CMAKE_SOURCE_DIR}/cmake/kobo-libs")
            target_link_libraries(hadisplay PRIVATE curl)
            # Point the runtime linker at Kobo Stuff's lib path.
            # Allow unresolved symbols in shared libs — libcurl's transitive
            # deps (openssl, zstd, zlib) are on the device but not in our sysroot.
            target_link_options(hadisplay PRIVATE
                "LINKER:--allow-shlib-undefined"
                "LINKER:-rpath,/mnt/onboard/.niluje/usbnet/lib"
                "LINKER:-rpath,/usr/lib"
            )
        endif()
    endif()

    if(HADISPLAY_BUILD_FBINK_MIRROR_X11 AND CURL_FOUND)
        add_executable(hello_fbink_mirror_x11
            src/common/app_config.cpp
            src/common/ha_client.cpp
            src/common/json.cpp
            src/common/scene.cpp
            src/common/scene_draw.cpp
            src/common/scene_icons.cpp
            src/common/scene_layout.cpp
            src/common/system_status.cpp
            src/fbink_mirror/main.cpp
        )
        target_include_directories(hello_fbink_mirror_x11 PRIVATE src/common src/fbink_mirror)
        target_link_libraries(hello_fbink_mirror_x11 PRIVATE CURL::libcurl X11 fbink m)
        if(TARGET fbink_i2c)
            target_link_libraries(hello_fbink_mirror_x11 PRIVATE fbink_i2c)
        endif()
        target_compile_options(hello_fbink_mirror_x11 PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()
