# KAI watch

A retro clock face for the Waveshare ESP32-S3-Touch-AMOLED-2.06 whose hour and
minute digits morph into the two eyes of an assistant. One UI, built two ways:
an LVGL simulator that runs on macOS at the panel's exact size, and the ESP-IDF
firmware for the board itself.

```sh
make run        # the simulator, a 410 x 502 window
make firmware   # the board, with ESP-IDF exported into the shell
make flash      # onto the board, then its log
```

```
+---------------------+       +---------------------+
| wifi        62% [#] |       | wifi        62% [#] |
|                     |       |                     |
|   ## ##     ## ##   |       |     .---.   .---.   |
|   ## ##     ## ##   |  -->  |    (     ) (     )  |
|   ## ##     ## ##   |       |     `---'   `---'   |
|                     |       |                     |
|        09/06        |       |                     |
|         SUN         |       |                     |
|                     |       |                     |
|     ( Hey Kai )     |       |      ( Quit )       |
|                     |       |                     |
| spk             mic |       | spk             mic |
+---------------------+       +---------------------+
```

Amber on black, the way a VFD readout looks. Tapping `Hey Kai` grows each digit
group into an eye over 400 ms while the digits fade off the front of it; the
eyes then blink, 140 ms every 3.6 s. `Quit` runs it backwards. Nothing moves in
between — only size and opacity animate, which is why the hour and the minute
have to stay two separate objects placed symmetrically about the centre. They
are the two eyes.

## Why two builds

Reflashing an ESP32-S3 for every layout tweak makes the edit-look loop slow
enough that UI work stops happening. The simulator window is **410 x 502 at
1:1**, which is the whole reason it is worth having: a layout that fits there
fits on the panel.

It is only worth *trusting*, though, if what it shows is what the panel shows.
So there is exactly one of everything the two builds could disagree about — one
`ui.c`, one set of fonts, one `lvgl/` checkout, one `lv_conf.h` — and a rule
about which side of the line new code goes on.

## The host / device boundary

| | |
|---|---|
| `ui/` | **portable.** `ui.c`, `ui.h`, `ui_font_*.c`. LVGL and the C standard library, nothing else. Both builds compile these unchanged |
| `host/` | **host only.** `main.c` for the window, mouse, tick source and service loop; `voice.c` for the microphone, the two speech services and the speaker; `test_ui.c` for the headless renderer |
| `firmware/main/` | **device only.** The same jobs against the panel, plus the clock the board cannot read for itself |

The directory is the boundary. There is no `src/`, because one folder called
"the source" would hold the first two of those and say nothing about the rule.
`lv_conf.h` and `lvgl/` stay at the root: LVGL looks for its configuration
beside its own checkout, so it cannot move into `ui/`.

A fact the UI needs but cannot reach for itself — the charge, the radio, the
microphone — crosses the other way through one struct and one setter,
`ui_status_t` and `ui_status_set()`. The simulator fills it with a fake, the
firmware fills it from the hardware, and `ui.c` never learns which. The
button travels the other way, through `ui_on_view_change()`: one press morphs
the digits into eyes and opens the microphone together.

Both sides hold the line up mechanically. On the host, `make check` compiles
`ui/ui.c` alone and fails if `nm` finds anything undefined but `lv_*` and libc. In
the firmware, the component that builds those files requires LVGL and nothing
else, so an ESP-IDF header in `ui.c` is not on its include path.

## The hardware

| | |
|---|---|
| Board | Waveshare ESP32-S3-Touch-AMOLED-2.06 |
| SoC | ESP32-S3R8, 8 MB PSRAM, 32 MB flash |
| Panel | 410 x 502 AMOLED, RGB565, SH8601-compatible over QSPI |
| Touch | capacitive, FT5x06 protocol over I2C |

Pin numbers, the vendor power-on sequence and the panel's 22 px column offset
are transcribed from Waveshare's own BSP for this board. That BSP is not a
dependency; only the two driver components underneath it are, pinned exactly.

## Building

### The simulator

```sh
brew install cmake pkg-config sdl2
git clone --depth 1 --branch v9.4.0 https://github.com/lvgl/lvgl.git lvgl
make run
```

`lvgl/` is gitignored, so a fresh clone needs that one command. The assistant
speaks once `voices/kai.opus` and `voices/kai.txt` are in place — the
synthesiser has no voice of its own and clones that clip; without them the
watch runs and the two bottom corners stay dim. `make` on its
own is `make check`: the `lv_conf.h` liveness grep, the build, the boundary
check, and 80 assertions rendered into a byte array by `test_ui.c` — geometry,
opacity, label text and the pixels themselves, with no window and no
screenshot. `make ci` runs the lot against the commit.

### The firmware

```sh
git clone --depth 1 --shallow-submodules --recursive -b v5.5 \
    https://github.com/espressif/esp-idf.git ~/esp/esp-idf
~/esp/esp-idf/install.sh esp32s3

. ~/esp/esp-idf/export.sh
make firmware
make flash
```

WiFi and SNTP are off until you set an SSID — `idf.py -C firmware menuconfig`,
under **KAI watch**. The board has no RTC, so without them the clock counts
from reset and the WiFi corner draws dim, which is the reading that dimming is
for.

Neither target is part of `make check`: that gate has to stay runnable on a Mac
with nothing on it but Homebrew.

## Repository

| | |
|---|---|
| `ui/ui.c`, `ui/ui.h` | the clock face, the assistant face, and the morph between them |
| `ui/ui_font_digits_118.c`, `ui/ui_font_date_72.c`, `ui/ui_font_assistant_18.c` | generated by `tools/gen_fonts.py`; digits with tabular figures, and the two FontAwesome glyphs LVGL does not ship |
| `host/main.c`, `CMakeLists.txt` | the simulator |
| `host/voice.c`, `host/voice.h` | the voice loop: microphone, Parakeet, Chatterbox, speaker |
| `host/test_ui.c` | the headless renderer that `make test` runs |
| `lv_conf.h` | one LVGL configuration for both targets |
| `firmware/` | the ESP-IDF project |

## Documents

| | |
|---|---|
| [`SPEC.md`](SPEC.md) | the job, the guardrails, and what done means |
| [`CLAUDE.md`](CLAUDE.md) | how it is built, and why every line is the way it is |
| [`GLOSSARY.md`](GLOSSARY.md) | the words this project uses |

## License

MIT — see [`LICENSE`](LICENSE).
