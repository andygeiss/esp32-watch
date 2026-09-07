# ESP32 Watch

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
|                     |       |                     |
|   ## ##     ## ##   |       |      .-.     .-.    |
|   ## ##     ## ##   |  -->  |     ( o )   ( o )   |
|   ## ##     ## ##   |       |      `-'     `-'    |
|                     |       |                     |
|        09/06        |       |                     |
|         SUN         |       |                     |
|                     |       |                     |
|                     |       |                     |
| spk             mic |       | spk             mic |
+---------------------+       +---------------------+
        "Hey Kai"  ------------->
        <-------------  "Tschüss", or 30 s of quiet
```

Amber on black, the way a VFD readout looks. Say `Hey Kai` and each digit group
pulls in to an eye over 400 ms while the digits fade off the front of it — a
69 px circle with a pupil in it and a catchlight in that, a good deal narrower
than the digits were, because it is the black left between the two that makes
them read as a pair. The eyes then blink, 140 ms every 3.6 s. A goodbye, or half a minute
with nothing said, runs it backwards. Nothing moves in between — only size and opacity
animate, which is why the hour and the minute have to stay two separate objects
placed symmetrically about the centre. They are the two eyes.

There is nothing to press on either face. The microphone is open from start-up
and the transcriber is the wake word: the watch listens for its own name, and
everything after it is a turn. **Both builds do this** — the simulator through
a Mac's microphone and a socket, the watch through its own ES7210 and ES8311 —
and the words they listen for are one list in `voice/turn.c`, compiled into
both. `Hey Kai` is the default; each side can be pointed at another name
without touching that file.

The answer comes from a chat model — `Qwen3.8-27B-oQ4e-mtp` unless told
otherwise — or is the question said back, which is what `echo` asks for. Where
the server is, the key it wants, which three models answer and what language
to speak in are settings on both builds: `.env` on the host, menuconfig on the
device, and the same `voice/turn.c` turns them into the same requests either
way.

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
| `voice/` | **portable, stricter.** `turn.c` is everything about a turn that is not a device — the words, the gate, the wire, the address, which models answer. The C standard library and *nothing else*, not even LVGL |
| `host/` | **host only.** `main.c` for the window, mouse, tick source and service loop; `voice.c` for the SDL microphone, the request over a socket or TLS, and the SDL speaker; `test_ui.c` for the headless renderer |
| `firmware/main/` | **device only.** The same jobs against the panel and the board's own codecs, plus the clock it cannot read for itself |

The directory is the boundary. There is no `src/`, because one folder called
"the source" would hold the first two of those and say nothing about the rule.
`lv_conf.h` and `lvgl/` stay at the root: LVGL looks for its configuration
beside its own checkout, so it cannot move into `ui/`.

A fact the UI needs but cannot reach for itself — the charge, the radio, the
microphone — crosses through one struct and one setter, `ui_status_t` and
`ui_status_set()`. The simulator fills it with a fake, the firmware fills it
from the hardware, and `ui.c` never learns which.

Nothing crosses the other way. Which face is up is another such fact, so it
arrives the same direction through `ui_view_set()`: the simulator hears the
watch's name in a transcript, the firmware will hear it on a codec, and `ui.c`
is only ever told.

Both sides hold the line up mechanically. On the host, `make check` compiles
each portable directory alone and fails if `nm` finds anything undefined but
`lv_*` and libc. In the firmware, the component that builds `ui/` requires
LVGL and nothing else and the one that builds `voice/turn.c` requires nothing
at all, so an ESP-IDF header in either is not on its include path.

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
brew install cmake pkg-config sdl2 openssl@3
git clone --depth 1 --branch v9.4.0 https://github.com/lvgl/lvgl.git lvgl
make run
```

`lvgl/` is gitignored, so a fresh clone needs that one command. The assistant
speaks once `voices/kai.opus` and `voices/kai.txt` are in place — the
synthesiser has no voice of its own and clones that clip; without them the
watch runs and the two bottom corners stay dim. Point it at a speech server
in `.env`, which `make run` sources and which is gitignored — a key compiled
in is a key committed:

```sh
cp .env.example .env    # then fill in the key
```

`.env.example` is the whole list with every default written next to it;
`.env` is gitignored because one of the lines is a secret.

```sh
WATCH_VOICE_URL="https://omlx.ai-at-home.de"   # http://127.0.0.1:8000 by default
WATCH_VOICE_KEY="..."                          # no key, no Authorization header
WATCH_WAKE_PHRASE="hey ada|hey adah|hi ada"    # empty is `Hey Kai` and five spellings
```

The quotes are not decoration. `make run` sources the file, so an unquoted
value with a space or a `|` in it is not an assignment at all —
`WATCH_WAKE_PHRASE=hey ada|hey adah` runs `ada` in a pipeline and leaves the
variable holding `hey`. The start-up log says what the watch actually took.

`WATCH_STT_MODEL`, `WATCH_BRAIN_MODEL`, `WATCH_TTS_MODEL`, `WATCH_LANGUAGE`
and `WATCH_WAKE_PHRASE` are the rest of the list, and empty means the default
written down in `voice/turn.h` — `Qwen3.8-27B-oQ4e-mtp` for the brain, so the
watch thinks unless `WATCH_BRAIN_MODEL=echo` tells it to say the question back
instead. `make` on its
own is `make check`: the `lv_conf.h` liveness grep, the build, the boundary
check, and 83 assertions rendered into a byte array by `test_ui.c` — geometry,
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

WiFi, SNTP and the assistant are off until you configure them — `idf.py -C
firmware menuconfig`, under **ESP32 Watch**. The board has no RTC, so without an
SSID the clock counts from reset and the WiFi corner draws dim, which is the
reading that dimming is for. The assistant needs that SSID, a speech server
URL it can reach, and `voices/kai.opus` in the tree when the firmware is
built; without any one of them the watch is a clock. `CONFIG_WATCH_VOICE_URL`
takes a whole address — `https://omlx.ai-at-home.de` goes through TLS against
the certificate bundle ESP-IDF compiles in, `http://192.168.1.20:8000` does
not — and `CONFIG_WATCH_VOICE_KEY`, the three model names, the language and
the wake phrase are the rest of the same menu. Empty means the default in
every one of them, and `echo` in the brain means the question said back.

Neither target is part of `make check`: that gate has to stay runnable on a Mac
with nothing on it but Homebrew.

## Repository

| | |
|---|---|
| `ui/ui.c`, `ui/ui.h` | the clock face, the assistant face, and the morph between them |
| `ui/ui_font_digits_118.c`, `ui/ui_font_date_72.c`, `ui/ui_font_assistant_18.c` | generated by `tools/gen_fonts.py`; digits with tabular figures, and the two FontAwesome glyphs LVGL does not ship |
| `voice/turn.c`, `voice/turn.h` | the portable half of the voice loop: the words, the gate, the JSON, the WAV, the address, the models, all three request bodies |
| `voice/voice.h` | the five functions each platform's voice loop implements |
| `host/main.c`, `CMakeLists.txt` | the simulator |
| `host/voice.c` | its voice loop: SDL microphone, a hand-written request over a socket or OpenSSL, SDL speaker |
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
