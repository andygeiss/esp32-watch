# SPEC

**Job.** Build and look at the KAI watch UI on macOS, at the exact panel size,
without flashing hardware.

**Why.** Reflashing an ESP32-S3 for every layout tweak makes the edit–look loop
slow enough that UI work stops happening. The pain belongs to whoever is
building the watch face.

**Guardrails.**

- No libraries in the simulator beyond LVGL and SDL2. On the device,
  ESP-IDF plus the two drivers for this board's panel and touch
  controller, and nothing else.
- LVGL stays pinned to a release tag. Never `master`.
- `ui.c` / `ui.h` must not reference SDL or ESP-IDF; they compile unchanged into
  the firmware.
- The simulator window stays 410 x 502, 1:1.
- The hour and minute groups stay separate objects, symmetric about the
  centre: they are the assistant's two eyes.

**Done means.**

- `make run` opens a 410 x 502 window showing the time, the date and a
  `Hey Kai` button in amber on black, and that button morphs the digits into
  the assistant's two eyes and back.
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
  `lv_conf.h`, with `ui.c` and the three fonts compiled from where they sit.
- The panel, the touch and the platform readouts reach the UI only through
  `ui_build()` and `ui_status_set()`.
- `make check` still passes on a machine with no ESP-IDF on it.
