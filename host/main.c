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

/* The view follows the voice loop, which runs on a thread of its own, so it
 * has to be polled: ui.c may only be touched from here. Fast enough that the
 * eyes come up as the wake phrase lands, rather than up to a second later
 * with the status readouts. */
#define VIEW_PERIOD_MS 100

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

/* Whether the assistant is awake, which is the whole of what decides which
 * face is up. ui_view_set() is idempotent, so this hands it the same answer
 * ten times a second and only the answer that changed is a morph. */
static void view_tick(lv_timer_t * timer)
{
    LV_UNUSED(timer);

    ui_view_set(voice_awake());
}

/* The close box, seen before LVGL sees it.
 *
 * LVGL's SDL backend answers SDL_QUIT with SDL_Quit() and then exit(), in that
 * order — lv_sdl_window.c. SDL_Quit() takes the audio devices down, and the
 * voice thread is sitting inside SDL_DequeueAudio() at the time, so the
 * process dies of a bus error before any atexit() handler runs. Registering
 * voice_stop() there is not early enough: it has to happen while SDL is still
 * up.
 *
 * SDL_PumpEvents() fills the queue and the peek leaves the event in it, so
 * LVGL still finds SDL_QUIT on its own poll a moment later and quits exactly
 * as it did before. All this does is close the microphone first. It costs a
 * moment — up to a second if a request is in flight, which is the socket's own
 * timeout. */
static void quit_first(void)
{
    SDL_Event quit;

    SDL_PumpEvents();
    if (SDL_PeepEvents(&quit, 1, SDL_PEEKEVENT, SDL_QUIT, SDL_QUIT) > 0) voice_stop();
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
    lv_timer_ready(lv_timer_create(view_tick, VIEW_PERIOD_MS, NULL));

    /* There is nothing to press: the loop listens for the watch's name from
     * the moment it starts, and saying it is what morphs the digits into
     * eyes. Without voices/kai.opus the loop never starts, so the watch stays
     * a clock and the two bottom corners stay dim.
     *
     * LV_SDL_DIRECT_EXIT means closing the window calls exit() from inside
     * lv_timer_handler(), so the loop below is never left. atexit() is the
     * backstop for every other way out of this process; the close box itself
     * is quit_first()'s job, because by the time atexit() runs SDL is already
     * gone. voice_stop() takes both, and is harmless the second time. */
    voice_start();
    atexit(voice_stop);

    /* LV_SDL_DIRECT_EXIT is 1 in lv_conf.h: closing the window ends the process. */
    for (;;) {
        uint32_t idle_ms;

        quit_first();
        idle_ms = lv_timer_handler();
        if (idle_ms < 1) idle_ms = 1;
        if (idle_ms > FRAME_INTERVAL_MS) idle_ms = FRAME_INTERVAL_MS;
        SDL_Delay(idle_ms);
    }
}
