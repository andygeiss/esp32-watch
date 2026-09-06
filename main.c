/**
 * @file main.c
 * Host entry point for the KAI watch simulator.
 *
 * This file is the whole host layer: SDL window, input devices, tick source
 * and the LVGL service loop. The firmware replaces this file and keeps ui.c
 * untouched, so SDL symbols must not leak past ui_build().
 */

#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>

#include "lvgl/lvgl.h"
#include "ui.h"

/* Waveshare ESP32-S3-Touch-AMOLED-2.06 panel, at 1:1 scale so that what
 * lands on screen here is what lands on the device. */
#define PANEL_WIDTH  410
#define PANEL_HEIGHT 502

/* Ceiling on the idle sleep (~60 Hz), so input stays responsive even when
 * no LVGL timer is due. */
#define FRAME_INTERVAL_MS 16

int main(void)
{
    SDL_SetMainReady();

    lv_init();
    lv_tick_set_cb((lv_tick_get_cb_t) SDL_GetTicks);

    lv_display_t * display = lv_sdl_window_create(PANEL_WIDTH, PANEL_HEIGHT);
    if (display == NULL) {
        SDL_Log("lv_sdl_window_create failed: %s", SDL_GetError());
        return 1;
    }
    lv_sdl_window_set_title(display, "KAI watch simulator");

    lv_sdl_mouse_create(); /* stands in for the capacitive touch panel */
    lv_sdl_keyboard_create();

    ui_build();

    /* LV_SDL_DIRECT_EXIT is 1 in lv_conf.h: closing the window ends the process. */
    for (;;) {
        uint32_t idle_ms = lv_timer_handler();
        if (idle_ms < 1) idle_ms = 1;
        if (idle_ms > FRAME_INTERVAL_MS) idle_ms = FRAME_INTERVAL_MS;
        SDL_Delay(idle_ms);
    }
}
