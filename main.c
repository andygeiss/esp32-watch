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

#define STATUS_PERIOD_MS 1000

/* The host has no fuel gauge, no radio and no microphone, so the corner
 * readouts get a stand-in: the charge walks down to nearly empty, charges
 * back up, and the wake-word engine's microphone opens now and then. It moves
 * far too fast to be real, on purpose — every state a readout can show goes
 * past while you watch. The firmware replaces this with the real readings. */
static void status_tick(lv_timer_t * timer)
{
    static ui_status_t fake = { .battery_pct = 87, .wifi_up = true };
    static uint32_t ticks;

    LV_UNUSED(timer);

    if (fake.charging) {
        fake.battery_pct++;
        if (fake.battery_pct >= 100) fake.charging = false;
    }
    else {
        fake.battery_pct--;
        if (fake.battery_pct <= 5) fake.charging = true;
    }

    fake.listening = (ticks++ % 10) < 2;

    ui_status_set(&fake);
}

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
    lv_timer_ready(lv_timer_create(status_tick, STATUS_PERIOD_MS, NULL));

    /* LV_SDL_DIRECT_EXIT is 1 in lv_conf.h: closing the window ends the process. */
    for (;;) {
        uint32_t idle_ms = lv_timer_handler();
        if (idle_ms < 1) idle_ms = 1;
        if (idle_ms > FRAME_INTERVAL_MS) idle_ms = FRAME_INTERVAL_MS;
        SDL_Delay(idle_ms);
    }
}
