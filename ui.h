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
    bool listening;   /**< the wake-word engine has the microphone open */
} ui_status_t;

/** Build the clock face on the active screen. Call once, after lv_init(). */
void ui_build(void);

/** Hand the UI the current platform status. Safe to call before ui_build(). */
void ui_status_set(const ui_status_t * status);

#endif /* UI_H */
