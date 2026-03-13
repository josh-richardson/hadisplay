set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(_kobo_toolchain_root "")
if(DEFINED KOBO_TOOLCHAIN_ROOT AND NOT "${KOBO_TOOLCHAIN_ROOT}" STREQUAL "")
    set(_kobo_toolchain_root "${KOBO_TOOLCHAIN_ROOT}")
elseif(DEFINED ENV{KOBO_TOOLCHAIN_ROOT} AND NOT "$ENV{KOBO_TOOLCHAIN_ROOT}" STREQUAL "")
    set(_kobo_toolchain_root "$ENV{KOBO_TOOLCHAIN_ROOT}")
endif()

if(NOT "${_kobo_toolchain_root}" STREQUAL "")
    set(ENV{KOBO_TOOLCHAIN_ROOT} "${_kobo_toolchain_root}")
endif()

list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES KOBO_TOOLCHAIN_ROOT)

find_program(KOBO_GCC
    NAMES arm-kobo-linux-gnueabihf-gcc
    HINTS
        "${_kobo_toolchain_root}"
        "${_kobo_toolchain_root}/bin"
)

find_program(KOBO_GXX
    NAMES arm-kobo-linux-gnueabihf-g++
    HINTS
        "${_kobo_toolchain_root}"
        "${_kobo_toolchain_root}/bin"
)

if(NOT KOBO_GCC OR NOT KOBO_GXX)
    message(FATAL_ERROR
        "Could not find the Kobo cross-compiler. Install KOReader's koxtoolchain and either "
        "put arm-kobo-linux-gnueabihf-gcc/g++ in PATH or set KOBO_TOOLCHAIN_ROOT to the toolchain root."
    )
endif()

set(CMAKE_C_COMPILER "${KOBO_GCC}" CACHE FILEPATH "Kobo C compiler" FORCE)
set(CMAKE_CXX_COMPILER "${KOBO_GXX}" CACHE FILEPATH "Kobo C++ compiler" FORCE)

get_filename_component(_kobo_bin_dir "${KOBO_GCC}" DIRECTORY)
get_filename_component(_kobo_prefix "${_kobo_bin_dir}/.." ABSOLUTE)

set(_kobo_candidate_sysroots
    "${_kobo_prefix}/arm-kobo-linux-gnueabihf/sysroot"
    "${_kobo_prefix}/sysroot"
)

foreach(_candidate IN LISTS _kobo_candidate_sysroots)
    if(EXISTS "${_candidate}")
        set(CMAKE_SYSROOT "${_candidate}" CACHE PATH "Kobo sysroot" FORCE)
        break()
    endif()
endforeach()

set(CMAKE_FIND_ROOT_PATH "${_kobo_prefix}")
if(DEFINED CMAKE_SYSROOT)
    list(APPEND CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}")
endif()
list(REMOVE_DUPLICATES CMAKE_FIND_ROOT_PATH)
set(CMAKE_FIND_ROOT_PATH "${CMAKE_FIND_ROOT_PATH}" CACHE STRING "Kobo search roots" FORCE)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
