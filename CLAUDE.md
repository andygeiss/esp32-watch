# ESP32 Watch

Two builds of one watch UI. An LVGL simulator that runs it on macOS at the
exact panel size, so the interface can be built and seen without flashing
hardware, and the ESP-IDF firmware that runs the same files on the board. The
long-term goal is a retro clock face whose hour and minute digits morph into
the two eyes of an assistant face — that morph is the point of the project, so
nothing here may foreclose it.

Read `SPEC.md` for the job, guardrails and definition of done, and
`GLOSSARY.md` for the word this project uses for each thing. `README.md`
is the short version of this file.

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
| ESP-IDF | **v5.5**, shallow clone at `~/esp/esp-idf` |
| `waveshare/esp_lcd_sh8601` | **2.0.0** — the panel controller |
| `espressif/esp_lcd_touch_ft5x06` | **1.1.1**, over `espressif/esp_lcd_touch` **1.2.1** — the touch controller |

`lvgl/` is gitignored. To restore it in a fresh clone:

    git clone --depth 1 --branch v9.4.0 https://github.com/lvgl/lvgl.git lvgl

LVGL MUST stay on a release tag and MUST NOT track `master`. Upstream **v9.5.0
exists**; moving to it is a deliberate decision with a re-check of the
`lv_conf.h` line numbers below, not a default.

The three driver components land in `firmware/managed_components/`, which is
gitignored; `firmware/dependencies.lock` is checked in and is what pins them.
ESP-IDF itself lives outside the tree:

    git clone --depth 1 --shallow-submodules --recursive -b v5.5 \
        https://github.com/espressif/esp-idf.git ~/esp/esp-idf
    ~/esp/esp-idf/install.sh esp32s3

## lv_conf.h

`lv_conf.h` is a copy of `lvgl/lv_conf_template.h` with seven changes. It sits
at the project root and LVGL finds it via `LV_CONF_INCLUDE_SIMPLE` plus that
root on the include path — in the host build because `CMakeLists.txt` sets both
before `add_subdirectory(lvgl)`, in the firmware because LVGL's own
`env_support/cmake/esp.cmake` does the same thing for `${LVGL_ROOT_DIR}/..`,
which is that root.

**One file serves both targets.** The two settings that cannot be the same on a
Mac and on the board switch on `ESP_PLATFORM`, which every ESP-IDF compile
defines and nothing else does. A second `lv_conf.h` would be a second layout.

| Line | Setting | Why it matters |
|---|---|---|
| 15 | `#if 0` -> `#if 1` | **The file is inert without this, and it is the most common way to lose an hour here.** The template wraps its whole body in `#if 0`; leave it and LVGL silently compiles against built-in defaults, so every other setting below does nothing. Verify with `grep -c '^#if 1 /\* Set this' lv_conf.h` — must print `1`. |
| 30 | `LV_COLOR_DEPTH 16` | The panel is RGB565. Already 16 in the v9.4 template; kept explicit so a template bump cannot change it silently. |
| 47 | `LV_USE_STDLIB_MALLOC` under `ESP_PLATFORM` | `LV_STDLIB_CLIB` on the device, `LV_STDLIB_BUILTIN` on the host. The built-in allocator's pool is a static array, and the 512 KB below is the whole of the ESP32-S3's internal SRAM. On the device LVGL allocates through the C library, which `CONFIG_SPIRAM_USE_MALLOC` points at the 8 MB of PSRAM. |
| 80 | `LV_MEM_SIZE (512 * 1024U)` | The 64 KB default cannot hold the objects and draw buffers of a 410 x 502 UI. Host only — the line above takes the device off this allocator. |
| 619 | `LV_FONT_MONTSERRAT_18 1` | The corner readouts, and the WiFi and battery symbols, which LVGL compiles into its built-in fonts. |
| 630 | `LV_FONT_MONTSERRAT_40 1` | The weekday under the date. It is letters, which none of the generated fonts here hold, and built-in fonts are off by default. `_32` was on beside it for the button label, and went out with the button. |
| 1222 | `LV_USE_SDL` under `ESP_PLATFORM` | `1` on the host, which compiles LVGL's own SDL display and input backend — the host half of the simulator. `0` on the device, where there is no SDL and those sources would not compile. |

Line numbers are for the v9.4.0 template. Re-grep rather than trusting them
after any LVGL bump:

    grep -n -E '^\s*#(define (LV_COLOR_DEPTH|LV_MEM_SIZE|LV_USE_STDLIB_MALLOC|LV_FONT_MONTSERRAT_(18|40)|LV_USE_SDL)\b|ifdef ESP_PLATFORM)' lv_conf.h

## The host / device boundary

This is the rule that matters most as the code grows.

**The directory a file is in is which side of the boundary it is on.** That is
the whole reason for the layout, and it is why there is no `src/`: one folder
called "the source" would put the portable directories and the platform ones
in the same bag and say nothing.

- **`ui/` — portable.** `ui.c`, `ui.h` and the three fonts. LVGL and the C
  standard library only. No SDL, no ESP-IDF, no `driver/` headers. Both builds
  compile these files *unchanged*, from where they sit.
- **`voice/` — portable, and one notch stricter.** `turn.c` and `turn.h` are
  everything about a turn that is not a device: the words the watch answers
  to, the gate that ends a turn, the JSON, the base64, the WAV and both
  request bodies. The C standard library and *nothing else* — not even LVGL.
  `voice.h` beside them is the five-function contract the two platform halves
  both implement. Both builds compile `turn.c` unchanged.
- **`host/` — host only.** `main.c` for the SDL window, input devices, tick
  source and service loop; `voice.c` for the SDL microphone, the socket and
  the SDL speaker; `test_ui.c` for the headless renderer.
- **`firmware/main/` — device only.** The same jobs against the panel and the
  board's own codecs, plus the clock the board cannot read for itself, split
  over `main.c`, `board.c`, `net.c` and `voice.c`. It replaces `host/main.c`
  and `host/voice.c` rather than adding to them.

**Why `voice/` exists at all**: `host/voice.c` and `firmware/main/voice.c` are
the same loop against two different sets of hardware, and almost everything
interesting in it is neither. The wake phrase in particular *must* be one
list — a watch that answers to a different name than the simulator does makes
the simulator worthless — and so must the gate, the endpointing and the two
bodies that go on the wire. So they are one file, and what is left in each
platform half is an audio device, an HTTP transport and a thread.

`lv_conf.h` and `lvgl/` stay at the root and cannot follow the sources into
`ui/`. LVGL's own `env_support/cmake/esp.cmake` registers `${LVGL_ROOT_DIR}/..`
as the place to find `lv_conf.h`, so it has to sit beside `lvgl/` — see the
`lv_conf.h` section above. The root stays on the include path for that reason
alone, which is also what resolves `ui/ui.c`'s own `"lvgl/lvgl.h"`.

New code goes in `ui/` unless it genuinely needs the host; decide which
directory a new file is in before writing it, because that is the same
decision.

A fact the UI needs but cannot reach for itself — the battery, the radio, the
microphone — crosses in the other direction, through a struct and a setter in
`ui.h` (`ui_status_t`, `ui_status_set()`). The host fills it with a fake, the
firmware fills it from the hardware, and `ui.c` never learns which. That is the
pattern for the next one too.

**Nothing travels the other way.** There is no button on either face, so
there is no press for `ui.c` to report. Which face is up is a fact about the
platform, like the charge and the radio, and it arrives in the same direction:
`ui_view_set(bool assistant)`. The simulator hears the watch's name in a
transcript, the firmware will hear it on a codec, and `ui.c` never learns
which. It is idempotent because the platform polls it — `host/main.c` hands it
an answer ten times a second, and only the answer that changed is a morph.

Keep it one-directional. A callback out of `ui.c` is a sign the UI is being
asked to drive the platform rather than be told by it, and the button was the
only thing that ever wanted one.

Two checks settle it, one on each side. The host's is the symbol list —
compile each portable directory alone and look at what it leaves undefined:

    clang -std=c11 -DLV_CONF_INCLUDE_SIMPLE -I. -c ui/ui.c -o /tmp/ui.o
    nm -u /tmp/ui.o | grep -i 'sdl\|esp_'      # must print nothing
    clang -std=c11 -Ivoice -c voice/turn.c -o /tmp/turn.o
    nm -u /tmp/turn.o | grep -i 'sdl\|esp_'    # must print nothing

`make check` runs exactly this, plus `-Wall -Wextra -Werror` on both compiles.
Today `ui.o` leaves `lv_*` plus `time` and `localtime_r`, and `turn.o` leaves
twelve libc symbols and not one more — no LVGL, no sockets, no logging. Do
not grep the sources for the string `SDL` instead — the file comments say the
word, so it always false-positives.

The firmware's is the build itself. `firmware/components/kai_ui/` compiles the
same four files out of `ui/` and `REQUIRES lvgl` and nothing else;
`firmware/components/kai_turn/` compiles `voice/turn.c` and `REQUIRES` nothing
at all, which is that file's claim about itself written into the build. An
ESP-IDF header in either would not be on its component's include path and the
build stops. Neither names `host/`, which is the same rule stated as a
directory they cannot reach.

## Build and run

    make run

`make` on its own is `make check`: the `lv_conf.h` liveness grep above, the
build, the boundary check below, and `test_ui.c`, in that order. `make test`
runs that last gate alone, for the inner loop. `make ci` runs all of them
against the commit, which is what catches a file that was never added — it
symlinks this checkout's `lvgl/` into the copy, since a gitignored directory
is never in the archive. `make clean` removes `build/`.

The Makefile is the baseline's (`stack/makefile.md`) with CMake in place of
the Go toolchain, minus one thing this project has no work for: `fmt`, because
no formatter is set up and picking one now would reflow every hand-laid line.
The `.env` line in `run` was missing for a while on the grounds that the
simulator read no configuration; it does now — the speech server's address and
its key — so the line is back, and `.env` is gitignored, which is the half of
the baseline's rule 6 that is never waived. Underneath it is only:

    cmake -S . -B build
    cmake --build build -j
    ./build/kai_sim

`make run` is that last line with `.env` sourced in front of it. `check`, `ci`
and `test` deliberately do not source it: a gate that reads one machine's file
is a gate that passes on one machine.

Close the window to exit — `LV_SDL_DIRECT_EXIT` is 1. `compile_commands.json`
lands in `build/` for clangd.

Prerequisites, with Homebrew at `/opt/homebrew` (arm64; `/usr/local` would mean
an x86 install and is wrong for this machine):

    brew install cmake pkg-config sdl2

The simulator runs without it, but the assistant only speaks once
`voices/female.wav` and `voices/female.txt` are in place —
`tools/gen_voice.sh` makes them, and the voice loop
below. `make run` starts the binary from the repository root, which is where
those paths are relative to.

For the board, with ESP-IDF exported into the shell first:

    . ~/esp/esp-idf/export.sh
    make firmware   # idf.py -C firmware build
    make flash      # idf.py -C firmware flash monitor

Neither is part of `check`. That gate has to stay runnable on a Mac with
nothing on it but Homebrew, and it is the one that runs before every commit.

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

## The firmware

`firmware/` is the ESP-IDF project. Four files of its own, and everything
else shared with the simulator:

| | |
|---|---|
| `firmware/main/main.c` | the device host layer — tick source, LVGL loop, `ui_status_set()`, `ui_view_set()`. The counterpart of `host/main.c`, and deliberately the same shape |
| `firmware/main/board.c` | the QSPI panel and the touch controller handed to LVGL, and the two audio codecs handed to `voice.c` |
| `firmware/main/net.c` | WiFi and SNTP, both off unless an SSID is configured |
| `firmware/main/voice.c` | the device half of the voice loop: the codecs, `esp_http_client`, and a task. The counterpart of `host/voice.c`, and the same shape again |
| `firmware/components/kai_ui/` | a `CMakeLists.txt` and nothing else: it compiles `ui.c` and the three fonts out of the repo's `ui/` |
| `firmware/components/kai_turn/` | the same trick for `voice/turn.c`, and it `REQUIRES` nothing at all |

**`lvgl/` is the same checkout, not a second one.** `firmware/CMakeLists.txt`
puts the repo root's `lvgl/` on `EXTRA_COMPONENT_DIRS` rather than letting the
component manager fetch `lvgl/lvgl`, so the two halves cannot end up on
different LVGL versions. It is also what finds `lv_conf.h` — see that section
above.

**The link is the `lv_conf.h` liveness check on this side.** `CONFIG_LV_CONF_SKIP`
defaults to `y`, which makes LVGL ignore `lv_conf.h` entirely and take its
settings from menuconfig — the firmware's version of the `#if 0` trap at the
top of that file, and just as quiet. `sdkconfig.defaults` sets it to `n`. If it
ever goes back, LVGL's Kconfig defaults leave `LV_FONT_MONTSERRAT_40` off,
`ui.c` names it for the weekday, and the build fails at the link instead of
booting to a face with a blank line under the date.

### The audio

Two chips, both in `board.c` beside the panel and for the same reason — their
pin numbers cannot be derived from anything:

| | |
|---|---|
| **ES8311** | the speaker. I2S `DOUT` on GPIO40, and the power amplifier on GPIO46, which the codec driver raises when the device opens so nothing hisses between replies |
| **ES7210** | the microphones. I2S `DSIN` on GPIO42 |

They share one duplex I2S bus — MCLK 16, BCLK 41, WS 45 — and they share the
I2C bus the touch controller already sits on, GPIO14/15. Two consequences,
both load-bearing:

- **`board_touch_init()` has to run before `board_audio_init()`**, because it
  is what creates that I2C bus. `app_main` calls them in that order and the
  audio side refuses to come up if it finds no bus.
- **One bus is one clock, so the two codecs cannot both be open** at different
  rates. That is why the board API is open/close rather than always-on:
  `board_speaker_open()` closes the microphone and vice versa. The loop is
  half duplex anyway, so this costs nothing that was not already gone — and it
  is the same pause the simulator makes.

`BOARD_MIC_GAIN_DB` is the one number here worth suspecting. The gate in
`voice/turn.c` was measured against a Mac's own microphone, so if the watch
never hears a sentence *end*, the gain is too high; if it never hears one
start, too low.

### The panel

Every pin number, the vendor power-on sequence and the column offset in
`board.c` are transcribed from Waveshare's own BSP for this board
(`waveshare/esp32_s3_touch_amoled_2_06` 2.0.0 in the ESP component registry).
They describe one specific piece of hardware and cannot be derived from
anything, so they are copied rather than worked out. That BSP is not a
dependency — it would drag in `esp_lvgl_port`, an audio codec and an LVGL of
its own — only the two driver components underneath it are.

Three things about the panel are not obvious and all three are load-bearing:

- **It reads RGB565 big-endian.** LVGL renders little-endian, so `flush()`
  runs `lv_draw_sw_rgb565_swap()` over the strip before handing it to DMA.
- **It addresses its frame memory in pairs of pixels**, so a dirty area with an
  odd edge has to grow out to the next even one — the `LV_EVENT_INVALIDATE_AREA`
  handler. It also settles the strip height: LVGL calls that same handler from
  `get_max_row()` with a trial area and shrinks until the rounded height fits
  the buffer, so no strip boundary can land back on an odd row.
- **Column 0 of the glass is column 22 of the controller**, hence
  `esp_lcd_panel_set_gap(panel, 22, 0)`.

Two draw buffers of 410 x 50 px, 41 kB each, in internal DMA-capable RAM, so
LVGL renders the next strip while the last one is still going out. The link
leaves about 266 kB of internal RAM free, so they and the WiFi stack fit with
room over.

### The heap

`LV_MEM_SIZE` is 512 kB and the ESP32-S3 has 512 kB of internal SRAM in total,
so the built-in allocator's static pool cannot exist on the device. `lv_conf.h`
switches `LV_USE_STDLIB_MALLOC` to `LV_STDLIB_CLIB` under `ESP_PLATFORM`, and
`CONFIG_SPIRAM_USE_MALLOC` points the C library's `malloc` at the 8 MB of
PSRAM.

**The voice task's stack does not hold a transcript.** Two of them are
`VOICE_MAX_TEXT` each, which was the whole of the task's original 8 kB before
a single call was made — it would have smashed its stack on the first turn,
and this firmware has never run on hardware, so nothing had said so. They are
`static` in `loop()` instead, there being exactly one voice task, and the
stack is 12 kB for `esp_http_client` and the TLS handshake under it.

### One task

Everything that touches LVGL runs in `app_main`'s loop — the timers, the touch
read, the status refresh — so there is no lock to take and none to forget.
`CONFIG_ESP_MAIN_TASK_STACK_SIZE` is 8192 because the default 3584 is not
enough to render from, and `CONFIG_FREERTOS_HZ` is 1000 so the loop's 1 ms
floor really is 1 ms.

`net.c` is the one thing outside that task, and all it ever does is set a flag
the loop reads.

### What the device can answer

The same `ui_status_t` the simulator fakes, filled from what is actually there:

| | |
|---|---|
| `wifi_up` | real, from the station's `IP_EVENT_STA_GOT_IP` |
| `battery_pct` | `-1`. There is no fuel gauge on this board, and the UI already draws `--%` for a charge it does not know — the honest reading, not an invented one |
| `listening`, `speaking` | real, from `voice.c` — the ES7210 in front of the microphones and the ES8311 in front of the speaker |

The charge is the only invented one left, and it is one line in
`status_tick()` when a gauge arrives. None of it reaches into `ui.c`. That is
the point of the struct.

**The assistant's face comes from the same place.** `view_tick()` polls
`voice_awake()` into `ui_view_set()` ten times a second, exactly as the
simulator's does — see the voice loop below.

### The clock

The board has no RTC, so `time(NULL)` counts from reset until something tells
it otherwise, and SNTP over WiFi is the only thing on this board that can.
Both are off by default. `idf.py -C firmware menuconfig`, under **ESP32 Watch**:
an empty SSID keeps the radio down, the watch runs off its boot clock, and the
WiFi corner draws dim — which is the reading that dimming is for. The timezone
is compiled in there too, because the device has no locale to turn UTC into
local time with.

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
both `CMakeLists.txt` files, and in `ui.c` the font name and the cell and
line-height constants the layout is derived from. The generated files land in
`ui/`, beside the code that names them.

The weekday is the exception: it is letters, so it uses LVGL's built-in
`lv_font_montserrat_40`, and the corner readouts use `_18` because LVGL
compiles the WiFi and battery symbols into it.

## Current UI

`ui_build()` puts two views on one screen and morphs between them.

**Clock.** Black background, amber `0xFFB000` throughout: the hour group 94 px
left of centre, the minute group 94 px right of it, then `MM/DD` and its
weekday centred below them. One 1 Hz `lv_timer` refreshes all four labels.

The stack steps down in size — 118 px for the time, 72 px for the date, 40 px
for the weekday — and the gap above the date-and-weekday pair (24 px) is wider
than the one inside it (8 px), so the two read as one block under the time
rather than as three separate lines. Every offset in it is derived from those
heights and `UI_EDGE_MARGIN`; none is typed in by hand. The block sits in the
middle of the whole panel: there is no button along the bottom reserving a
strip of it any more, and `UI_STACK_TOP` says exactly that.

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

**Assistant.** The same two groups, now the assistant's two eyes: 69 px amber
circles in the same places, with the digits and the date faded out. Each holds
a pupil — a hole the colour of the background, half of what the lids leave,
28 px — and a catchlight in the pupil, 8 px, up and to the left in both because
there is one light in the room. Neither is decoration: the pupil and the
catchlight are what tell an eye from a dot, and two discs the width of the
digit groups read as two discs.

The eye is smaller than the gap it sits beside, and by some way — 69 px of eye
with 119 px of black between the pair, where a face has roughly an eye's width
between the two. That is a deliberate choice of look and not a derived number:
closing it up would mean animating the eyes toward each other, and the morph
moves nothing. Whatever the size, the check is the same and it only binds
upwards — the gap must not fall below one eye's width, which is what a 120 px
eye did.

**Nothing on either face can be pressed.** Saying `Hey Kai` is what crosses
over, and a goodbye or 30 s of nothing said is what comes back — see the voice
loop below. `ui.c` is told which face to draw through `ui_view_set()` and has
no opinion about how the platform decided; it does not even own a `lv_button`
any more, which is why `LV_FONT_MONTSERRAT_32` came out of `lv_conf.h`. The
touch panel is still initialised on the device: the face is a thing to tap
later, and the eyes deliberately have `LV_OBJ_FLAG_CLICKABLE` off so they
cannot swallow that tap.

**The morph** takes 400 ms. Each eye is an `lv_obj` that starts as the digit
group's own box — same size, same centre, `LV_RADIUS_CIRCLE`, invisible — so
the switch is that box pulling in square while the digits fade off the front of
it. Nothing moves; only size and opacity animate, and `lv_obj_align` keeps
each eye on its centre as it resizes.

**The pupil and the catchlight are percentages, not animations.** LVGL sizes a
child against its parent's content box and scales its opacity by its parent's,
so both follow the eye down and back out and fade with it — there is still one
animation per property per eye. Give either a size of its own and it sits there
while the eye moves around it.

**The blink** is one infinite animation per eye, started by the morph's own
completion callback: the height pulls in to a 12 px line over 70 ms, back out
over 70 ms, then holds open for 3.6 s. It drives the same property the morph
does, which is the point — `lv_anim_start` replaces an animation with the same
object and callback, so switching back to the clock cancels the blink with no
bookkeeping. It is also why that callback asks which view it is in before
starting: it runs at the end of every eye resize, in both directions.

**The lids are the eye's own padding**, 6 px a side — half the height it shuts
to, so the two of them meet exactly as the blink bottoms out, and the pupil,
being a percentage of what they leave between them, is pinched out at that same
moment. Take the padding off and the eye closes to a line with a slit across
it.

This is why the two digit groups are **separate objects placed symmetrically
about the centre**: they are the two eyes. Keep them independent — do not
merge them into a single label and do not put a fixed separator between them.

**The layout rule is a 32 px margin.** Nothing comes closer than that to an
edge of the panel. `UI_EDGE_MARGIN` is the constant, and the 94 px group
offset derives from it. It is also what caps the digits at 118 px:
two 158 px groups plus two 32 px margins leave 30 px between the groups, and
a larger font would close that gap.

The corners are safe to write in. A corner label's outermost pixel sits 32 px
in on both axes, which stays inside the panel's rounded corner for any corner
radius up to about 109 px — more than anything a 410 x 502 panel is likely to
have. The bottom two have the whole width between them now that no button
sits there, and the centred stack clears them by more than 80 px.

## The voice loop

One loop, three files:

| | |
|---|---|
| `voice/turn.c` | **portable.** The words, the gate, the JSON, the base64, the WAV, the URL, which models answer, and all three request bodies. Compiled into both builds unchanged |
| `host/voice.c` | the SDL microphone, a hand-written request over a socket or OpenSSL, the SDL speaker, an `SDL_Thread` |
| `firmware/main/voice.c` | the ES7210, `esp_http_client`, the ES8311, a FreeRTOS task |

The two platform halves are deliberately the same shape: the same five
functions out of `voice/voice.h`, the same record/transcribe/answer/play pass
in the same order, the same three booleans published to the loop that draws.
Read one and you have read the other.

**Almost nothing in a turn is a device**, which is what `voice/turn.c` is for.
It went in when the firmware grew a voice, because the alternative was two
copies of the wake-phrase table — and a watch that answers to a different name
than the simulator does makes the simulator worthless. The gate, the
endpointing, the goodbye and the exact bytes that go on the wire are in there
for the same reason.

It is a translation of four packages of `~/workspace/kai/orchestrator`, which
is the same loop in Go:

| Go | here |
|---|---|
| `internal/parakeet` | `transcribe()` — a multipart POST, one field read back |
| `internal/chatterbox` | `synthesise()` — a JSON POST carrying the clip to clone |
| `internal/openai` | `reply()` — a JSON POST to `/v1/chat/completions`, one field read back |
| `internal/echo` | `reply()` again, with no model configured: the answer is the question |
| `internal/app`'s turn state | a thread and three atomics, once per platform |

**The answer comes from a chat model, or is the question said back.** The
model is the default — `Qwen3.8-27B-oQ4e-mtp`, in `turn.h` beside the other
three — so a watch with a server configured thinks rather than echoes. The
word `echo` in `WATCH_BRAIN_MODEL` or `CONFIG_WATCH_BRAIN_MODEL` turns it off
again; it is a word rather than an empty string because empty already means
"the default", and `turn.c` is where that word is understood, once, on the way
in. `BRAIN=echo` is what the Go orchestrator calls the same mode. The echo
is a mode rather than a
placeholder — it is the shortest path through the whole pipeline, so a turn
that breaks there breaks everywhere, what comes out of the speaker is exactly
what the transcriber heard, and it needs no third service. Both live behind
`reply()`, which is the only function that knows the difference; nothing else
moved when the model went in. A brain that is configured and fails is silence
rather than an echo: a watch that repeats the question when the model could
not be reached looks like it answered. `internal/domain/speech.go`'s
chunker is the other half of that job and is deliberately not translated yet:
it cuts a streaming reply at sentence seams so speech starts before the text
is finished, and with an echo there is nothing to stream.

**The transcriber is the wake-word engine.** There is no button and no
wake-word model, so the microphone is open from start-up, every utterance in
the room is recorded and transcribed, and the watch wakes when its own name
comes back in the text. `WAKE` in `voice/turn.c` is a table rather than one
string because Parakeet has never been shown that name and spells it a few
different ways; it is the greeting that is matched, not the name alone, or
*Kaiser* and half the German news would wake the watch. Whatever follows the
greeting is the first turn, so `Hey Kai, hallo` wakes it and answers `hallo`
in one go.

**The phrase is configuration, and `Hey Kai` is its default.** The platform
hands `voice_wake_set()` a `|`-separated list of spellings at start-up —
`WATCH_WAKE_PHRASE` in the host's environment, `CONFIG_WATCH_WAKE_PHRASE` on
the device
— which is the crossing the server address already makes. An empty list means
`WAKE` itself, so those six spellings stay written down exactly once and a
platform that wants them says nothing rather than repeating them. Case and
punctuation come off on the way in, so `Hey Kai!` and `hey kai` are one
phrase and whoever configures one need not know which. A list that does not
fit, or a phrase that cleans away to nothing, leaves the default in force and
says so in the log: an empty phrase is a substring of everything and would
wake the watch on every word in the room.

**On the host this is the only option; on the device it is a choice, and the
first one worth revisiting.** ESP-SR would hear the name on the S3 itself and
open a connection only then — less radio, less battery, and no room audio
leaving the watch. What stops it today is that WakeNet's models are a fixed
set — *Hi ESP*, *Alexa* and so on — and none of them is this watch's name. So
it is a trained model away rather than a flag away, and until then the watch
sends what it hears.

**Two ways back to the clock, because a misheard word must not trap you.** A
goodbye — `tschüss`, `quit`, `stop` — matched against the whole transcript
rather than as a substring, so *stopp mal die Musik* is a thing to answer; and
`VOICE_IDLE_MS`, 30 s with nothing said, which needs no word at all.
`record()` takes that as a deadline on the silence *before* the first word,
and `VOICE_WAIT_FOREVER` is the same function asleep, where there is nothing
to time out of.

**Everything runs on its own thread**, an `SDL_Thread` here and a FreeRTOS
task there. A turn costs seconds and LVGL is single threaded, so the loop
cannot live in `lv_timer_handler()` on either side — and because it never
does, there is still no lock anywhere near LVGL. It publishes three booleans
through atomics. `status_tick()` reads two of them once a second and
`ui_status_set()` carries them the rest of the way; `view_tick()` reads
`voice_awake()` ten times a second and hands it to `ui_view_set()`, which is
idempotent so nine of those ten cost nothing. The faster timer is the point:
the eyes come up as the wake phrase lands rather than up to a second later.
Both `main.c` files have exactly these two timers with exactly these two
periods.

`voice_listening()` is `awake && recording`, not `recording` alone. The
microphone really is open the whole time, but a corner that is always lit says
nothing — lit means the next thing said is meant for the assistant.

**Nothing interrupts a reply any more.** The button used to, mid-sentence.
Talking over the assistant means being heard while it is speaking, which needs
the acoustic echo cancellation this half-duplex loop has none of — the same
reason the two corner flags are independent. ESP-SR gives the S3 that, and the
interrupt comes back with it.

**No libraries beyond SDL2 and OpenSSL**, which is what `SPEC.md` allows. So
the HTTP client is still a request written by hand and a reply read back; the
JSON is a scanner for one string field, not a parser; and base64 and the WAV
header are twenty lines each. That is not a workaround. The device speaks to
the same three endpoints through `esp_http_client`, and a body built by hand
ports where a libcurl call site would not.

**OpenSSL is `send()` and `recv()` and nothing above them.** It went in when
the server stopped being on the same desk: `omlx.ai-at-home.de` answers `308`
on port 80, so a watch that talks to it has no plaintext way in. An `http://`
address still skips all of it, which is what an oMLX on this machine is. Two
calls do the work and leaving either out fails quietly —
`SSL_set_tlsext_host_name()` is how a proxy fronting several names knows which
certificate to present, and `SSL_set1_host()` is what makes OpenSSL check that
the certificate belongs to the name asked for. Verification without the second
proves only that some CA signed something. The device gets the same two things
from `esp_http_client` plus the certificate bundle ESP-IDF already compiles in
(`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE`), which is one line in `http_post()`.

**Everything about the server is configuration, and it is the same list
twice.** The address, the API key, the three model names, the language and the
wake phrase: the environment on the host, menuconfig on the device, and both
hand them to `voice_url_parse()`, `voice_models_set()` and `voice_wake_set()`
in `turn.c`. Empty means the default, which is written down exactly once — in
`turn.h` for the models, in `turn.c` for the wake table — so a platform that
wants the default says nothing rather than keeping a second copy of it.

| host, from the environment | device, from menuconfig | empty means |
|---|---|---|
| `WATCH_VOICE_URL` | `CONFIG_WATCH_VOICE_URL` | `http://127.0.0.1:8000` on the host; on the device, no assistant at all |
| `WATCH_VOICE_KEY` | `CONFIG_WATCH_VOICE_KEY` | no `Authorization` header is sent |
| `WATCH_STT_MODEL` | `CONFIG_WATCH_STT_MODEL` | `parakeet-tdt-0.6b-v3` |
| `WATCH_BRAIN_MODEL` | `CONFIG_WATCH_BRAIN_MODEL` | `Qwen3.8-27B-oQ4e-mtp`; the word `echo` means no chat request at all |
| `WATCH_TTS_MODEL` | `CONFIG_WATCH_TTS_MODEL` | `chatterbox-multilingual-v3` |
| `WATCH_LANGUAGE` | `CONFIG_WATCH_LANGUAGE` | `de` |
| `WATCH_NAME` | `CONFIG_WATCH_NAME` | `Kai` |
| `WATCH_WAKE_PHRASE` | `CONFIG_WATCH_WAKE_PHRASE` | the six spellings in `WAKE` |
| `WATCH_VOICE_CLIP` | fixed at `voices/female.wav` | `voices/female.wav`, transcript from the `.txt` beside it |

**The transcript is cleaned the same way the phrase was, and for a while it
was not.** `voice_wake_set()` strips case and ASCII punctuation from a phrase
on the way in; `voice_after_wake()` used to `strstr()` that cleaned phrase in a
transcript that still had all of its own. So `Hey, Lissi` — which is how a
transcriber writes it as often as not — could never meet `hey lissi`, and a
comma made the watch deaf to its own name. `wake_matches()` now compares in
place, cleaning both sides as it goes: no second buffer, which also took
`VOICE_MAX_TEXT` off a stack that had two of them.

**A transcript the watch decides is not for it is logged, not dropped.** The
transcriber has never been shown this name and writes down whatever sounded
closest, so choosing the spellings to listen for means reading what it
actually produced — `not for me: "Halusy."` is the line that makes that
possible, and its absence is what made a watch that would not wake impossible
to diagnose. Some names are simply not heard: spoken through the synthesiser
and transcribed back, `Hey Lissi` came out as `Halusy` and as nothing at all,
while `Hey Lisi` came back exactly. No wake list can recover a greeting the
transcriber did not write down.

**The name and the wake phrase are two settings, and that is deliberate.**
The wake list is what the *transcriber* writes down for a name it has never
been shown — the six built-in spellings include `hey ky` and `hey chai` — so a
name derived from it would have the watch introducing itself as Chai. What the
assistant is called and how it is misheard are two different facts, and only
one of them is ever spoken. `WATCH_NAME` is the answer to "who are you?", and
it reaches the brain through the system prompt and nowhere else; the wake
phrase is what the microphone listens for, and it reaches `voice_after_wake()`
and nowhere else.

Setting one and forgetting the other is the mistake this arrangement invites,
and it is quiet: a watch renamed only in the wake phrase answers to `Hey Ada`
and then says it is Kai.

**So both halves say what they took, on one line, at start-up.** A wake list
that does not fit also leaves the default in force, so a phrase that was
rejected and one that was never set look identical from then on. One line
settles both:

    voice: called Ada, answering to "hey ada" and 2 other spellings of it

`voice_name()`, `voice_wake_count()` and `voice_wake_at()` are in `turn.h` for
it, the same reason `voice_models_get()` is. The system prompt is built once,
when the name is set, rather than on every turn — it changes only when the
name does.

The key is the reason the host reads the environment rather than a header. A
key compiled in is a key committed, and this repository is public; the rest
follow it so that configuring the simulator is one kind of act and not two.
`make run` sources `.env` and then inherits the shell, so either place works
and `.env` is the one that survives a new terminal:

    cp .env.example .env

`.env.example` is checked in and carries the whole list with every default
written beside it; `.env` is gitignored, because one of those lines is a
secret. That pairing is the reason the example exists at all — a gitignored
file is invisible to whoever clones next, and a setting nobody can see is a
setting nobody uses.

    # .env — gitignored, one machine's setup
    WATCH_VOICE_URL="https://omlx.ai-at-home.de"
    WATCH_VOICE_KEY="..."
    WATCH_NAME="Ada"
    WATCH_WAKE_PHRASE="hey ada|hey adah|hi ada"

**Quote every value in it.** `make run` sources the file, so an unquoted value
holding a space or a `|` is not an assignment — `WATCH_WAKE_PHRASE=hey ada|hey
adah` is a temporary assignment of `hey` in front of the command `ada`, piped
into `hey adah`, and the variable is left empty. The `command not found: ada`
that says so scrolls past in the middle of a build, and the wake phrase is the
one setting likely to contain both characters. This cost an evening once.

Neither half ever logs the key, only whether there is one. On the device it is
compiled into the image, and anyone with the flash has it.

**An empty key means no header at all, not an empty one.** `Authorization:
Bearer ` with nothing behind it is a rejected request rather than an
unauthenticated one, and the two want different answers.

**Thinking has to be turned off through `chat_template_kwargs`.** This one was
measured, not assumed. Against oMLX with `Qwen3.8-27B-oQ4e-mtp` the top-level
`enable_thinking` has no effect whatever — three requests out of three came
back with the reply starting `Thinking: 1. Analyze the Request …`, every word
of which the watch reads aloud before reaching the answer. The same three with
`"chat_template_kwargs":{"enable_thinking":false}` came back `Es ist ungefähr
zehn Uhr morgens.` `voice_brain_body()` sends both, because other servers read
the top-level one and no server here objects to seeing it twice. The Go
orchestrator's `internal/openai` sends only the top-level field, and against
this server it has the same problem.

Five things about it are load-bearing:

- **`chatterbox-multilingual-v3` ships no voice conditionals.** Measured, not
  assumed: `/v1/audio/voices?model=chatterbox-multilingual-v3` answers
  `{"voices":[]}`, and a synthesis request carrying no clip answers `500` —
  *"No conditionals available. Either provide audio_prompt/audio_prompt_sr for
  voice cloning, or ensure conds.safetensors is in the model directory."*
  Naming a voice changes nothing, because there are none to name. So
  `voices/female.wav` and its transcript in `voices/female.txt` are not
  optional. `tools/gen_voice.sh` makes them. Without them the loop does not start
  at all: the watch is a clock, saying its name does nothing because nothing
  is listening for it, and the two corners stay dim. **The firmware links the
  same two files into flash** — `main/CMakeLists.txt` does that only
  `if(EXISTS ...)`, and by a fixed name: the linker symbol is derived from
  the file name, so on the device "which clip" is a build-time fact, so a fresh clone builds without them and says so
  (`WATCH: no voices/female.wav`) rather than failing.
- **The clip and its words travel together.** The server aligns one against
  the other and rejects the audio on its own. Which is why the transcript is
  the `.txt` beside the clip rather than a setting of its own — one path to
  configure, and no way to point a transcript at the wrong recording.

- **The clip is generated, not found, and that is the point.** A reference
  clip is a voice to be cloned, so a recording of someone who did not agree to
  that is not a candidate — a working voice actor's demo reel least of all,
  since their voice is the thing they sell. `tools/gen_voice.sh` speaks the
  words in this repository through macOS's own German voice Anna, which is
  synthetic and already on the machine.

- **Keep it to about five seconds.** It travels base64 in the body of every
  reply, so its length is paid for on every turn: 5.1 s is 162 kB of WAV and
  217 kB on the wire, where 8.2 s was 351 kB. `gen_voice.sh` says the duration
  and warns past six seconds. A longer clip does condition slightly better —
  the eight-second one transcribed back word-perfect where the five-second one
  turned *Viertel nach zehn* into *vierte Nacht zehn* — so this is a trade
  rather than a free win.
- **16 kHz mono in, whatever comes back out.** `sdl2-compat` opens the
  microphone at exactly 16 kHz mono with no resampling, and nothing here
  resamples, so a device that will not open at that rate is refused rather
  than transcribed at the wrong speed. The reply has been 24 kHz mono every
  time, but it says so in its own header, so `play()` follows the file.
- **The turn ends on silence, measured rather than guessed.** A quiet room
  reads a mean RMS of 42 and peaks at 82; speech runs in the thousands. The
  gate is at 500, and 800 ms under it ends the turn. Raise `VOICE_SILENCE_RMS`
  in a louder room.
- **A stream of exact zeros is a broken microphone, not a quiet room**, and the
  same measurement is what says so: a room never reads zero. Without that
  distinction the two are the same thing to the gate — both simply never end a
  turn — and a watch listening to a dead device waits forever for a wake phrase
  that cannot arrive, in silence, with nothing in the log. `voice_turn_dead()`
  is the rule, in `voice/turn.c` because it is true of both microphones, and it
  answers true once per turn so each half logs one line of its own:
  `host/voice.c` points at System Settings, `firmware/main/voice.c` at
  `BOARD_MIC_GAIN_DB` and the ES7210. This is a real failure and not a
  hypothetical one — a Mac whose default input is a pair of AirPods that are
  connected but not capturing opens at 16 kHz mono, delivers zeros, and looks
  from the outside exactly like a watch that is ignoring you.
- **SDL will not say which input it opened.** `SDL_OpenAudioDevice(NULL, ...)`
  takes the system default, and `SDL_GetDefaultAudioInfo` on sdl2-compat —
  which is what Homebrew's `sdl2` now is — answers with the literal words
  "System default" rather than a device, and fails outright if the spec it is
  handed is NULL. So `log_inputs()` lists the inputs SDL can see at start-up
  instead, and the choice between them stays where macOS keeps it. Naming the
  real one would mean CoreAudio, which is a framework and a page of code for a
  log line.
- **The close box has to reach `voice_stop()` before it reaches `SDL_Quit()`,**
  which is what `quit_first()` in `host/main.c` is for. LVGL's SDL backend
  answers `SDL_QUIT` with `SDL_Quit()` and *then* `exit(0)` — `lv_sdl_window.c`
  — so the audio devices are torn down while the voice thread is inside
  `SDL_DequeueAudio()`, and the process dies of a bus error (`make: *** [run]
  Bus error: 10`) before any `atexit()` handler runs. `quit_first()` pumps the
  event queue itself and peeks for `SDL_QUIT`, leaving the event there for LVGL
  to find a moment later; all it does is get the microphone closed first. This
  only started biting when the microphone stopped being opened on a button
  press and started being open the whole time — before that, a window closed on
  the clock face never had a device to pull out.

**The watch needs three things before it says a word**: an SSID, a server
address, and that clip. All three are off by default and each one missing
gives the same answer — a clock with two dim corners. A key is the fourth on
any server that checks one, and it fails differently: `401`, with the status
and the server's own sentence in the log, and a line naming the setting to
fill in. `idf.py -C firmware menuconfig`, under **ESP32 Watch**:
`CONFIG_WATCH_VOICE_URL` is a whole address — `https://omlx.ai-at-home.de`,
or `http://192.168.1.20:8000` for one on the network the watch joins, but
never `127.0.0.1`, which on the watch means the watch.

Because they default off, the interesting half of `firmware/main/voice.c` is
folded away by the compiler in a default build — `voice_start()` returns at
its first line and the linker drops the rest. A build that only proves the
default configuration is not proof the voice path compiles. Set a URL in
`sdkconfig` before believing a green firmware build.

Measured on this machine, against the oMLX server at `127.0.0.1:8000`:
transcription answers in about **0.9 s** for 3 s of speech, and synthesis
takes **2.6-3.3 s** to produce 3 s of it — roughly real time, which is the
number the chunker exists to hide once there is a model writing the reply.
Through `https://omlx.ai-at-home.de`, which is that same machine reached the
long way round, a whole turn measured **0.4 s** to transcribe, **6-11 s** to
think with `Qwen3.8-27B-oQ4e-mtp` and **2-23 s** to speak, depending on how
long the answer was. The brain is the expensive part, and it is what the
untranslated chunker exists to hide.

None of `host/voice.c` is in `check`. The gate has to stay runnable on a Mac
with nothing on it, so `kai_test` never links it and `ui/` cannot reach it at
all. `voice/turn.c` *is* in `check`, on both counts: it compiles clean under
`-Werror` and its symbol list is inspected, which is exactly what it earns by
being the file both platforms share. The shape of the system prompt and the
reply's token budget are what stayed decisions rather than settings: both are
about a watch rather than about a server, and a reply that leaves through a
speaker on someone's wrist has the same shape whichever model wrote it. The
name inside that prompt is the exception, and it is configuration for the
plainest reason there is — it is the answer to "who are you?", which is the
owner's to give.

## Verifying a render without a screenshot

`screencapture` and `osascript` need Screen Recording and Accessibility
permission for the terminal, which is not granted here, so window screenshots
fail with `could not create image from display`. `test_ui.c` is the way round
it, and `make test` runs it: it creates a display with
`LV_DISPLAY_RENDER_MODE_FULL` over a plain `uint8_t` buffer, calls
`ui_build()`, steps a fake tick source, drives `ui_view_set()` the way
`host/main.c` does, and checks geometry, opacity, label text and the pixels
themselves. 93 checks. Fifteen of them have been made to fail on purpose,
seven from before the eye got a pupil and eight since. The first seven: fading a corner
readout out with the clock, putting the edge margin back to 16, letting the
eyes keep `LV_OBJ_FLAG_CLICKABLE`, leaving `UI_STACK_TOP` reserving the strip
the button used to sit in, and three ways of getting `ui_view_set()` wrong —
flipping which flag means which view, restarting the morph when told the view
it is already in, and dropping the guard against being told anything before
`ui_build()`, which takes the whole run down rather than printing a failure. Do
that to any check you add — a check that has never failed is a check you have
not tested.

The last two are worth knowing the shape of. The morph and the blink drive the
same property, so a `ui_view_set()` that is not idempotent silently kills the
blink and nothing else — which is why the test re-asserts the view on *every*
frame for nine seconds rather than twice in a row. And the missing `NULL`
guard hangs rather than crashing: `LV_USE_LOG` is off and LVGL's assert
handler is `while(1)`, so a broken run has to be given a watchdog.

The other eight are the eye's, and were tried the same way: the eye back at
120 px, which closes the gap between the pair, and the eye down at 40 px, which
leaves a catchlight of 4 px that no longer reads as one; the pupil given a size
of its own instead of a percentage, and the lids' padding taken off, which both
leave it sitting in a shut eye; the pupil filled in amber rather than left as a
hole; the catchlight removed, and the catchlight hung off the eye instead of
the pupil; and the pupil left clickable, which would swallow the touch the eye
lets through. The three of them that read pixels out of the buffer are the only
checks here that can tell an eye from a dot — the geometry is the same either
way, and those three probe points are derived from the eye's size rather than
typed in, because a probe left behind at the old geometry lands on the wrong
thing and still passes.

Rebuild with the object removed when trying this:

    rm build/CMakeFiles/kai_ui.dir/ui/ui.c.o    # note the ui/ — it mirrors the source tree

Two edits a second apart can leave `make` convinced `ui.c` is older than its
object, and a stale binary passing is worse than no check at all. `touch` is
not enough on its own: `make` compares whole seconds, so a source touched and
built inside the same second still looks up to date. There is no `timeout` on
this machine either — background the run and kill it.

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
