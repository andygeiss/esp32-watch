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

`lv_conf.h` is a copy of `lvgl/lv_conf_template.h` with seven changes. It sits at
the project root and LVGL finds it via `LV_CONF_INCLUDE_SIMPLE` plus that root
on the include path.

| Line | Setting | Why it matters |
|---|---|---|
| 15 | `#if 0` -> `#if 1` | **The file is inert without this, and it is the most common way to lose an hour here.** The template wraps its whole body in `#if 0`; leave it and LVGL silently compiles against built-in defaults, so every other setting below does nothing. Verify with `grep -c '^#if 1 /\* Set this' lv_conf.h` — must print `1`. |
| 30 | `LV_COLOR_DEPTH 16` | The panel is RGB565. Already 16 in the v9.4 template; kept explicit so a template bump cannot change it silently. |
| 72 | `LV_MEM_SIZE (512 * 1024U)` | The 64 KB default cannot hold the objects and draw buffers of a 410 x 502 UI. The device has 8 MB PSRAM, so 512 KB is affordable there too. |
| 611 | `LV_FONT_MONTSERRAT_18 1` | The corner readouts, and the WiFi and battery symbols, which LVGL compiles into its built-in fonts. |
| 618 | `LV_FONT_MONTSERRAT_32 1` | The `Hey Kai` / `Quit` button label is letters, and the generated fonts hold only digits. Built-in fonts are off by default. |
| 622 | `LV_FONT_MONTSERRAT_40 1` | The weekday under the date. It is letters, which none of the generated fonts here hold. |
| 1212 | `LV_USE_SDL 1` | Compiles LVGL's own SDL display and input backend — the host half of the simulator. The firmware build sets this back to `0`. |

Line numbers are for the v9.4.0 template. Re-grep rather than trusting them
after any LVGL bump:

    grep -n -E '^\s*#define (LV_COLOR_DEPTH|LV_MEM_SIZE|LV_FONT_MONTSERRAT_(18|32|40)|LV_USE_SDL)\b' lv_conf.h

## The host / device boundary

This is the rule that matters most as the code grows.

- **`ui.c` / `ui.h` — portable.** LVGL and the C standard library only. No SDL,
  no ESP-IDF, no `driver/` headers. This is compiled *unchanged* into the
  firmware.
- **`main.c` — host only.** SDL window, input devices, tick source, service
  loop. The firmware replaces this file wholesale.

New code goes on the portable side unless it genuinely needs the host; decide
which side a new file is on before writing it.

A fact the UI needs but cannot reach for itself — the battery, the radio —
crosses in the other direction, through a struct and a setter in `ui.h`
(`ui_status_t`, `ui_status_set()`). The host fills it with a fake, the
firmware will fill it from the hardware, and `ui.c` never learns which. That
is the pattern for the next one too.

The check that settles it is the symbol list — compile the portable side alone
and look at what it leaves undefined. Anything but `lv_*` and libc is a leak:

    clang -std=c11 -DLV_CONF_INCLUDE_SIMPLE -I. -c ui.c -o /tmp/ui.o
    nm -u /tmp/ui.o | grep -i 'sdl\|esp_'    # must print nothing

`make check` runs exactly this, plus `-Wall -Wextra -Werror` on the same
compile. Today that list is `lv_*` plus `time` and `localtime_r`, and nothing
else. Do
not grep the sources for the string `SDL` instead — the file comments say the
word, so it always false-positives.

## Build and run

    make run

`make` on its own is `make check`: the `lv_conf.h` liveness grep above, the
build, the boundary check below, and `test_ui.c`, in that order. `make test`
runs that last gate alone, for the inner loop. `make ci` runs all of them
against the commit, which is what catches a file that was never added — it
symlinks this checkout's `lvgl/` into the copy, since a gitignored directory
is never in the archive. `make clean` removes `build/`.

The Makefile is the baseline's (`stack/makefile.md`) with CMake in place of
the Go toolchain, minus two things this project has no work for: `fmt` (no
formatter is set up, and picking one now would reflow every hand-laid line)
and the `.env` line in `run` (the simulator reads no configuration).
Underneath it is only:

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
- The portable half is its own target, `kai_ui`. Two hosts link it — `kai_sim`
  with its SDL window, `kai_test` with a byte array — which is the host/device
  boundary above, put where the build can hold it up.
- `CONFIG_LV_BUILD_EXAMPLES`, `CONFIG_LV_BUILD_DEMOS` and
  `CONFIG_LV_USE_THORVG_INTERNAL` are `FORCE`d **cache** entries. LVGL declares
  them with `option()` under `cmake_minimum_required(3.12.4)`, where CMP0077 is
  unset — a plain `set()` of the same name gets cleared and the options stay on.

## The fonts

Three are generated here, because LVGL ships Montserrat pre-generated only up
to 48 px — far too small on this panel — and ships no microphone glyph at all:

| File | Drawn with it | From |
|---|---|---|
| `ui_font_digits_118.c` | the clock digits — `0`-`9` at 118 px, in 79 px cells | Montserrat |
| `ui_font_date_72.c` | the date — `0`-`9` and `/` at 72 px, about 60% of the digits | Montserrat |
| `ui_font_assistant_18.c` | the speaker and microphone corners — U+F028 and U+F130, 313 bytes | FontAwesome 5 Solid |

Both source faces are the ones LVGL ships in `lvgl/scripts/built_in_font/`.

Both are checked in, so the build needs nothing extra. Regenerate them only to
change a size, which needs node because the converter comes from npm:

    tools/gen_fonts.py

Three things about them are deliberate:

- **Tabular figures.** Montserrat's figures are proportional — at 118 px a `1`
  is 44 px wide where a `0` is 79 px — so a centred label shifts sideways
  whenever a digit changes, and at this size the shift is impossible to miss.
  `lv_font_conv` has no monospace switch, so the script rewrites every digit to
  the widest digit's advance with its ink centred in that cell, the way the
  digit cells of a VFD sit. `--no-kerning` belongs to the same decision: a kern
  pair would pull a digit back out of its cell.
- **Punctuation keeps its own advance.** The `/` of `MM/DD` stays 33 px rather
  than taking a 63 px digit cell, where it would look stranded.
- **They hold those characters and nothing else** — 25 KB, 10 KB and 313 bytes
  of glyphs — and there is no glyph to fall back on, so placeholder text in
  `ui.c` has to be made of characters the font actually has.

Changing a size touches four places: `tools/gen_fonts.py`, the file name in
`CMakeLists.txt`, and in `ui.c` the font name and the cell and line-height
constants the layout is derived from.

The button is the exception: its label is letters, so it uses LVGL's built-in
`lv_font_montserrat_32`.

## Current UI

`ui_build()` puts two views on one screen and morphs between them.

**Clock.** Black background, amber `0xFFB000` throughout: the hour group 94 px
left of centre, the minute group 94 px right of it, then `MM/DD` and its
weekday centred below them, and a `Hey Kai` button 32 px off the bottom. One
1 Hz `lv_timer` refreshes all four labels.

The stack steps down in size — 118 px for the time, 72 px for the date, 40 px
for the weekday — and the gap above the date-and-weekday pair (24 px) is wider
than the one inside it (8 px), so the two read as one block under the time
rather than as three separate lines. Every offset in it is derived from those
heights and `UI_EDGE_MARGIN`; none is typed in by hand.

**Corners.** The hardware along the top: `LV_SYMBOL_WIFI` top-left and the
charge as `62% ` top-right, both in LVGL's built-in Montserrat 18, whose
symbols ship inside it. The assistant along the bottom: the speaker
bottom-left and the microphone bottom-right, from `ui_font_assistant_18.c`,
because LVGL has a speaker symbol but no microphone at all.

**The speaker and the microphone are independent flags, not one mode.** A
half-duplex firmware never raises both — the speaker plays, then the
microphone opens. One with acoustic echo cancellation, which ESP-SR gives the
S3, listens *while* it talks, and that is exactly what lets someone interrupt
the assistant mid-sentence. Wiring the two readouts as a single toggle would
make that state undrawable, so `ui.c` reads `listening` and `speaking`
separately and the test asserts both can be lit at once.

A reading the platform does not have is **dimmed rather than hidden**: an
empty corner reads as a bug, a dim one reads as "no". All four stay up in both
views — the corners are a status layer over whichever face is showing, and
none of it stops mattering while the assistant is listening. The values arrive
through `ui_status_set()` — see the host/device boundary above.

**Assistant.** The same two groups, now the assistant's two eyes: 120 px amber
circles in the same places, with the digits and the date faded out. The button
reads `Quit` and switches back.

**The morph** takes 400 ms. Each eye is an `lv_obj` that starts as the digit
group's own box — same size, same centre, `LV_RADIUS_CIRCLE`, invisible — so
the switch is that box growing square while the digits fade off the front of
it. Nothing moves; only size and opacity animate, and `lv_obj_align` keeps
each eye on its centre as it resizes.

**The blink** is one infinite animation per eye, started by the morph's own
completion callback: the height pulls in to a 12 px line over 70 ms, back out
over 70 ms, then holds open for 3.6 s. It drives the same property the morph
does, which is the point — `lv_anim_start` replaces an animation with the same
object and callback, so switching back to the clock cancels the blink with no
bookkeeping. It is also why that callback asks which view it is in before
starting: it runs at the end of every eye resize, in both directions.

This is why the two digit groups are **separate objects placed symmetrically
about the centre**: they are the two eyes. Keep them independent — do not
merge them into a single label and do not put a fixed separator between them.

**The layout rule is a 32 px margin.** Nothing comes closer than that to an
edge of the panel. `UI_EDGE_MARGIN` is the constant; the 94 px group offset,
the vertical centring of the time-and-date stack and the button's distance
from the bottom all derive from it. It is also what caps the digits at 118 px:
two 158 px groups plus two 32 px margins leave 30 px between the groups, and
a larger font would close that gap.

The corners are safe to write in. A corner label's outermost pixel sits 32 px
in on both axes, which stays inside the panel's rounded corner for any corner
radius up to about 109 px — more than anything a 410 x 502 panel is likely to
have. The bottom two clear the button as well: the speaker ends 43 px short of
it and the microphone starts 51 px past it.

## Verifying a render without a screenshot

`screencapture` and `osascript` need Screen Recording and Accessibility
permission for the terminal, which is not granted here, so window screenshots
fail with `could not create image from display`. `test_ui.c` is the way round
it, and `make test` runs it: it creates a display with
`LV_DISPLAY_RENDER_MODE_FULL` over a plain `uint8_t` buffer, calls
`ui_build()`, steps a fake tick source, clicks the button through
`lv_obj_send_event()`, and checks geometry, opacity, label text and the pixels
themselves. 59 checks. Three of them have been made to fail on purpose:
fading a corner readout out with the clock, putting the edge margin back to
16, and letting the eyes keep `LV_OBJ_FLAG_CLICKABLE`. Do that to any check you add —
a check that has never failed is a check you have not tested.

Two things to know before writing another renderer like it:

- **The display needs a flush callback** that calls `lv_display_flush_ready()`.
  Without one the first refresh leaves the display marked as flushing and the
  second spins in `wait_for_flushing` forever — a silent hang, because
  `LV_USE_LOG` is off.
- **Step a fake tick source**, not the wall clock. It is what makes a 400 ms
  morph and a 3.6 s blink pause cost nothing, and it is why the test can look
  at a frame part-way through an animation.

Amber `0xFFB000` reads back as `#FFB200` after the RGB565 round-trip — that is
correct, not a bug, and the test asserts exactly that value.
