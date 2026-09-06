# SPEC

**Job.** Build and look at the KAI watch UI on macOS, at the exact panel size,
without flashing hardware.

**Why.** Reflashing an ESP32-S3 for every layout tweak makes the edit–look loop
slow enough that UI work stops happening. The pain belongs to whoever is
building the watch face.

**Guardrails.**

- No libraries beyond LVGL and SDL2.
- LVGL stays pinned to a release tag. Never `master`.
- `ui.c` / `ui.h` must not reference SDL or ESP-IDF; they compile unchanged into
  the firmware.
- The simulator window stays 410 x 502, 1:1.
- The morph animation is not scaffolded yet.

**Done means.**

- `cmake --build build -j && ./build/kai_sim` opens a 410 x 502 window showing
  the current time in amber on black.
- `CLAUDE.md` carries the pinned LVGL version, the `lv_conf.h` settings and
  their reasons, the build and run commands, the host/device boundary rule and
  the panel specification.
- The tree is a git repository with `build/` and `lvgl/` ignored.
