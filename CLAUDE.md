# KAI watch — host simulator

An LVGL simulator that runs the watch UI on macOS, so the interface can be
built and seen without flashing hardware. The long-term goal is a retro clock
face whose hour and minute digits morph into the two eyes of an assistant
face — that morph is the point of the project, so nothing here may foreclose
it.

Read `SPEC.md` for the job, guardrails and definition of done.

## Target hardware

| | |
|---|---|
| Board | Waveshare ESP32-S3-Touch-AMOLED-2.06 |
| SoC | ESP32-S3R8 |
| PSRAM | 8 MB |
| Flash | 32 MB |
| Panel | 410 x 502 AMOLED, RGB565, driven over QSPI |
| Touch | capacitive (the simulator's mouse stands in for it) |

The simulator window is **410 x 502 at 1:1**. Do not change it and do not add a
zoom factor. Matching the panel exactly is the whole reason the simulator is
worth having: a layout that fits here fits on the device.

## Pinned versions

| | |
|---|---|
| LVGL | **v9.4.0**, tag `c016f72`, checked out detached in `lvgl/` |
| SDL2 | Homebrew `sdl2`, which now resolves to `sdl2-compat` 2.32.72 (SDL2 API over SDL3) |
| CMake | 4.4.3 |
| `lv_font_conv` | **1.5.3**, run through `npx`, and only to regenerate the digit font |

`lvgl/` is gitignored. To restore it in a fresh clone:

    git clone --depth 1 --branch v9.4.0 https://github.com/lvgl/lvgl.git lvgl

LVGL MUST stay on a release tag and MUST NOT track `master`. Upstream **v9.5.0
exists**; moving to it is a deliberate decision with a re-check of the
`lv_conf.h` line numbers below, not a default.

## lv_conf.h

`lv_conf.h` is a copy of `lvgl/lv_conf_template.h` with four changes. It sits at
the project root and LVGL finds it via `LV_CONF_INCLUDE_SIMPLE` plus that root
on the include path.

| Line | Setting | Why it matters |
|---|---|---|
| 15 | `#if 0` -> `#if 1` | **The file is inert without this, and it is the most common way to lose an hour here.** The template wraps its whole body in `#if 0`; leave it and LVGL silently compiles against built-in defaults, so every other setting below does nothing. Verify with `grep -c '^#if 1 /\* Set this' lv_conf.h` — must print `1`. |
| 30 | `LV_COLOR_DEPTH 16` | The panel is RGB565. Already 16 in the v9.4 template; kept explicit so a template bump cannot change it silently. |
| 72 | `LV_MEM_SIZE (512 * 1024U)` | The 64 KB default cannot hold the objects and draw buffers of a 410 x 502 UI. The device has 8 MB PSRAM, so 512 KB is affordable there too. |
| 1212 | `LV_USE_SDL 1` | Compiles LVGL's own SDL display and input backend — the host half of the simulator. The firmware build sets this back to `0`. |

Line numbers are for the v9.4.0 template. Re-grep rather than trusting them
after any LVGL bump:

    grep -n -E '^\s*#define (LV_COLOR_DEPTH|LV_MEM_SIZE|LV_USE_SDL)\b' lv_conf.h

## The host / device boundary

This is the rule that matters most as the code grows.

- **`ui.c` / `ui.h` — portable.** LVGL and the C standard library only. No SDL,
  no ESP-IDF, no `driver/` headers. This is compiled *unchanged* into the
  firmware.
- **`main.c` — host only.** SDL window, input devices, tick source, service
  loop. The firmware replaces this file wholesale.

New code goes on the portable side unless it genuinely needs the host; decide
which side a new file is on before writing it.

The check that settles it is the symbol list — compile the portable side alone
and look at what it leaves undefined. Anything but `lv_*` and libc is a leak:

    clang -std=c11 -DLV_CONF_INCLUDE_SIMPLE -I. -c ui.c -o /tmp/ui.o
    nm -u /tmp/ui.o | grep -i 'sdl\|esp_'    # must print nothing

Today that list is `lv_*` plus `time` and `localtime_r`, and nothing else. Do
not grep the sources for the string `SDL` instead — the file comments say the
word, so it always false-positives.

## Build and run

    cmake -S . -B build
    cmake --build build -j
    ./build/kai_sim

Close the window to exit — `LV_SDL_DIRECT_EXIT` is 1. `compile_commands.json`
lands in `build/` for clangd.

Prerequisites, with Homebrew at `/opt/homebrew` (arm64; `/usr/local` would mean
an x86 install and is wrong for this machine):

    brew install cmake pkg-config sdl2

### Why CMakeLists.txt is shaped the way it is

Each of these was a real failure or a real warning, not a preference:

- `add_compile_definitions(LV_CONF_INCLUDE_SIMPLE)` and `include_directories()`
  come **before** `add_subdirectory(lvgl)`. LVGL's own sources include
  `lv_conf.h`; set them after and LVGL builds against its defaults.
- SDL2 is linked **`PUBLIC` into the `lvgl` target**, not only into `kai_sim`.
  `LV_USE_SDL=1` makes LVGL compile its own SDL backend, and those sources
  `#include <SDL2/SDL.h>`. Without this the build fails in
  `lvgl/src/drivers/sdl/`.
- SDL2 is found with `pkg_check_modules(... IMPORTED_TARGET sdl2)` rather than
  `find_package(SDL2)`; pkg-config follows the Homebrew arm64 prefix with no
  hand-written hint paths.
- `CONFIG_LV_BUILD_EXAMPLES`, `CONFIG_LV_BUILD_DEMOS` and
  `CONFIG_LV_USE_THORVG_INTERNAL` are `FORCE`d **cache** entries. LVGL declares
  them with `option()` under `cmake_minimum_required(3.12.4)`, where CMP0077 is
  unset — a plain `set()` of the same name gets cleared and the options stay on.

## The digit font

The clock digits are `ui_font_digits_130.c`: Montserrat Medium at 130 px,
digits only, generated from the TTF that LVGL ships in
`lvgl/scripts/built_in_font/`. It is checked in, so the build needs nothing
extra; regenerate it only to change the size, which needs node because the
converter comes from npm:

    tools/gen_digit_font.py --size 130

Two things about that font are deliberate:

- **The digits are monospaced.** Montserrat's figures are proportional — at
  130 px a `1` is 48 px wide where a `0` is 87 px — so a centred label shifts
  sideways whenever a digit changes, and at this size the shift is impossible
  to miss. `lv_font_conv` has no monospace switch, so the script rewrites
  every glyph to the widest advance, 87 px, with its ink centred in that cell,
  the way the digit cells of a VFD sit. `--no-kerning` belongs to the same
  decision: a kern pair would pull a digit back out of its cell. A group of
  two digits is therefore 174 px wide whatever the time is.
- **It holds `0`-`9` and nothing else.** 31 KB of glyphs, and no glyph to fall
  back on, so placeholder text in `ui.c` has to be digits as well.

48 px is the largest Montserrat LVGL ships pre-generated, which is what kept
the digits too small before. `LV_FONT_MONTSERRAT_48` is now `0` — nothing
else uses it — and `LV_FONT_MONTSERRAT_14` stays on as `LV_FONT_DEFAULT`.

## Current UI

`ui_build()` draws the clock face on the active screen: black background, amber
`0xFFB000` digits in `ui_font_digits_130`, the hour group 102 px left of centre
and the minute group 102 px right of it, refreshed by a 1 Hz `lv_timer`.

That 102 px is derived, not chosen: the layout rule is a **16 px margin
between a group's outer edge and the edge of the panel**, and with a 174 px
group on a 410 px panel that puts each group's centre 102 px out, leaving
30 px between the two groups. Change `UI_EDGE_MARGIN` and the offset follows.
The margin is measured on the group box; the amber itself sits further in,
by however much narrower than its 87 px cell the digit showing happens to be.

The two digit groups are deliberately **separate objects placed symmetrically
about the centre**, because they have to become the assistant's two eyes. Keep
them independent: do not merge them into a single label and do not put a fixed
separator between them.

The morph itself is not scaffolded yet.

## Verifying a render without a screenshot

`screencapture` and `osascript` need Screen Recording and Accessibility
permission for the terminal, which is not granted here, so window screenshots
fail with `could not create image from display`. To check pixels instead,
render `ui.c` headlessly: create a display with
`LV_DISPLAY_RENDER_MODE_FULL` over a plain `uint8_t` buffer, call `ui_build()`,
pump `lv_timer_handler()`, then `lv_refr_now()` and dump the buffer. Amber
`0xFFB000` reads back as `#FFB200` after the RGB565 round-trip — that is
correct, not a bug.
