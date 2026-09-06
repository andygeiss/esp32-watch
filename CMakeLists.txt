cmake_minimum_required(VERSION 3.20)
project(kai_sim LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# lv_conf.h sits at the project root and LVGL's own sources include it too,
# so both the define and the include path must be in place before
# add_subdirectory(lvgl) — otherwise LVGL compiles against its defaults.
add_compile_definitions(LV_CONF_INCLUDE_SIMPLE)
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# pkg-config rather than find_package(SDL2): it follows the Homebrew arm64
# prefix (/opt/homebrew) without any hand-written hint paths.
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)

# The simulator needs the library alone; LVGL's bundled examples, demos and
# ThorVG (LV_USE_THORVG is 0 in lv_conf.h) are dead weight in the build.
# These must be FORCEd cache entries: LVGL declares them with option() under
# cmake_minimum_required(3.12.4), where CMP0077 is unset and a normal variable
# of the same name gets cleared.
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
add_subdirectory(lvgl)

# LV_USE_SDL=1 makes LVGL compile its own SDL backend, and those sources
# include <SDL2/SDL.h>. So SDL2 has to reach the lvgl target as well, not just
# kai_sim — PUBLIC so the executable inherits it.
target_link_libraries(lvgl PUBLIC PkgConfig::SDL2)

add_executable(kai_sim
    main.c
    ui.c
    ui_font_digits_130.c  # generated, see tools/gen_digit_font.py
)

target_link_libraries(kai_sim PRIVATE lvgl PkgConfig::SDL2)
