/**
 * @file ui.c
 * Retro clock face: hours left of centre, minutes right of it.
 *
 * The two digit groups are separate objects placed symmetrically about the
 * centre, because they later have to morph into the two eyes of the
 * assistant face. Keep them independent.
 *
 * Host-portable: no SDL, no ESP-IDF. See ui.h.
 */

#include "ui.h"

#include <time.h>

#include "lvgl/lvgl.h"

/* Amber-on-black, the way a VFD readout looks. */
#define UI_COLOR_BG     0x000000
#define UI_COLOR_AMBER  0xFFB000

/* Distance of each digit group from the centre of the panel, in pixels.
 * Wide enough that the two groups already read as a pair of eyes. */
#define UI_GROUP_OFFSET_X 70

#define UI_REFRESH_PERIOD_MS 1000

static lv_obj_t * hours_label;
static lv_obj_t * minutes_label;

static lv_obj_t * digits_create(lv_obj_t * parent, int32_t offset_x)
{
    lv_obj_t * label = lv_label_create(parent);

    lv_obj_set_style_text_font(label, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_AMBER), LV_PART_MAIN);
    lv_label_set_text(label, "--");

    /* Centre alignment is a style, so the group stays put as digits change width. */
    lv_obj_align(label, LV_ALIGN_CENTER, offset_x, 0);

    return label;
}

static void clock_refresh(lv_timer_t * timer)
{
    LV_UNUSED(timer);

    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);

    lv_label_set_text_fmt(hours_label, "%02d", local.tm_hour);
    lv_label_set_text_fmt(minutes_label, "%02d", local.tm_min);
}

void ui_build(void)
{
    lv_obj_t * screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(UI_COLOR_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

    hours_label   = digits_create(screen, -UI_GROUP_OFFSET_X);
    minutes_label = digits_create(screen, UI_GROUP_OFFSET_X);

    lv_timer_t * timer = lv_timer_create(clock_refresh, UI_REFRESH_PERIOD_MS, NULL);
    lv_timer_ready(timer); /* draw the real time now, not a second from now */
}
