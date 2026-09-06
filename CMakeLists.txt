cmake_minimum_required(VERSION 3.20)
project(kai_sim LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# lv_conf.h sits at the project root and LVGL's own sources include it too,
# so both the define and the include path must be in place before
# add_subdirectory(lvgl) — otherwise LVGL compiles against its defaults.
#
# The root stays on the include path even though no source lives there any
# more: it is what resolves lv_conf.h, and what makes ui/ui.c's own
# "lvgl/lvgl.h" reachable from a subdirectory. lv_conf.h cannot follow the
# sources into ui/ — LVGL's esp.cmake registers the parent of lvgl/ as the
# place to find it, so it has to stay beside lvgl/.
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

# The portable half as its own target, which is the host/device boundary from
# CLAUDE.md written into the build: two hosts link it here — the SDL window and
# the headless test — and the firmware builds the same sources unchanged. The
# directories say the same thing the targets do: ui/ compiles on both sides,
# host/ only here.
add_library(kai_ui STATIC
    ui/ui.c
    ui/ui_font_digits_118.c  # generated, see tools/gen_fonts.py
    ui/ui_font_date_72.c
    ui/ui_font_assistant_18.c
)
target_link_libraries(kai_ui PUBLIC lvgl)

# PUBLIC, so both hosts find ui.h without knowing where it lives. It is the
# only thing they may reach into ui/ for.
target_include_directories(kai_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/ui)

# The portable half of the voice loop, its own target for the same reason
# kai_ui is one: the firmware compiles this very file out of voice/. Stricter
# than kai_ui, though — it links nothing at all, not even LVGL, so the target
# has no target_link_libraries line and that is deliberate.
add_library(kai_turn STATIC voice/turn.c)
target_include_directories(kai_turn PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/voice)

# host/voice.c is host-only, like main.c: the SDL microphone, the socket and
# the SDL speaker, which is all that is left once voice/turn.c has the words,
# the gate and the two request bodies. Deliberately not in kai_ui and not in
# kai_test — `make test` has to pass with no oMLX server and no audio device
# anywhere near it.
add_executable(kai_sim host/main.c host/voice.c)
target_link_libraries(kai_sim PRIVATE kai_ui kai_turn PkgConfig::SDL2)

# Renders the UI into a byte array and checks what came out. `make test`.
add_executable(kai_test host/test_ui.c)
target_link_libraries(kai_test PRIVATE kai_ui PkgConfig::SDL2)
