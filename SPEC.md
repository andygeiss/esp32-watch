# SPEC

**Job.** Build and look at the ESP32 Watch UI on macOS, at the exact panel size,
without flashing hardware.

**Why.** Reflashing an ESP32-S3 for every layout tweak makes the edit–look loop
slow enough that UI work stops happening. The pain belongs to whoever is
building the watch face.

**Guardrails.**

- No libraries in the simulator beyond LVGL and SDL2. On the device,
  ESP-IDF plus the two drivers for this board's panel and touch
  controller, and nothing else.
- LVGL stays pinned to a release tag. Never `master`.
- `ui/` and `voice/` must not reference SDL or ESP-IDF; both compile unchanged
  into the firmware, and `voice/` may not reference LVGL either. Host-only
  code lives in `host/`; the directory is the boundary.
- The simulator window stays 410 x 502, 1:1.
- The hour and minute groups stay separate objects, symmetric about the
  centre: they are the assistant's two eyes.

**Done means.**

- `make run` opens a 410 x 502 window showing the time and the date in amber
  on black, and saying `Hey Kai` morphs the digits into the assistant's two
  eyes; a goodbye or half a minute of quiet morphs them back. Neither face has
  anything on it to press.
- `CLAUDE.md` carries the pinned LVGL version, the `lv_conf.h` settings and
  their reasons, the build and run commands, the host/device boundary rule and
  the panel specification.
- The tree is a git repository with `build/` and `lvgl/` ignored.

# The firmware

**Job.** Run the same `ui.c` on the board it was drawn for.

**Why.** The simulator is only worth having if what it shows is what the panel
shows. A port that diverges — a second LVGL, a second `lv_conf.h`, a second
layout — turns the fast loop back into guesswork.

**Done means.**

- `make firmware` builds `firmware/` against the repo root's own `lvgl/` and
  `lv_conf.h`, with `ui/`'s four files compiled from where they sit.
- The panel, the touch, the codecs and the platform readouts reach the UI only
  through `ui_build()`, `ui_status_set()` and `ui_view_set()`, and nothing
  reaches back out of it.
- The watch hears its own name on the board's own microphone and answers
  through its own speaker, against the same two services the simulator uses
  and the same words in `voice/turn.c`.
- `make check` still passes on a machine with no ESP-IDF on it.

# The voice loop

**Job.** Hear the watch's name, then hear a sentence, transcribe it and say it
back, so the two views and the microphone and speaker corners all show
something true.

**Why.** The two readouts that stand for the microphone and the speaker were
invented by a timer. A loop against the real speech services is what turns
them into a reading — and it is the shortest path through the whole pipeline,
so a turn that breaks there breaks everywhere. It runs on both: the simulator
so the loop can be worked on without flashing, the watch because that is the
point.

**Done means.**

- Saying `Hey Kai` wakes the assistant: the digits morph into eyes and the
  next sentence comes back out of the speakers, with the two corners lit while
  it does. Saying `tschüss`, or saying nothing for 30 s, goes back to the
  clock.
- `host/voice.c` uses nothing but SDL2 and the C standard library: no HTTP
  library, no JSON library.
- `make check` still passes with no oMLX server running and no `voices/` in
  the tree.
