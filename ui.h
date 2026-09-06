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

/** Build the clock face on the active screen. Call once, after lv_init(). */
void ui_build(void);

#endif /* UI_H */
