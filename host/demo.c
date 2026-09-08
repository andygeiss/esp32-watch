/**
 * @file demo.c
 * Renders both faces and the morph between them into a strip of frames, for
 * the animation at the top of README.md.
 *
 * The third host. main.c drives the UI with an SDL window, a real clock and a
 * microphone; test_ui.c drives it with a byte array it makes assertions
 * about; this one drives it with a byte array it hands to tools/gen_demo.py,
 * which turns the frames into a GIF. The fake tick source is the test's, for
 * the test's reason: a 400 ms morph and a 3.6 s blink pause cost nothing, and
 * a frame can be taken part way through either.
 *
 * It plays what the platform does rather than anything of its own — the two
 * calls host/main.c's timers make, ui_view_set() and ui_status_set(), in the
 * order a turn makes them. So a face that is wrong here is wrong on the
 * watch, which is the only reason a picture of it is worth checking in.
 *
 * Host-only, like main.c: the firmware builds neither. It writes to the path
 * given as its one argument — a 12-byte header of width, height and frame
 * count, each a little-endian uint32, then for each frame 4 more bytes of
 * milliseconds followed by width * height * 3 bytes of RGB. A frame identical
 * to the one before it is folded into that one's duration rather than written
 * again, which is most of them: nothing on either face moves except during a
 * morph or a blink.
 */

#include <stdio.h>
#include <string.h>

#include "lvgl/lvgl.h"
#include "ui.h"

#define PANEL_WIDTH  410
#define PANEL_HEIGHT 502

/* 25 frames a second, which is what the GIF plays at. */
#define STEP_MS 40

static uint8_t buf[PANEL_WIDTH * PANEL_HEIGHT * 2];
static uint8_t written[sizeof(buf)];
static uint32_t fake_tick;
static lv_display_t * display;

static FILE * out;
static uint32_t frames;
static long duration_at; /* where the last frame's duration went, to add to it */

static uint32_t tick_get(void)
{
    return fake_tick;
}

/* Without a flush callback LVGL leaves the display marked as flushing and the
 * second refresh spins in wait_for_flushing forever — the same trap test_ui.c
 * documents. Nothing needs copying: the render mode is FULL, so `buf` is
 * already the frame. */
static void flush_cb(lv_display_t * d, const lv_area_t * area, uint8_t * px_map)
{
    LV_UNUSED(area);
    LV_UNUSED(px_map);
    lv_display_flush_ready(d);
}

static void put32(uint32_t value)
{
    uint8_t bytes[4];

    bytes[0] = (uint8_t) value;
    bytes[1] = (uint8_t) (value >> 8);
    bytes[2] = (uint8_t) (value >> 16);
    bytes[3] = (uint8_t) (value >> 24);
    fwrite(bytes, 1, sizeof(bytes), out);
}

/* One frame, unless it is the one already on disk, in which case that one
 * simply lasts longer. */
static void capture(void)
{
    uint32_t i;

    if (frames > 0 && memcmp(buf, written, sizeof(buf)) == 0) {
        long here = ftell(out);
        uint32_t was;
        uint8_t bytes[4];

        fseek(out, duration_at, SEEK_SET);
        fread(bytes, 1, sizeof(bytes), out);
        was = (uint32_t) bytes[0] | ((uint32_t) bytes[1] << 8) |
              ((uint32_t) bytes[2] << 16) | ((uint32_t) bytes[3] << 24);
        fseek(out, duration_at, SEEK_SET);
        put32(was + STEP_MS);
        fseek(out, here, SEEK_SET);
        return;
    }

    duration_at = ftell(out);
    put32(STEP_MS);
    for (i = 0; i < PANEL_WIDTH * PANEL_HEIGHT; i++) {
        uint16_t pixel = (uint16_t) (buf[i * 2] | (buf[i * 2 + 1] << 8));
        uint8_t rgb[3];

        /* RGB565 back out to eight bits a channel, the way the panel would
         * show it: amber 0xFFB000 reads back as 0xFFB200 and that is the
         * round trip, not a bug. */
        rgb[0] = (uint8_t) (((pixel >> 11) & 0x1F) * 255 / 31);
        rgb[1] = (uint8_t) (((pixel >> 5) & 0x3F) * 255 / 63);
        rgb[2] = (uint8_t) ((pixel & 0x1F) * 255 / 31);
        fwrite(rgb, 1, sizeof(rgb), out);
    }
    memcpy(written, buf, sizeof(buf));
    frames++;
}

/* Let the UI run, taking a frame every step. lv_timer_handler() draws when
 * something is dirty and not otherwise, and the render mode is FULL, so `buf`
 * holds the last frame either way and a still stretch costs one frame. */
static void hold(uint32_t ms)
{
    uint32_t t;

    for (t = 0; t < ms; t += STEP_MS) {
        fake_tick += STEP_MS;
        lv_timer_handler();
        capture();
    }
}

/* What the platform knows, as host/main.c's status timer would hand it over.
 * The charge and the radio stay put; the two the assistant owns are the ones
 * the demo is showing. */
static void status_set(bool listening, bool speaking)
{
    ui_status_t status = { .battery_pct = 62, .wifi_up = true };

    status.listening = listening;
    status.speaking = speaking;
    ui_status_set(&status);
}

int main(int argc, char ** argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <frames-file>\n", argv[0]);
        return 2;
    }
    out = fopen(argv[1], "w+b");
    if (out == NULL) {
        perror(argv[1]);
        return 1;
    }

    lv_init();
    lv_tick_set_cb(tick_get);

    display = lv_display_create(PANEL_WIDTH, PANEL_HEIGHT);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, buf, NULL, sizeof(buf), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, flush_cb);

    put32(PANEL_WIDTH);
    put32(PANEL_HEIGHT);
    put32(0); /* patched once the count is known */

    ui_build();

    /* The clock, then a turn: the watch hears its name and wakes with the
     * microphone open, answers with the speaker open, and a goodbye puts it
     * back. Each wait is long enough to reach the blink that falls in it —
     * 3.6 s after the eye last finished opening. */
    status_set(false, false);
    hold(1400);

    ui_view_set(true);
    status_set(true, false);
    hold(400);  /* the morph */
    hold(3800); /* settled, listening, and the first blink */

    status_set(false, true);
    hold(3800); /* answering, and the second */

    status_set(true, false);
    hold(700);

    ui_view_set(false);
    status_set(false, false);
    hold(400); /* the morph back */
    hold(1400);

    fseek(out, 8, SEEK_SET);
    put32(frames);
    fclose(out);

    fprintf(stderr, "%u frames\n", frames);
    return 0;
}
