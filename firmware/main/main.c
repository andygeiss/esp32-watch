/**
 * @file main.c
 * Device entry point for the ESP32 Watch.
 *
 * The firmware half of the host/device boundary: this file is to the panel
 * what the repo root's main.c is to the SDL window. Both bring up a display,
 * a pointer device and a tick source, call ui_build() once, and then do
 * nothing but drive LVGL's timers and feed ui_status_set(). Nothing behind
 * ui.h learns which of the two it woke up on.
 */

#include "board.h"
#include "net.h"
#include "ui.h"
#include "voice.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

/* Ceiling on the idle sleep (~60 Hz), so touch stays responsive even when no
 * LVGL timer is due. The simulator's loop has the same number in it. */
#define FRAME_INTERVAL_MS 16

#define STATUS_PERIOD_MS 1000

/* The view follows the voice loop, which runs in a task of its own, so it has
 * to be polled: ui.c may only be touched from this one. Fast enough that the
 * eyes come up as the wake phrase lands, rather than up to a second later
 * with the status readouts. The simulator's loop has the same two numbers. */
#define VIEW_PERIOD_MS 100

static uint32_t tick_ms(void)
{
    return (uint32_t) (esp_timer_get_time() / 1000);
}

/* What this board can answer, and what it cannot.
 *
 * The radio is real, and so are the microphone and the speaker now that
 * voice.c has the ES7210 and the ES8311. The charge is not: there is no fuel
 * gauge on this board, so battery_pct stays negative and the corner reads
 * `--%` — the honest answer, and one the UI already draws. That last one is a
 * single line here when a gauge arrives, and none of it reaches into ui.c.
 * That is the point of the struct. */
static void status_tick(lv_timer_t * timer)
{
    ui_status_t status = {
        .battery_pct = -1,
        .charging    = false,
        .wifi_up     = net_is_up(),
        .listening   = voice_listening(),
        .speaking    = voice_speaking(),
    };

    LV_UNUSED(timer);

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

void app_main(void)
{
    lv_init();
    lv_tick_set_cb(tick_ms);

    lv_display_t * display = board_display_init();
    board_touch_init(display);

    ui_build();
    lv_timer_ready(lv_timer_create(status_tick, STATUS_PERIOD_MS, NULL));
    lv_timer_ready(lv_timer_create(view_tick, VIEW_PERIOD_MS, NULL));

    /* After the face is up, so the first frame does not wait on a radio.
     *
     * There is nothing to press: the loop listens for the watch's name for as
     * long as there is a network and a server to ask, and saying it is what
     * morphs the digits into eyes. Without an SSID, without a server, or
     * without the reference clip compiled in, none of it starts and the watch
     * is a clock with two dim corners. voice_start() has to come after
     * board_touch_init(), which is what makes the I2C bus the codecs sit on. */
    net_start();
    voice_start();

    /* Everything that touches LVGL runs from this one task — the timers, the
     * touch read, status_tick — so there is no lock to take and none to
     * forget. The one thing that does not is net.c, and it only ever sets a
     * flag this task reads. */
    for (;;) {
        uint32_t idle_ms = lv_timer_handler();
        if (idle_ms < 1) idle_ms = 1;
        if (idle_ms > FRAME_INTERVAL_MS) idle_ms = FRAME_INTERVAL_MS;
        vTaskDelay(pdMS_TO_TICKS(idle_ms));
    }
}
