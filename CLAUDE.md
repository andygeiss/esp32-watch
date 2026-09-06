# KAI watch

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

`lv_conf.h` is a copy of `lvgl/lv_conf_template.h` with eight changes. It sits
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
| 626 | `LV_FONT_MONTSERRAT_32 1` | The `Hey Kai` / `Quit` button label is letters, and the generated fonts hold only digits. Built-in fonts are off by default. |
| 630 | `LV_FONT_MONTSERRAT_40 1` | The weekday under the date. It is letters, which none of the generated fonts here hold. |
| 1222 | `LV_USE_SDL` under `ESP_PLATFORM` | `1` on the host, which compiles LVGL's own SDL display and input backend — the host half of the simulator. `0` on the device, where there is no SDL and those sources would not compile. |

Line numbers are for the v9.4.0 template. Re-grep rather than trusting them
after any LVGL bump:

    grep -n -E '^\s*#(define (LV_COLOR_DEPTH|LV_MEM_SIZE|LV_USE_STDLIB_MALLOC|LV_FONT_MONTSERRAT_(18|32|40)|LV_USE_SDL)\b|ifdef ESP_PLATFORM)' lv_conf.h

## The host / device boundary

This is the rule that matters most as the code grows.

**The directory a file is in is which side of the boundary it is on.** That is
the whole reason for the layout, and it is why there is no `src/`: one folder
called "the source" would put the first two of these in the same bag and say
nothing.

- **`ui/` — portable.** `ui.c`, `ui.h` and the three fonts. LVGL and the C
  standard library only. No SDL, no ESP-IDF, no `driver/` headers. Both builds
  compile these files *unchanged*, from where they sit.
- **`host/` — host only.** `main.c` for the SDL window, input devices, tick
  source and service loop; `voice.c` for the microphone, the two speech
  services and the speaker; `test_ui.c` for the headless renderer.
- **`firmware/main/` — device only.** The same four jobs against the panel,
  plus the clock the board cannot read for itself, split over `main.c`,
  `board.c` and `net.c`. It replaces `host/main.c` rather than adding to it.

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

The one fact that travels the other way is the button, through
`ui_on_view_change()`: the platform cannot see a press, and waking an
assistant is not something `ui.c` can do for itself. It is a single function
pointer, `NULL` by default, and `ui.c` never learns what it does — the
simulator opens a microphone on it, the firmware will wake a codec. Keep that
seam this narrow. A second callback is a sign the UI is being asked to drive
the platform rather than report to it.

Two checks settle it, one on each side. The host's is the symbol list — compile
the portable side alone and look at what it leaves undefined. Anything but
`lv_*` and libc is a leak:

    clang -std=c11 -DLV_CONF_INCLUDE_SIMPLE -I. -c ui/ui.c -o /tmp/ui.o
    nm -u /tmp/ui.o | grep -i 'sdl\|esp_'    # must print nothing

`make check` runs exactly this, plus `-Wall -Wextra -Werror` on the same
compile. Today that list is `lv_*` plus `time` and `localtime_r`, and nothing
else — `voice.c` is a whole voice loop and none of it shows up here, which is
the point. Do
not grep the sources for the string `SDL` instead — the file comments say the
word, so it always false-positives.

The firmware's is the build itself: `firmware/components/kai_ui/` compiles the
same four files out of `ui/` and `REQUIRES lvgl` and nothing else, so an
ESP-IDF header in `ui.c` is not on its include path and the build stops. It
never names `host/` at all, which is the same rule stated as a directory it
cannot reach.

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

The simulator runs without it, but the assistant only speaks once
`voices/kai.opus` and `voices/kai.txt` are in place — see the voice loop
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

`firmware/` is the ESP-IDF project. Three files of its own, and everything
else shared with the simulator:

| | |
|---|---|
| `firmware/main/main.c` | the device host layer — tick source, LVGL loop, `ui_status_set()`. The counterpart of `host/main.c`, and deliberately the same shape |
| `firmware/main/board.c` | the QSPI panel and the touch controller, handed to LVGL |
| `firmware/main/net.c` | WiFi and SNTP, both off unless an SSID is configured |
| `firmware/components/kai_ui/` | a `CMakeLists.txt` and nothing else: it compiles `ui.c` and the three fonts out of the repo's `ui/` |

**`lvgl/` is the same checkout, not a second one.** `firmware/CMakeLists.txt`
puts the repo root's `lvgl/` on `EXTRA_COMPONENT_DIRS` rather than letting the
component manager fetch `lvgl/lvgl`, so the two halves cannot end up on
different LVGL versions. It is also what finds `lv_conf.h` — see that section
above.

**The link is the `lv_conf.h` liveness check on this side.** `CONFIG_LV_CONF_SKIP`
defaults to `y`, which makes LVGL ignore `lv_conf.h` entirely and take its
settings from menuconfig — the firmware's version of the `#if 0` trap at the
top of that file, and just as quiet. `sdkconfig.defaults` sets it to `n`. If it
ever goes back, LVGL's Kconfig defaults leave `LV_FONT_MONTSERRAT_32` and `_40`
off, `ui.c` names both, and the build fails at the link instead of booting to a
face with no letters on it.

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
| `listening`, `speaking` | `false` until there is an audio path behind them: the ES8311 codec and a wake-word engine. The simulator already fills both for real — see the voice loop |

Each is one line in `status_tick()` when it arrives, and none of it reaches
into `ui.c`. That is the point of the struct.

### The clock

The board has no RTC, so `time(NULL)` counts from reset until something tells
it otherwise, and SNTP over WiFi is the only thing on this board that can.
Both are off by default. `idf.py -C firmware menuconfig`, under **KAI watch**:
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

## The voice loop

`host/voice.c` is the simulator's audio path, and the reason the microphone
and speaker corners are no longer faked. It is host-only, like `main.c`: the board
has no codec wired up and no wake word yet, so the Mac's own microphone and
speakers do the job the ES8311 will do later.

It is a translation of four packages of `~/workspace/kai/orchestrator`, which
is the same loop in Go:

| Go | here |
|---|---|
| `internal/parakeet` | `transcribe()` — a multipart POST, one field read back |
| `internal/chatterbox` | `synthesise()` — a JSON POST carrying the clip to clone |
| `internal/echo` | `reply()` — the answer is the question, word for word |
| `internal/app`'s turn state | an SDL thread and two atomics |

**The answer is the question said back.** That is a mode rather than a
placeholder — it is the shortest path through the whole pipeline, so a turn
that breaks here breaks everywhere, and what comes out of the speaker is
exactly what the transcriber heard. `reply()` is the one function a language
model would go behind; nothing else would move. `internal/domain/speech.go`'s
chunker is the other half of that job and is deliberately not translated yet:
it cuts a streaming reply at sentence seams so speech starts before the text
is finished, and with an echo there is nothing to stream.

**Everything runs on its own thread.** A turn costs seconds and LVGL is single
threaded, so the loop cannot live in `lv_timer_handler()`. It publishes two
booleans through SDL atomics, `status_tick()` in `main.c` reads them once a
second, and `ui_status_set()` carries them the rest of the way. The button
arrives from the other direction through `ui_on_view_change()`, so one press
morphs the digits into eyes and opens the microphone together, and `Quit`
closes it — mid-sentence if a reply is playing, which is what makes the button
an interrupt.

**No libraries beyond SDL2**, which is what `SPEC.md` allows. So the HTTP
client is a socket, a request written by hand and a reply read back; the JSON
is a scanner for one string field, not a parser; and base64 and the WAV header
are twenty lines each. That is not a workaround. The device will speak to the
same two endpoints through `esp_http_client`, and a body built by hand ports
where a libcurl call site would not.

Four things about it are load-bearing:

- **`chatterbox-multilingual-v3` ships no voice conditionals.** It answers
  `500` — *"No conditionals available"* — to every request that carries no clip
  to clone, so `voices/kai.opus` and its transcript in `voices/kai.txt` are not
  optional. They are gitignored: the clip is a recording of a person and this
  repository is licensed. Without them the loop does not start and the two
  corners stay dim, the same answer the firmware gives an unconfigured SSID.
  Copy them from `~/workspace/kai/orchestrator/voices/`.
- **The clip and its words travel together.** The server aligns one against
  the other and rejects the audio on its own.
- **16 kHz mono in, whatever comes back out.** `sdl2-compat` opens the
  microphone at exactly 16 kHz mono with no resampling, and nothing here
  resamples, so a device that will not open at that rate is refused rather
  than transcribed at the wrong speed. The reply has been 24 kHz mono every
  time, but it says so in its own header, so `play()` follows the file.
- **The turn ends on silence, measured rather than guessed.** A quiet room
  reads a mean RMS of 42 and peaks at 82; speech runs in the thousands. The
  gate is at 500, and 800 ms under it ends the turn. Raise `VOICE_SILENCE_RMS`
  in a louder room.

Measured on this machine, against the oMLX server at `127.0.0.1:8000`:
transcription answers in about **0.9 s** for 3 s of speech, and synthesis
takes **2.6-3.3 s** to produce 3 s of it — roughly real time, which is the
number the chunker exists to hide once there is a model writing the reply.

None of it is in `check`. The gate has to stay runnable on a Mac with nothing
on it, so `kai_test` never links `voice.c` — and `ui/` cannot reach it at
all, and the loop itself needs no
configuration: the endpoint and both model names are `#define`s at the top of
the file.

## Verifying a render without a screenshot

`screencapture` and `osascript` need Screen Recording and Accessibility
permission for the terminal, which is not granted here, so window screenshots
fail with `could not create image from display`. `test_ui.c` is the way round
it, and `make test` runs it: it creates a display with
`LV_DISPLAY_RENDER_MODE_FULL` over a plain `uint8_t` buffer, calls
`ui_build()`, steps a fake tick source, clicks the button through
`lv_obj_send_event()`, and checks geometry, opacity, label text and the pixels
themselves. 80 checks. Seven of them have been made to fail on purpose:
fading a corner readout out with the clock, putting the edge margin back to
16, letting the eyes keep `LV_OBJ_FLAG_CLICKABLE`, and four ways of getting
`ui_on_view_change()` wrong — never calling it, flipping which view it
reports, reporting one from `ui_build()`, and dropping the `NULL` guard, which
takes the whole run down with a segfault rather than printing a failure. Do
that to any check you add — a check that has never failed is a check you have
not tested.

Rebuild with the object removed (`rm build/CMakeFiles/kai_ui.dir/ui.c.o`) when
trying this. Two edits a second apart can leave `make` convinced `ui.c` is
older than its object, and a stale binary passing is worse than no check at
all.

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
