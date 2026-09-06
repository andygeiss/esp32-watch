/**
 * @file main.c
 * Device entry point for the KAI watch.
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

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

/* Ceiling on the idle sleep (~60 Hz), so touch stays responsive even when no
 * LVGL timer is due. The simulator's loop has the same number in it. */
#define FRAME_INTERVAL_MS 16

#define STATUS_PERIOD_MS 1000

static uint32_t tick_ms(void)
{
    return (uint32_t) (esp_timer_get_time() / 1000);
}

/* What this board can answer, and what it cannot answer yet.
 *
 * The radio is real. The charge is not: there is no fuel gauge on this board,
 * so battery_pct stays negative and the corner reads `--%` — the honest
 * answer, and one the UI already draws. listening and speaking wait on the
 * audio path, the ES8311 codec and a wake-word engine, and stay dim until
 * there is something behind them. Each is one line here when it arrives; none
 * of it reaches into ui.c.
 *
 * The assistant's face waits on the same thing. With no button on either face
 * and nothing on this board that can hear its name yet, ui_view_set() has no
 * caller here and the watch stays a clock — one more line, beside these, once
 * ESP-SR is listening. The simulator already does the whole of it; see
 * host/voice.c. */
static void status_tick(lv_timer_t * timer)
{
    ui_status_t status = {
        .battery_pct = -1,
        .charging    = false,
        .wifi_up     = net_is_up(),
        .listening   = false,
        .speaking    = false,
    };

    LV_UNUSED(timer);

    ui_status_set(&status);
}

void app_main(void)
{
    lv_init();
    lv_tick_set_cb(tick_ms);

    lv_display_t * display = board_display_init();
    board_touch_init(display);

    ui_build();
    lv_timer_ready(lv_timer_create(status_tick, STATUS_PERIOD_MS, NULL));

    /* After the face is up, so the first frame does not wait on a radio. */
    net_start();

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
