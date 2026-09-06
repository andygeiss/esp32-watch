/**
 * @file test_ui.c
 * Renders ui.c into a plain buffer and checks what came out.
 *
 * The second host. main.c drives the UI with an SDL window and a real clock;
 * this one drives it with a byte array and a fake clock, so the layout, the
 * views, the morph, the blink and the status readouts can all be checked
 * without a screen — `screencapture` needs permissions this machine does not
 * grant, and a person looking at a window is not a gate.
 *
 * Host-only, like main.c: the firmware builds neither. `make test` runs it,
 * and a non-zero exit is a failure.
 */

#include <stdio.h>
#include <string.h>

#include "lvgl/lvgl.h"
#include "ui.h"

#define PANEL_WIDTH  410
#define PANEL_HEIGHT 502

/* The numbers ui.c derives its layout from. Spelled out again here on
 * purpose: a test that imports the constant cannot catch it changing. */
#define EDGE_MARGIN  32
#define GROUP_WIDTH  158
#define GROUP_HEIGHT 85
#define EYE_SIZE     120

/* Children of the screen, in the order ui_build() creates them. */
enum {
    EYE_LEFT, EYE_RIGHT, HOURS, MINUTES, DATE,
    WIFI, BATTERY, WEEKDAY, MIC, BUTTON,
    CHILD_COUNT
};

static uint8_t buf[PANEL_WIDTH * PANEL_HEIGHT * 2];
static uint32_t fake_tick;
static lv_display_t * display;
static int checks;
static int failures;

#define CHECK(cond, ...)                          \
    do {                                          \
        checks++;                                 \
        if (!(cond)) {                            \
            failures++;                           \
            printf("  FAIL line %d: ", __LINE__); \
            printf(__VA_ARGS__);                  \
            putchar('\n');                        \
        }                                         \
    } while (0)

static uint32_t tick_get(void)
{
    return fake_tick;
}

/* Without a flush callback LVGL leaves the display marked as flushing and the
 * second refresh spins in wait_for_flushing forever. */
static void flush_cb(lv_display_t * d, const lv_area_t * area, uint8_t * px_map)
{
    LV_UNUSED(area);
    LV_UNUSED(px_map);
    lv_display_flush_ready(d);
}

static void pump(uint32_t ms)
{
    uint32_t step;
    for (step = 0; step < ms; step += 16) {
        fake_tick += 16;
        lv_timer_handler();
    }
}

static lv_obj_t * child(int index)
{
    return lv_obj_get_child(lv_screen_active(), index);
}

static int32_t right_of(lv_obj_t * obj)
{
    return lv_obj_get_x(obj) + lv_obj_get_width(obj) - 1;
}

static int32_t bottom_of(lv_obj_t * obj)
{
    return lv_obj_get_y(obj) + lv_obj_get_height(obj) - 1;
}

/* Equal margins to a pixel. An object of odd width on an even panel cannot
 * have them exactly, so centring is allowed to land one pixel either way. */
static bool centred(lv_obj_t * obj)
{
    int32_t left = lv_obj_get_x(obj);
    int32_t right = PANEL_WIDTH - 1 - right_of(obj);
    return left - right <= 1 && right - left <= 1;
}

static int32_t opa_of(lv_obj_t * obj)
{
    return lv_obj_get_style_opa(obj, LV_PART_MAIN);
}

static const char * text_of(lv_obj_t * obj)
{
    return lv_label_get_text(obj);
}

static void click_button(void)
{
    lv_obj_send_event(child(BUTTON), LV_EVENT_CLICKED, NULL);
}

/* The label inside the button, which is the only thing that names the view. */
static const char * button_text(void)
{
    return lv_label_get_text(lv_obj_get_child(child(BUTTON), 0));
}

static void expect_clock_view(const char * when)
{
    CHECK(opa_of(child(HOURS)) == LV_OPA_COVER, "%s: hours faded (%d)", when, opa_of(child(HOURS)));
    CHECK(opa_of(child(MINUTES)) == LV_OPA_COVER, "%s: minutes faded", when);
    CHECK(opa_of(child(DATE)) == LV_OPA_COVER, "%s: date faded", when);
    CHECK(opa_of(child(EYE_LEFT)) == LV_OPA_TRANSP, "%s: left eye showing", when);
    CHECK(opa_of(child(EYE_RIGHT)) == LV_OPA_TRANSP, "%s: right eye showing", when);
    CHECK(lv_obj_get_width(child(EYE_LEFT)) == GROUP_WIDTH &&
          lv_obj_get_height(child(EYE_LEFT)) == GROUP_HEIGHT,
          "%s: left eye is %dx%d, not the digit group's box", when,
          lv_obj_get_width(child(EYE_LEFT)), lv_obj_get_height(child(EYE_LEFT)));
    CHECK(strcmp(button_text(), "Hey Kai") == 0, "%s: button reads \"%s\"", when, button_text());
}

static void expect_assistant_view(const char * when)
{
    CHECK(opa_of(child(HOURS)) == LV_OPA_TRANSP, "%s: hours still showing", when);
    CHECK(opa_of(child(MINUTES)) == LV_OPA_TRANSP, "%s: minutes still showing", when);
    CHECK(opa_of(child(DATE)) == LV_OPA_TRANSP, "%s: date still showing", when);
    CHECK(opa_of(child(EYE_LEFT)) == LV_OPA_COVER, "%s: left eye not showing", when);
    CHECK(opa_of(child(EYE_RIGHT)) == LV_OPA_COVER, "%s: right eye not showing", when);
    CHECK(lv_obj_get_width(child(EYE_LEFT)) == EYE_SIZE &&
          lv_obj_get_height(child(EYE_LEFT)) == EYE_SIZE,
          "%s: left eye is %dx%d, not %d square", when,
          lv_obj_get_width(child(EYE_LEFT)), lv_obj_get_height(child(EYE_LEFT)), EYE_SIZE);
    CHECK(strcmp(button_text(), "Quit") == 0, "%s: button reads \"%s\"", when, button_text());
}

/* Shortest the left eye gets over a stretch of time, and how often it got
 * there: a blink is the height pulling in to a line and back out. */
static int32_t watch_blinks(uint32_t ms, int * blinks)
{
    lv_obj_t * eye = child(EYE_LEFT);
    int32_t shortest = INT32_MAX;
    int shut = 0;
    uint32_t t;

    *blinks = 0;
    for (t = 0; t < ms; t += 16) {
        int32_t height;
        fake_tick += 16;
        lv_timer_handler();
        height = lv_obj_get_height(eye);
        if (height < shortest) shortest = height;
        if (height < 60 && !shut) { shut = 1; (*blinks)++; }
        if (height > 100) shut = 0;
    }
    return shortest;
}

static void check_structure(void)
{
    printf("structure\n");
    CHECK(lv_obj_get_child_count(lv_screen_active()) == CHILD_COUNT,
          "screen holds %u children, not %d — has ui_build's order changed?",
          lv_obj_get_child_count(lv_screen_active()), CHILD_COUNT);
    CHECK(lv_obj_check_type(child(BUTTON), &lv_button_class), "last child is not the button");
    CHECK(lv_obj_check_type(child(HOURS), &lv_label_class), "hours is not a label");
    CHECK(lv_obj_check_type(child(EYE_LEFT), &lv_obj_class), "left eye is not a plain object");
    CHECK(!lv_obj_has_flag(child(EYE_LEFT), LV_OBJ_FLAG_CLICKABLE),
          "the left eye is clickable and would swallow touches");
}

/* The layout rule: nothing comes closer than EDGE_MARGIN to an edge. */
static void check_layout(void)
{
    printf("layout\n");
    CHECK(lv_obj_get_x(child(HOURS)) == EDGE_MARGIN,
          "hours sit %d from the left, not %d", lv_obj_get_x(child(HOURS)), EDGE_MARGIN);
    CHECK(PANEL_WIDTH - 1 - right_of(child(MINUTES)) == EDGE_MARGIN,
          "minutes sit %d from the right, not %d",
          PANEL_WIDTH - 1 - right_of(child(MINUTES)), EDGE_MARGIN);
    CHECK(lv_obj_get_width(child(HOURS)) == GROUP_WIDTH &&
          lv_obj_get_width(child(MINUTES)) == GROUP_WIDTH,
          "a digit group is not %d wide — tabular figures gone?", GROUP_WIDTH);
    CHECK(lv_obj_get_x(child(HOURS)) + right_of(child(MINUTES)) == PANEL_WIDTH - 1,
          "the two groups are not symmetric about the centre");
    CHECK(centred(child(DATE)), "the date is not centred: %d px left, %d px right",
          lv_obj_get_x(child(DATE)), PANEL_WIDTH - 1 - right_of(child(DATE)));
    CHECK(PANEL_HEIGHT - 1 - bottom_of(child(BUTTON)) == EDGE_MARGIN,
          "the button sits %d off the bottom, not %d",
          PANEL_HEIGHT - 1 - bottom_of(child(BUTTON)), EDGE_MARGIN);
    CHECK(centred(child(BUTTON)), "the button is not centred: %d px left, %d px right",
          lv_obj_get_x(child(BUTTON)), PANEL_WIDTH - 1 - right_of(child(BUTTON)));

    CHECK(lv_obj_get_x(child(WIFI)) == EDGE_MARGIN &&
          lv_obj_get_y(child(WIFI)) == EDGE_MARGIN, "wifi is not in the top-left corner");
    CHECK(PANEL_WIDTH - 1 - right_of(child(BATTERY)) == EDGE_MARGIN &&
          lv_obj_get_y(child(BATTERY)) == EDGE_MARGIN, "battery is not in the top-right corner");
    CHECK(lv_obj_get_x(child(WEEKDAY)) == EDGE_MARGIN &&
          PANEL_HEIGHT - 1 - bottom_of(child(WEEKDAY)) == EDGE_MARGIN,
          "the weekday is not in the bottom-left corner");
    CHECK(PANEL_WIDTH - 1 - right_of(child(MIC)) == EDGE_MARGIN &&
          PANEL_HEIGHT - 1 - bottom_of(child(MIC)) == EDGE_MARGIN,
          "the microphone is not in the bottom-right corner");
    CHECK(right_of(child(WEEKDAY)) < lv_obj_get_x(child(BUTTON)),
          "the weekday runs into the button");
    CHECK(lv_obj_get_x(child(MIC)) > right_of(child(BUTTON)),
          "the microphone runs into the button");
}

static void check_readouts(void)
{
    const char * date = text_of(child(DATE));
    const char * hours = text_of(child(HOURS));

    printf("readouts\n");
    CHECK(strlen(hours) == 2 && hours[0] >= '0' && hours[0] <= '9',
          "hours read \"%s\", not two digits", hours);
    CHECK(strlen(date) == 5 && date[2] == '/', "the date reads \"%s\", not MM/DD", date);
    CHECK(strlen(text_of(child(WEEKDAY))) == 3,
          "the weekday reads \"%s\"", text_of(child(WEEKDAY)));

    {
        ui_status_t good = { .battery_pct = 62, .wifi_up = true };
        ui_status_set(&good);
        CHECK(strncmp(text_of(child(BATTERY)), "62%", 3) == 0,
              "battery reads \"%s\" at 62%%", text_of(child(BATTERY)));
        CHECK(lv_obj_get_style_text_opa(child(WIFI), LV_PART_MAIN) == LV_OPA_COVER,
              "wifi is dim while the radio is up");
        CHECK(lv_obj_get_style_text_opa(child(MIC), LV_PART_MAIN) < LV_OPA_COVER,
              "the microphone is lit while it is shut");
    }
    {
        ui_status_t poor = { .battery_pct = 8, .charging = true, .listening = true };
        ui_status_set(&poor);
        CHECK(strncmp(text_of(child(BATTERY)), "8%", 2) == 0,
              "battery reads \"%s\" at 8%%", text_of(child(BATTERY)));
        CHECK(lv_obj_get_style_text_opa(child(WIFI), LV_PART_MAIN) < LV_OPA_COVER,
              "wifi is lit while the radio is down");
        CHECK(lv_obj_get_style_text_opa(child(MIC), LV_PART_MAIN) == LV_OPA_COVER,
              "the microphone is dim while it is open");
    }
    {
        ui_status_t unknown = { .battery_pct = -1, .wifi_up = true };
        ui_status_set(&unknown);
        CHECK(strncmp(text_of(child(BATTERY)), "--", 2) == 0,
              "battery reads \"%s\" before the platform has said anything",
              text_of(child(BATTERY)));
    }
}

/* It has to actually draw, not just lay out. Amber 0xFFB000 comes back as
 * 0xFFB200 through RGB565, which is the round-trip, not a bug. */
static void check_pixels(void)
{
    uint32_t i, lit = 0;
    uint16_t brightest = 0;

    printf("pixels\n");
    lv_refr_now(display);
    for (i = 0; i < PANEL_WIDTH * PANEL_HEIGHT; i++) {
        uint16_t v = (uint16_t) (buf[i * 2] | (buf[i * 2 + 1] << 8));
        if (v) lit++;
        if (v > brightest) brightest = v;
    }
    CHECK(lit > 5000, "only %u pixels are lit — is anything drawing?", lit);
    CHECK(lit < (PANEL_WIDTH * PANEL_HEIGHT) / 2,
          "%u pixels are lit — the background should be black", lit);
    CHECK(((brightest >> 11) & 0x1F) == 31 && ((brightest >> 5) & 0x3F) == 44 &&
          (brightest & 0x1F) == 0,
          "the brightest pixel is 0x%04X, not amber", brightest);
}

int main(void)
{
    int blinks;
    int32_t shortest;
    int32_t eye_centre_x, eye_centre_y;

    lv_init();
    lv_tick_set_cb(tick_get);

    display = lv_display_create(PANEL_WIDTH, PANEL_HEIGHT);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, buf, NULL, sizeof(buf), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, flush_cb);

    ui_build();
    pump(32);

    check_structure();
    check_layout();
    check_readouts();
    check_pixels();

    printf("views\n");
    expect_clock_view("on the clock");
    eye_centre_x = lv_obj_get_x(child(EYE_LEFT)) + lv_obj_get_width(child(EYE_LEFT)) / 2;
    eye_centre_y = lv_obj_get_y(child(EYE_LEFT)) + lv_obj_get_height(child(EYE_LEFT)) / 2;

    click_button();
    pump(600);
    expect_assistant_view("after the morph");
    CHECK(lv_obj_get_x(child(EYE_LEFT)) + lv_obj_get_width(child(EYE_LEFT)) / 2 == eye_centre_x &&
          lv_obj_get_y(child(EYE_LEFT)) + lv_obj_get_height(child(EYE_LEFT)) / 2 == eye_centre_y,
          "the eye moved during the morph; only its size may change");
    CHECK(opa_of(child(WIFI)) == LV_OPA_COVER && opa_of(child(BATTERY)) == LV_OPA_COVER &&
          opa_of(child(WEEKDAY)) == LV_OPA_COVER && opa_of(child(MIC)) == LV_OPA_COVER,
          "a corner readout left with the clock; all four sit over both views");

    printf("blink\n");
    shortest = watch_blinks(9000, &blinks);
    CHECK(blinks >= 2, "%d blinks in 9 s", blinks);
    CHECK(shortest < 20, "the eye only closed to %d px", shortest);
    CHECK(lv_obj_get_height(child(EYE_LEFT)) == EYE_SIZE,
          "the eye did not open back to %d px", EYE_SIZE);

    click_button();
    pump(600);
    expect_clock_view("back on the clock");

    shortest = watch_blinks(9000, &blinks);
    CHECK(blinks == 0, "the eye blinked %d times on the clock face", blinks);
    CHECK(shortest == GROUP_HEIGHT, "the eye box changed height on the clock face");

    printf("%d checks, %d failed\n", checks, failures);
    return failures != 0;
}
