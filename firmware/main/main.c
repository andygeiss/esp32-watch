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
 * watch moves the way a wrist does when it is raised to be read, and goes
 * dark after that, or at once when the glass is turned toward the ground.
 * Nothing under ui.h knows: rendering carries on, so the panel lights on the
 * present frame.
 *
 * Moving is the smoothed acceleration vector — the mean of the last
 * WAKE_SMOOTH samples — having changed by more than WAKE_MOVE_G against the
 * same mean WAKE_SPAN samples earlier. It used to be two raw samples a tenth
 * of a second apart changing by 0.25 g, and that lit the panel for a few
 * millimetres: a single sample catches the spike of a knock or a twitch, and
 * a spike is exactly as large as a raised wrist for the one sample it lasts.
 * The mean takes the spike out, and the half-second span asks for the
 * movement to have gone somewhere — which a raised arm does, mostly by
 * turning the glass toward the eyes, and a jiggle does not. Every lit panel
 * logs the change that lit it, which is the number to tune against a wrist.
 *
 * WAKE_MOVE_G was read off a wrist, not guessed. Three raises of about 10 cm
 * peaked at 0.62, 0.85 and 1.08 g; small movements — a few millimetres, a
 * knock on the table, typing — at 0.42, 0.33 and 0.23 g. Half a g sits in
 * that gap. Reaching for a cup is as big as a raise and lights the panel,
 * which no threshold on size alone can help.
 *
 * There is deliberately no "face up" test. There was one, and it lit the
 * desk and not the wrist: on this chip a watch lying glass-up reads a full g
 * on Z, and a watch raised to be read reads about +1.0 on X and +0.2 on Z —
 * the glass faces the eyes, not the sky. So movement is the whole of the
 * wake, and orientation is only ever a reason to go dark: WAKE_DOWN_G on Z
 * is the glass turned over, which the desk reading says is +1. */
#define WAKE_PERIOD_MS 100
#define WAKE_HOLD_MS   8000
#define WAKE_SMOOTH    3     /* samples in each mean: 0.3 s */
#define WAKE_SPAN      5     /* samples between the two means: 0.5 s */
#define WAKE_MOVE_G    0.5f
#define WAKE_DOWN_G    0.6f
#define WAKE_DOWN_AXIS 2

static void wake_tick(lv_timer_t * timer)
{
    enum { N = WAKE_SMOOTH + WAKE_SPAN };
    static float    hist[N][3];     /* the last N samples, newest at `head` */
    static int      head, have;
    static uint32_t lit_until = WAKE_HOLD_MS; /* the boot face is worth a look */
    static bool     lit = true;
    float g[3], change = 0;
    uint32_t now = tick_ms();
    bool moved = false, face_down = false;

    LV_UNUSED(timer);

    if (board_motion_read(g)) {
        head = (head + 1) % N;
        memcpy(hist[head], g, sizeof g);
        if (have < N) have++;

        if (have == N) {
            float d[3] = { 0, 0, 0 };
            for (int i = 0; i < WAKE_SMOOTH; i++) {
                const float * recent = hist[(head - i + N) % N];
                const float * before = hist[(head - WAKE_SPAN - i + 2 * N) % N];
                for (int k = 0; k < 3; k++) d[k] += (recent[k] - before[k]) / WAKE_SMOOTH;
            }
            change = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            moved = change > WAKE_MOVE_G;
        }
        face_down = g[WAKE_DOWN_AXIS] > WAKE_DOWN_G;
    }

    if (board_touch_take_tap() || moved) lit_until = now + WAKE_HOLD_MS;
    if (face_down) lit_until = 0;
    /* A conversation keeps the panel lit, and with it the microphone: the
     * eyes stay up until the goodbye, and eight seconds after. */
    if (voice_awake()) lit_until = now + WAKE_HOLD_MS;

    if (((int32_t) (lit_until - now) > 0) != lit) {
        lit = !lit;
        if (lit && moved) ESP_LOGI(TAG, "panel lit, moved %.2f g", change);
        else ESP_LOGI(TAG, "panel %s", lit ? "lit" : "dark");
    }
    board_display_sleep(!lit);
}

/* What this board can answer: all of it, now.
 *
 * The radio is real, the microphone and the speaker are voice.c's ES7210 and
 * ES8311, and the charge is the battery's voltage off the AXP2101 — read once
 * a second, four bytes over I2C. With no power chip found, or no battery on
 * it, the reading stays negative and the corner reads `--%`, which is the
 * honest answer and one the UI already draws. None of it reaches into ui.c. That is the point
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
