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

/**
 * Told which view the button just switched to: true on the way to the
 * assistant, false on the way back to the clock.
 *
 * The other direction of the crossing ui_status_set() makes. The platform
 * hands the UI facts it cannot reach; this hands the platform the one thing
 * only the UI knows, which is that somebody pressed the button. ui.c calls it
 * and never learns what it does — the simulator opens a microphone, the
 * firmware will wake a codec, and neither appears here.
 */
typedef void (*ui_view_cb_t)(bool assistant);

/** Build the clock face on the active screen. Call once, after lv_init(). */
void ui_build(void);

/** Register the callback above. NULL, the default, is nothing to tell. */
void ui_on_view_change(ui_view_cb_t cb);

/** Hand the UI the current platform status. Safe to call before ui_build(). */
void ui_status_set(const ui_status_t * status);

#endif /* UI_H */
