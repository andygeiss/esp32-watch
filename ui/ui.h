/**
 * @file ui.h
 * Portable watch UI — the half that also runs on the device.
 *
 * Nothing behind this header may reference SDL or ESP-IDF symbols. It is
 * built unchanged for the host simulator and for the ESP32-S3 firmware, so
 * LVGL and the C standard library are the only things it may call. All
 * host-specific setup belongs in main.c.
 */

#ifndef UI_H
#define UI_H

#include <stdbool.h>

/**
 * What the platform knows and the UI cannot reach for itself. The corner
 * readouts are drawn from this: the firmware fills it from the fuel gauge,
 * the radio and the wake-word engine, and the simulator fakes it.
 */
typedef struct {
    int  battery_pct; /**< 0-100, or negative while it is not known yet */
    bool charging;
    bool wifi_up;
    /* The assistant's two ends. Independent on purpose: with acoustic echo
     * cancellation a real one listens while it talks, which is what lets you
     * interrupt it. A half-duplex firmware simply never sets both. */
    bool listening;   /**< the microphone is open */
    bool speaking;    /**< the speaker is playing */
} ui_status_t;

/** Build the clock face on the active screen. Call once, after lv_init(). */
void ui_build(void);

/**
 * Show the assistant's face, or go back to the clock.
 *
 * There is no button on either face. Waking is a thing the watch is told
 * about, not a thing it can decide: the platform hears its name and says so
 * here, and ui.c never learns what did the hearing — the simulator runs a
 * transcriber over everything the room says, the firmware will run a wake
 * word on a codec. The same direction ui_status_set() crosses in, which is
 * why nothing crosses the other way any more.
 *
 * Idempotent: being told the view it is already in is not a morph. Safe to
 * call before ui_build(), and safe to call every frame.
 */
void ui_view_set(bool assistant);

/** Hand the UI the current platform status. Safe to call before ui_build(). */
void ui_status_set(const ui_status_t * status);

#endif /* UI_H */
