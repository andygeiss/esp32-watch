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

#include <stdlib.h>

#include "lvgl/lvgl.h"
#include "ui.h"
#include "voice.h"

/* Waveshare ESP32-S3-Touch-AMOLED-2.06 panel, at 1:1 scale so that what
 * lands on screen here is what lands on the device. */
#define PANEL_WIDTH  410
#define PANEL_HEIGHT 502

/* Ceiling on the idle sleep (~60 Hz), so input stays responsive even when
 * no LVGL timer is due. */
#define FRAME_INTERVAL_MS 16

#define STATUS_PERIOD_MS 1000

/* The charge and the radio are still a stand-in: reading either on a Mac
 * means a framework this simulator has no business linking, and neither is
 * what the watch is for. The charge walks down to nearly empty and charges
 * back up, far too fast to be real, so every state the readout can show goes
 * past while you watch.
 *
 * The bottom two corners are no longer faked. They are the voice loop's own
 * microphone and speaker, read here and handed straight on — which is the
 * point of the struct: ui.c is told the same two booleans either way and
 * never learns that one pair is invented and the other is not. */
static void status_tick(lv_timer_t * timer)
{
    static ui_status_t status = { .battery_pct = 87, .wifi_up = true };

    LV_UNUSED(timer);

    if (status.charging) {
        status.battery_pct++;
        if (status.battery_pct >= 100) status.charging = false;
    }
    else {
        status.battery_pct--;
        if (status.battery_pct <= 5) status.charging = true;
    }

    /* Separate flags rather than a mode, so a loop with echo cancellation can
     * raise both at once. This one is half duplex and never does. */
    status.listening = voice_listening();
    status.speaking = voice_speaking();

    ui_status_set(&status);
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

    /* The button is what wakes the assistant, so the two arrive together: the
     * digits morph into eyes and the microphone opens on the same press. The
     * loop is quiet without voices/kai.opus, and the corners stay dim.
     *
     * LV_SDL_DIRECT_EXIT means closing the window calls exit() from inside
     * lv_timer_handler(), so the thread is stopped from there rather than
     * after the loop, which is not reachable. */
    ui_on_view_change(voice_listen);
    voice_start();
    atexit(voice_stop);

    /* LV_SDL_DIRECT_EXIT is 1 in lv_conf.h: closing the window ends the process. */
    for (;;) {
        uint32_t idle_ms = lv_timer_handler();
        if (idle_ms < 1) idle_ms = 1;
        if (idle_ms > FRAME_INTERVAL_MS) idle_ms = FRAME_INTERVAL_MS;
        SDL_Delay(idle_ms);
    }
}
