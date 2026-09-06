/**
 * @file board.h
 * The Waveshare ESP32-S3-Touch-AMOLED-2.06 itself, handed to LVGL: the QSPI
 * panel as a display, the capacitive touch as a pointer.
 *
 * The device-side counterpart of the SDL window and mouse in the repo root's
 * main.c. Nothing behind ui.h ever sees either of them.
 */

#ifndef BOARD_H
#define BOARD_H

#include "lvgl.h"

/* The panel. The same two numbers as the simulator window, which is the whole
 * point of the simulator being 1:1 — see CLAUDE.md. */
#define BOARD_LCD_H_RES 410
#define BOARD_LCD_V_RES 502

/** Bring up the QSPI panel and give LVGL a display over it. */
lv_display_t * board_display_init(void);

/** Bring up the touch controller as a pointer device on `display`. */
lv_indev_t * board_touch_init(lv_display_t * display);

#endif /* BOARD_H */
