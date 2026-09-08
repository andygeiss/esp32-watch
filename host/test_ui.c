/**
 * @file test_ui.c
 * Renders ui.c into a plain buffer and checks what came out.
 *
 * The second host. main.c drives the UI with an SDL window, a real clock and
 * a microphone; this one drives it with a byte array, a fake clock and
 * ui_view_set() called by hand, so the layout, the views, the morph, the
 * blink and the status readouts can all be checked without a screen —
 * `screencapture` needs permissions this machine does not grant, and a person
 * looking at a window is not a gate.
 *
 * Host-only, like main.c: the firmware builds neither. `make test` runs it,
 * and a non-zero exit is a failure.
 */

#include <stdio.h>
#include <string.h>

#include "lvgl/lvgl.h"
#include "ui.h"

/* The assistant's two corners have a font of their own. */
LV_FONT_DECLARE(ui_font_assistant_18);

#define PANEL_WIDTH  410
#define PANEL_HEIGHT 502

/* The numbers ui.c derives its layout from. Spelled out again here on
 * purpose: a test that imports the constant cannot catch it changing. */
#define EDGE_MARGIN  32
#define GROUP_WIDTH  158
#define GROUP_HEIGHT 85
#define EYE_SIZE     69
#define EYE_LID      6  /* the eye's padding, half the height it shuts to */
#define PUPIL_PCT    50 /* of what the lids leave, which is what makes the
                           pupil close with the eye rather than sit in it */
#define CATCHLIGHT_PCT     30 /* of the pupil */
#define CATCHLIGHT_OFF_PCT (-20)
#define EYE_SHUT_H   12 /* what the blink pulls the eye's height in to */
#define LASH_WIDTH   5

/* The lashes, spelled out again like the rest of it. LVGL's angles start at 3
 * o'clock and run clockwise, so these four are the left eye's upper-outer
 * arc; the right eye's are 540 minus each. */
static const struct {
    int32_t angle;
    int32_t length;
} LASHES[] = {
    { 188, 22 },
    { 208, 20 },
    { 228, 17 },
    { 248, 14 },
};
#define LASH_COUNT ((int) (sizeof(LASHES) / sizeof(LASHES[0])))

/* What those work out to with the eye open. Derived rather than typed in, so
 * that changing the eye's size moves the pixels this test looks at with it —
 * a probe left behind at the old geometry lands on the wrong thing and still
 * passes. Integer division truncates toward zero here exactly as it does in
 * LVGL's own percentage arithmetic, so these are the same numbers. */
#define PUPIL_SIZE      ((EYE_SIZE - 2 * EYE_LID) * PUPIL_PCT / 100)
#define CATCHLIGHT_SIZE (PUPIL_SIZE * CATCHLIGHT_PCT / 100)
#define CATCHLIGHT_OFF  (PUPIL_SIZE * CATCHLIGHT_OFF_PCT / 100)

/* And what the amber 0xFFB000 of a lit pixel reads back as through RGB565 —
 * see check_pixels. */
#define AMBER_565 ((31 << 11) | (44 << 5))

/* Children of the screen, in the order ui_build() creates them. Neither face
 * has a button on it: the platform hears the watch's name and says so through
 * ui_view_set(), so there is nothing here to click. */
enum {
    EYE_LEFT, EYE_RIGHT, HOURS, MINUTES, DATE, WEEKDAY,
    WIFI, BATTERY, SPEAKER, MIC,
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

/* An eye holds a pupil and the pupil holds a catchlight, each its only child.
 * They are what a disc needs to read as an eye, so the checks go at them. */
static lv_obj_t * pupil_of(lv_obj_t * eye)
{
    return lv_obj_get_child(eye, 0);
}

/* One pixel of the last render, the way the panel would show it. */
static uint16_t pixel_at(int32_t x, int32_t y)
{
    uint32_t i = (uint32_t) (y * PANEL_WIDTH + x);
    return (uint16_t) (buf[i * 2] | (buf[i * 2 + 1] << 8));
}

static int32_t lash_longest(void)
{
    int32_t longest = 0;
    int i;

    for (i = 0; i < LASH_COUNT; i++) {
        if (LASHES[i].length > longest) longest = LASHES[i].length;
    }
    return longest;
}

/* A point half way along one lash of one eye, taken off the eye's own live
 * box rather than typed in, so that changing the eye moves the probe with it.
 * `mirror` asks for the same lash on the other side of the eye, which is
 * where a lash goes if the mirroring is the wrong way round — the useful
 * thing to find nothing at. */
static void lash_point(lv_obj_t * eye, int i, bool mirror, int32_t * x, int32_t * y)
{
    int32_t rx = lv_obj_get_width(eye) / 2;
    int32_t ry = lv_obj_get_height(eye) / 2;
    int32_t cx = lv_obj_get_x(eye) + rx;
    int32_t cy = lv_obj_get_y(eye) + ry;
    int32_t angle = mirror ? 540 - LASHES[i].angle : LASHES[i].angle;
    int32_t cos_a = lv_trigo_cos((int16_t) angle);
    int32_t sin_a = lv_trigo_sin((int16_t) angle);

    *x = cx + (rx + LASHES[i].length / 2) * cos_a / LV_TRIGO_SIN_MAX;
    *y = cy + (ry + LASHES[i].length / 2) * sin_a / LV_TRIGO_SIN_MAX;
}

/* Lit pixels either side of the centre line, over the band the two eyes and
 * their lashes are in and nothing else. */
static void eye_band_lit(int32_t centre_y, uint32_t * left, uint32_t * right)
{
    int32_t reach = EYE_SIZE / 2 + lash_longest() + LASH_WIDTH;
    int32_t x, y;

    *left = *right = 0;
    for (y = centre_y - reach; y <= centre_y + reach; y++) {
        for (x = 0; x < PANEL_WIDTH; x++) {
            if (pixel_at(x, y) == 0) continue;
            if (x < PANEL_WIDTH / 2) (*left)++;
            else (*right)++;
        }
    }
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

static void expect_clock_view(const char * when)
{
    CHECK(opa_of(child(HOURS)) == LV_OPA_COVER, "%s: hours faded (%d)", when, opa_of(child(HOURS)));
    CHECK(opa_of(child(MINUTES)) == LV_OPA_COVER, "%s: minutes faded", when);
    CHECK(opa_of(child(DATE)) == LV_OPA_COVER, "%s: date faded", when);
    CHECK(opa_of(child(WEEKDAY)) == LV_OPA_COVER, "%s: weekday faded", when);
    CHECK(opa_of(child(EYE_LEFT)) == LV_OPA_TRANSP, "%s: left eye showing", when);
    CHECK(opa_of(child(EYE_RIGHT)) == LV_OPA_TRANSP, "%s: right eye showing", when);
    CHECK(lv_obj_get_width(child(EYE_LEFT)) == GROUP_WIDTH &&
          lv_obj_get_height(child(EYE_LEFT)) == GROUP_HEIGHT,
          "%s: left eye is %dx%d, not the digit group's box", when,
          lv_obj_get_width(child(EYE_LEFT)), lv_obj_get_height(child(EYE_LEFT)));
}

static void expect_assistant_view(const char * when)
{
    CHECK(opa_of(child(HOURS)) == LV_OPA_TRANSP, "%s: hours still showing", when);
    CHECK(opa_of(child(MINUTES)) == LV_OPA_TRANSP, "%s: minutes still showing", when);
    CHECK(opa_of(child(DATE)) == LV_OPA_TRANSP, "%s: date still showing", when);
    CHECK(opa_of(child(WEEKDAY)) == LV_OPA_TRANSP, "%s: weekday still showing", when);
    CHECK(opa_of(child(EYE_LEFT)) == LV_OPA_COVER, "%s: left eye not showing", when);
    CHECK(opa_of(child(EYE_RIGHT)) == LV_OPA_COVER, "%s: right eye not showing", when);
    CHECK(lv_obj_get_width(child(EYE_LEFT)) == EYE_SIZE &&
          lv_obj_get_height(child(EYE_LEFT)) == EYE_SIZE,
          "%s: left eye is %dx%d, not %d square", when,
          lv_obj_get_width(child(EYE_LEFT)), lv_obj_get_height(child(EYE_LEFT)), EYE_SIZE);
    CHECK(lv_obj_get_width(pupil_of(child(EYE_LEFT))) == PUPIL_SIZE &&
          lv_obj_get_height(pupil_of(child(EYE_LEFT))) == PUPIL_SIZE,
          "%s: the pupil is %dx%d, not %d square", when,
          lv_obj_get_width(pupil_of(child(EYE_LEFT))),
          lv_obj_get_height(pupil_of(child(EYE_LEFT))), PUPIL_SIZE);
}

/* Shortest the left eye gets over a stretch of time, and how often it got
 * there: a blink is the height pulling in to a line and back out.
 *
 * `saying` is a view to re-assert before every frame, or NULL for none. It is
 * how the poll main.c runs is reproduced here: ten times a second it hands
 * ui_view_set() the view the watch is already in, and if that restarts the
 * morph the eye never blinks again.
 *
 * `pupil_shortest` takes the same reading off the pupil, or NULL for none.
 * Nothing animates it, so the only thing that can bring it down is the eye
 * closing over it. */
static int32_t watch_blinks(uint32_t ms, int * blinks, const bool * saying,
                            int32_t * pupil_shortest)
{
    lv_obj_t * eye = child(EYE_LEFT);
    int32_t shortest = INT32_MAX;
    int32_t pupil = INT32_MAX;
    int shut = 0;
    uint32_t t;

    *blinks = 0;
    for (t = 0; t < ms; t += 16) {
        int32_t height;
        if (saying != NULL) ui_view_set(*saying);
        fake_tick += 16;
        lv_timer_handler();
        height = lv_obj_get_height(eye);
        if (height < shortest) shortest = height;
        if (lv_obj_get_height(pupil_of(eye)) < pupil) pupil = lv_obj_get_height(pupil_of(eye));
        if (height < EYE_SIZE / 2 && !shut) { shut = 1; (*blinks)++; }
        if (height > EYE_SIZE * 3 / 4) shut = 0;
    }
    if (pupil_shortest != NULL) *pupil_shortest = pupil;
    return shortest;
}

static void check_structure(void)
{
    printf("structure\n");
    CHECK(lv_obj_get_child_count(lv_screen_active()) == CHILD_COUNT,
          "screen holds %u children, not %d — has ui_build's order changed?",
          lv_obj_get_child_count(lv_screen_active()), CHILD_COUNT);
    CHECK(lv_obj_check_type(child(HOURS), &lv_label_class), "hours is not a label");
    CHECK(lv_obj_check_type(child(EYE_LEFT), &lv_obj_class), "left eye is not a plain object");
    CHECK(!lv_obj_has_flag(child(EYE_LEFT), LV_OBJ_FLAG_CLICKABLE),
          "the left eye is clickable and would swallow touches");
    CHECK(lv_obj_get_child_count(child(EYE_LEFT)) == 1 &&
          lv_obj_get_child_count(pupil_of(child(EYE_LEFT))) == 1,
          "the left eye is not a pupil with a catchlight in it");
    CHECK(!lv_obj_has_flag(pupil_of(child(EYE_LEFT)), LV_OBJ_FLAG_CLICKABLE),
          "the pupil is clickable and would swallow the touch the eye lets through");
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
    CHECK(centred(child(WEEKDAY)), "the weekday is not centred: %d px left, %d px right",
          lv_obj_get_x(child(WEEKDAY)), PANEL_WIDTH - 1 - right_of(child(WEEKDAY)));

    /* The stack reads big to small, top to bottom, and never overlaps. */
    CHECK(lv_obj_get_y(child(DATE)) > bottom_of(child(HOURS)),
          "the date is not below the time");
    CHECK(lv_obj_get_y(child(WEEKDAY)) > bottom_of(child(DATE)),
          "the weekday is not below the date");
    CHECK(lv_obj_get_height(child(DATE)) < lv_obj_get_height(child(HOURS)),
          "the date (%d px) is not smaller than the time (%d px)",
          lv_obj_get_height(child(DATE)), lv_obj_get_height(child(HOURS)));
    CHECK(lv_obj_get_height(child(WEEKDAY)) < lv_obj_get_height(child(DATE)),
          "the weekday (%d px) is not smaller than the date (%d px)",
          lv_obj_get_height(child(WEEKDAY)), lv_obj_get_height(child(DATE)));

    /* Nothing reserves the bottom of the panel any more, so the stack sits in
     * the middle of the whole of it: as much sky above the time as floor
     * below the weekday, give or take the pixel an odd height cannot split. */
    {
        int32_t above = lv_obj_get_y(child(HOURS));
        int32_t below = PANEL_HEIGHT - 1 - bottom_of(child(WEEKDAY));
        CHECK(above - below <= 1 && below - above <= 1,
              "the stack is not centred in the panel: %d px above, %d px below",
              above, below);
    }

    CHECK(lv_obj_get_x(child(WIFI)) == EDGE_MARGIN &&
          lv_obj_get_y(child(WIFI)) == EDGE_MARGIN, "wifi is not in the top-left corner");
    CHECK(PANEL_WIDTH - 1 - right_of(child(BATTERY)) == EDGE_MARGIN &&
          lv_obj_get_y(child(BATTERY)) == EDGE_MARGIN, "battery is not in the top-right corner");
    CHECK(lv_obj_get_x(child(SPEAKER)) == EDGE_MARGIN &&
          PANEL_HEIGHT - 1 - bottom_of(child(SPEAKER)) == EDGE_MARGIN,
          "the speaker is not in the bottom-left corner");
    CHECK(PANEL_WIDTH - 1 - right_of(child(MIC)) == EDGE_MARGIN &&
          PANEL_HEIGHT - 1 - bottom_of(child(MIC)) == EDGE_MARGIN,
          "the microphone is not in the bottom-right corner");
    CHECK(right_of(child(SPEAKER)) < lv_obj_get_x(child(MIC)),
          "the two bottom corners overlap");
    CHECK(bottom_of(child(WEEKDAY)) < lv_obj_get_y(child(SPEAKER)),
          "the weekday runs into the bottom corners");
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
          "the weekday reads \"%s\", not three letters", text_of(child(WEEKDAY)));
    {
        ui_status_t idle = { .battery_pct = 62, .wifi_up = true };
        ui_status_set(&idle);
        CHECK(strncmp(text_of(child(BATTERY)), "62%", 3) == 0,
              "battery reads \"%s\" at 62%%", text_of(child(BATTERY)));
        CHECK(lv_obj_get_style_text_opa(child(WIFI), LV_PART_MAIN) == LV_OPA_COVER,
              "wifi is dim while the radio is up");
        CHECK(lv_obj_get_style_text_opa(child(MIC), LV_PART_MAIN) < LV_OPA_COVER,
              "the microphone is lit while it is shut");
        CHECK(lv_obj_get_style_text_opa(child(SPEAKER), LV_PART_MAIN) < LV_OPA_COVER,
              "the speaker is lit while it is quiet");
    }
    {
        ui_status_t listening = { .battery_pct = 8, .charging = true, .listening = true };
        ui_status_set(&listening);
        CHECK(strncmp(text_of(child(BATTERY)), "8%", 2) == 0,
              "battery reads \"%s\" at 8%%", text_of(child(BATTERY)));
        CHECK(lv_obj_get_style_text_opa(child(WIFI), LV_PART_MAIN) < LV_OPA_COVER,
              "wifi is lit while the radio is down");
        CHECK(lv_obj_get_style_text_opa(child(MIC), LV_PART_MAIN) == LV_OPA_COVER,
              "the microphone is dim while it is open");
        CHECK(lv_obj_get_style_text_opa(child(SPEAKER), LV_PART_MAIN) < LV_OPA_COVER,
              "the speaker is lit while only the microphone is on");
    }
    {
        ui_status_t speaking = { .battery_pct = 50, .wifi_up = true, .speaking = true };
        ui_status_set(&speaking);
        CHECK(lv_obj_get_style_text_opa(child(SPEAKER), LV_PART_MAIN) == LV_OPA_COVER,
              "the speaker is dim while it is playing");
        CHECK(lv_obj_get_style_text_opa(child(MIC), LV_PART_MAIN) < LV_OPA_COVER,
              "the microphone is lit while only the speaker is on");
    }
    {
        /* Both at once: a firmware with echo cancellation listens while it
         * talks, and the two readouts must not be wired as one mode. */
        ui_status_t both = { .battery_pct = 50, .wifi_up = true,
                             .listening = true, .speaking = true };
        ui_status_set(&both);
        CHECK(lv_obj_get_style_text_opa(child(MIC), LV_PART_MAIN) == LV_OPA_COVER &&
              lv_obj_get_style_text_opa(child(SPEAKER), LV_PART_MAIN) == LV_OPA_COVER,
              "the microphone and the speaker cannot both be lit");
    }
    CHECK(lv_obj_get_style_text_font(child(MIC), LV_PART_MAIN) == &ui_font_assistant_18 &&
          lv_obj_get_style_text_font(child(SPEAKER), LV_PART_MAIN) == &ui_font_assistant_18,
          "an assistant corner is not drawn with the assistant font");
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
    int32_t shortest, pupil_shortest;
    int32_t eye_centre_x, eye_centre_y;

    lv_init();
    lv_tick_set_cb(tick_get);

    display = lv_display_create(PANEL_WIDTH, PANEL_HEIGHT);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, buf, NULL, sizeof(buf), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, flush_cb);

    /* Before anything exists, which the firmware could do the moment a wake
     * word beats ui_build() to it. A call through the labels here takes the
     * whole run down with a segfault rather than printing a failure. */
    ui_view_set(true);

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

    expect_clock_view("after being woken before ui_build()");

    ui_view_set(true);
    pump(600);
    expect_assistant_view("after the morph");
    CHECK(lv_obj_get_x(child(EYE_LEFT)) + lv_obj_get_width(child(EYE_LEFT)) / 2 == eye_centre_x &&
          lv_obj_get_y(child(EYE_LEFT)) + lv_obj_get_height(child(EYE_LEFT)) / 2 == eye_centre_y,
          "the eye moved during the morph; only its size may change");
    CHECK(opa_of(child(WIFI)) == LV_OPA_COVER && opa_of(child(BATTERY)) == LV_OPA_COVER &&
          opa_of(child(SPEAKER)) == LV_OPA_COVER && opa_of(child(MIC)) == LV_OPA_COVER,
          "a corner readout left with the clock; all four sit over both views");

    /* An eye's width of black between the two of them. That proportion is what
     * makes the pair read as a face; a bigger eye closes the gap up and they
     * go back to being two discs sitting where the digits were. */
    CHECK(lv_obj_get_x(child(EYE_RIGHT)) - right_of(child(EYE_LEFT)) - 1 >= EYE_SIZE,
          "%d px of black between two %d px eyes",
          lv_obj_get_x(child(EYE_RIGHT)) - right_of(child(EYE_LEFT)) - 1, EYE_SIZE);

    /* And it has to be an eye, not a dot: a hole in an amber disc with a light
     * caught in it. Only the pixels can say so — the geometry above is the
     * same either way. */
    {
        int32_t cx = lv_obj_get_x(child(EYE_LEFT)) + EYE_SIZE / 2;
        int32_t cy = lv_obj_get_y(child(EYE_LEFT)) + EYE_SIZE / 2;

        /* Well inside the pupil and away from the catchlight; midway between
         * the pupil's edge and the eye's, where only amber can be; and the
         * middle of the catchlight itself. */
        int32_t in_pupil = PUPIL_SIZE / 4;
        int32_t on_amber = (EYE_SIZE / 2 + PUPIL_SIZE / 2) / 2;

        lv_refr_now(display);
        CHECK(pixel_at(cx + in_pupil, cy + in_pupil) == 0,
              "the pupil is not a hole: 0x%04X in the middle of the eye",
              pixel_at(cx + in_pupil, cy + in_pupil));
        CHECK(pixel_at(cx, cy - on_amber) == AMBER_565,
              "the amber around the pupil reads 0x%04X",
              pixel_at(cx, cy - on_amber));
        CHECK(pixel_at(cx + CATCHLIGHT_OFF, cy + CATCHLIGHT_OFF) == AMBER_565,
              "no catchlight in the pupil: 0x%04X where it should be",
              pixel_at(cx + CATCHLIGHT_OFF, cy + CATCHLIGHT_OFF));
        CHECK(CATCHLIGHT_SIZE >= 6,
              "the catchlight is down to %d px — too small to read as one",
              CATCHLIGHT_SIZE);
    }

    /* The lashes, which are what makes the pair a woman's. Only the pixels can
     * say anything about them: they are drawn by the eye rather than built out
     * of objects, so there is no geometry to read. */
    printf("lashes\n");
    {
        int e, i;

        for (e = 0; e < 2; e++) {
            lv_obj_t * eye = child(e == 0 ? EYE_LEFT : EYE_RIGHT);
            int32_t x, y;

            for (i = 0; i < LASH_COUNT; i++) {
                lash_point(eye, i, e == 1, &x, &y);
                CHECK(pixel_at(x, y) == AMBER_565,
                      "%s eye: lash %d reads 0x%04X at (%d,%d)",
                      e == 0 ? "left" : "right", i, pixel_at(x, y), x, y);
            }

            /* And nothing on the inner side. Mirroring the fan the wrong way
             * puts every lash between the eyes, which the geometry cannot
             * tell from the right way round. */
            lash_point(eye, 0, e == 0, &x, &y);
            CHECK(pixel_at(x, y) == 0,
                  "%s eye: 0x%04X on its inner side — is the fan mirrored the "
                  "wrong way?", e == 0 ? "left" : "right", pixel_at(x, y));
        }
    }

    /* A mirrored pair has to weigh the same on both sides of the panel. This
     * is the check that an eye which is not drawn from its own centre fails:
     * the fan slides the same way on both eyes, which is out into the black on
     * one of them and into the amber on the other, and every point probed
     * above still lands on a lash. */
    {
        uint32_t left, right;
        eye_band_lit(eye_centre_y, &left, &right);
        CHECK(left > right ? left - right <= left / 50 : right - left <= right / 50,
              "%u lit pixels on the left of the face and %u on the right", left, right);
    }

    printf("blink\n");
    shortest = watch_blinks(9000, &blinks, NULL, &pupil_shortest);
    CHECK(blinks >= 2, "%d blinks in 9 s", blinks);
    CHECK(shortest < 20, "the eye only closed to %d px", shortest);
    CHECK(lv_obj_get_height(child(EYE_LEFT)) == EYE_SIZE,
          "the eye did not open back to %d px", EYE_SIZE);

    /* The lids meet on nothing. The pupil is a percentage of what they leave
     * between them, so it is pinched out exactly as the eye reaches its shut
     * height; one of its own would sit there as a slit across the closed
     * line. */
    CHECK(pupil_shortest == 0, "the eye shut to %d px with %d px of pupil still in it",
          shortest, pupil_shortest);

    /* Still a pair with the lids down. An eye stops being square the moment it
     * blinks, and that is exactly when a lash hung off anything but the eye's
     * own live box goes wandering. */
    {
        lv_obj_t * eye = child(EYE_LEFT);
        uint32_t t, left, right;
        int32_t shut;

        /* On to a frame with the lid down. lv_refr_now() steps the animations
         * as well as drawing them, so the height belongs after that call and
         * not before it: what read 12 px when this loop stopped is a few
         * pixels open again in the frame that actually got drawn. Any height
         * off the square will do — being off the square is the whole of what
         * this is looking for. */
        for (t = 0; t < 9000 && lv_obj_get_height(eye) == EYE_SIZE; t += 16) {
            fake_tick += 16;
            lv_timer_handler();
        }
        lv_refr_now(display);
        shut = lv_obj_get_height(eye);
        CHECK(shut < EYE_SIZE, "the eye never left %d px to be looked at mid-blink",
              EYE_SIZE);
        eye_band_lit(eye_centre_y, &left, &right);
        CHECK(left > right ? left - right <= left / 50 : right - left <= right / 50,
              "with the lid at %d px: %u lit pixels on the left of the face and %u "
              "on the right", shut, left, right);
    }

    /* The same stretch again, with the view re-asserted on every frame the
     * way main.c's timer does. Being told the view it is already in has to
     * cost nothing: a morph restarted ten times a second never arrives, and
     * takes the blink down with it because both drive the eye's height. */
    {
        bool assistant = true;
        watch_blinks(9000, &blinks, &assistant, NULL);
        CHECK(blinks >= 2, "%d blinks in 9 s while the view was re-asserted every frame",
              blinks);
        expect_assistant_view("while the view was re-asserted every frame");
    }

    ui_view_set(false);
    pump(600);
    expect_clock_view("back on the clock");

    /* The lashes go with the eye, because the stroke takes the eye's own
     * opacity. Given one of its own it would still be drawn here — and out
     * past the digit group's box, which is wider than the eye, so it would
     * also be over the edge margin. One probe says both. */
    {
        int e;

        lv_refr_now(display);
        for (e = 0; e < 2; e++) {
            int32_t x, y;
            lash_point(child(e == 0 ? EYE_LEFT : EYE_RIGHT), 0, e == 1, &x, &y);
            CHECK(pixel_at(x, y) == 0,
                  "%s eye: 0x%04X at (%d,%d) on the clock face — a lash outlived "
                  "the eye", e == 0 ? "left" : "right", pixel_at(x, y), x, y);
        }
    }

    shortest = watch_blinks(9000, &blinks, NULL, NULL);
    CHECK(blinks == 0, "the eye blinked %d times on the clock face", blinks);
    CHECK(shortest == GROUP_HEIGHT, "the eye box changed height on the clock face");

    printf("%d checks, %d failed\n", checks, failures);
    return failures != 0;
}
