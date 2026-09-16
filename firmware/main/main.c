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
#include <math.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "esp_log.h"
#include <string.h>

static const char * TAG = "main";

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

/* The clock at boot: the RTC's, if it has one. SNTP corrects both later,
 * through net.c, and the timezone is applied the same way either way. */
static void clock_from_rtc(void)
{
    time_t utc;
    struct timeval now = { 0 };

    if (!board_rtc_init()) return;
    if (!board_rtc_read(&utc)) {
        ESP_LOGI(TAG, "the RTC has never been set — the clock runs from boot until SNTP");
        return;
    }
    now.tv_sec = utc;
    settimeofday(&now, NULL);
    ESP_LOGI(TAG, "clock set from the RTC");
}

/* Raise to wake. The panel is lit for WAKE_HOLD_MS after a tap, or after the
 * watch moves and is then held face up — the two things a wrist does on its
 * way to being read — and goes dark otherwise. "Face up" is the panel's
 * normal within WAKE_FACE_G of straight up; "moves" is the acceleration
 * vector changing by WAKE_MOVE_G between two samples a tenth of a second
 * apart, which a watch on a desk never does and a lifted wrist always does.
 * Nothing under ui.h knows: rendering carries on, so the panel lights on the
 * present frame.
 *
 * The sensor's axes were read off the first run, with the board flat on a
 * desk face up: it read +0.13, +0.18, -1.01, so up is the third axis, and
 * negative — the sensor is mounted with its Z pointing into the wrist. */
#define WAKE_PERIOD_MS 100
#define WAKE_HOLD_MS   8000
#define WAKE_MOVE_G    0.25f
#define WAKE_FACE_G    0.6f
#define WAKE_UP_AXIS   2
#define WAKE_UP_SIGN   -1.0f

static void wake_tick(lv_timer_t * timer)
{
    static float    last[3];
    static bool     have_last;
    static uint32_t lit_until = WAKE_HOLD_MS; /* the boot face is worth a look */
    static bool     lit = true;
    float g[3];
    uint32_t now = tick_ms();
    bool moved = false, face_up = true;

    LV_UNUSED(timer);

    if (board_motion_read(g)) {
        if (have_last) {
            float dx = g[0] - last[0], dy = g[1] - last[1], dz = g[2] - last[2];
            moved = sqrtf(dx * dx + dy * dy + dz * dz) > WAKE_MOVE_G;
        }
        memcpy(last, g, sizeof last);
        have_last = true;
        face_up = g[WAKE_UP_AXIS] * WAKE_UP_SIGN > WAKE_FACE_G;
    }

    if (board_touch_take_tap() || (moved && face_up)) lit_until = now + WAKE_HOLD_MS;
    if (!face_up) lit_until = 0;

    if (((int32_t) (lit_until - now) > 0) != lit) {
        lit = !lit;
        ESP_LOGI(TAG, "panel %s", lit ? "lit" : "dark");
    }
    board_display_sleep(!lit);
}

/* What this board can answer: all of it, now.
 *
 * The radio is real, the microphone and the speaker are voice.c's ES7210 and
 * ES8311, and the charge is the AXP2101's own gauge — read once a second,
 * three bytes over I2C. With no gauge found, or no battery on it, the reading
 * stays negative and the corner reads `--%`, which is the honest answer and
 * one the UI already draws. None of it reaches into ui.c. That is the point
 * of the struct. */
static void status_tick(lv_timer_t * timer)
{
    bool charging;
    ui_status_t status = {
        .wifi_up   = net_is_up(),
        .listening = voice_listening(),
        .speaking  = voice_speaking(),
    };

    LV_UNUSED(timer);

    status.battery_pct = board_battery_read(&charging);
    status.charging = charging;

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
    board_battery_init(); /* on the bus the touch just made */
    clock_from_rtc();
    board_motion_init();

    ui_build();
    lv_timer_ready(lv_timer_create(status_tick, STATUS_PERIOD_MS, NULL));
    lv_timer_ready(lv_timer_create(view_tick, VIEW_PERIOD_MS, NULL));
    lv_timer_ready(lv_timer_create(wake_tick, WAKE_PERIOD_MS, NULL));

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
