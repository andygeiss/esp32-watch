/**
 * @file board.h
 * The Waveshare ESP32-S3-Touch-AMOLED-2.06 itself: the QSPI panel as an LVGL
 * display, the capacitive touch as an LVGL pointer, and the two audio codecs
 * as something voice.c can read and write.
 *
 * The device-side counterpart of the SDL window, mouse and audio devices the
 * simulator opens. Nothing behind ui.h ever sees any of them.
 *
 * Every pin number here is transcribed from Waveshare's own BSP for this
 * board — see the note at the top of board.c.
 */

#ifndef BOARD_H
#define BOARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"
#include <time.h>

/* The panel. The same two numbers as the simulator window, which is the whole
 * point of the simulator being 1:1 — see CLAUDE.md. */
#define BOARD_LCD_H_RES 410
#define BOARD_LCD_V_RES 502

/** Bring up the QSPI panel and give LVGL a display over it. */
lv_display_t * board_display_init(void);

/** Bring up the touch controller as a pointer device on `display`. */
lv_indev_t * board_touch_init(lv_display_t * display);

/** True once per touch-down since the last call: a tap is a reason to light
 * the panel, whether or not anything under it is pressable. */
bool board_touch_take_tap(void);

/** Blank the panel or light it. Rendering carries on underneath either way,
 * so a lit panel shows the present and not the frame it went dark on. */
void board_display_sleep(bool asleep);

/** Whether the panel is lit right now — which is to say, whether anyone is
 * looking. Safe to read from any task. */
bool board_display_lit(void);

/* ------------------------------------------------------------------ */
/* The real-time clock and the motion sensor: a PCF85063 and a QMI8658, both */
/* on the touch controller's I2C bus, so board_touch_init() first here too.  */
/* ------------------------------------------------------------------ */

/** Find the RTC. False if it is not there. */
bool board_rtc_init(void);

/** The RTC's time as UTC. False when it has no valid time to give — after
 * its first power-up, until board_rtc_write() has set it once. */
bool board_rtc_read(time_t * utc);

/** Set the RTC, in UTC. Safe from any task: the bus is locked per transfer. */
bool board_rtc_write(time_t utc);

/** Find the motion sensor and start its accelerometer. False if not there. */
bool board_motion_init(void);

/** The acceleration vector in g. False if there is no sensor. */
bool board_motion_read(float g[3]);

/* ------------------------------------------------------------------ */
/* Audio. The ES7210 in front of the microphones and the ES8311 in front of   */
/* the speaker share one I2S bus and one I2C bus — the same I2C bus the touch */
/* controller sits on, so board_touch_init() has to have run first.           */
/*                                                                            */
/* One bus means one clock, so the two cannot be open at once at different    */
/* rates. That is why these are open/close rather than always-on: the loop is */
/* half duplex anyway, and closing the microphone to play is the same         */
/* pause the simulator makes.                                                 */
/* ------------------------------------------------------------------ */

/** Bring up I2S and both codecs. False if the board did not answer. */
bool board_audio_init(void);

/* ------------------------------------------------------------------ */
/* Power. The AXP2101 charges the LiPo and measures its voltage, on the same  */
/* I2C bus, so board_touch_init() has to have run first here too.             */
/* ------------------------------------------------------------------ */

/** Find the power chip and switch its battery ADC on. False if it is not
 * there, after which board_battery_read() answers "unknown" forever. */
bool board_battery_init(void);

/** The charge, 0-100, from the battery's voltage, or negative when there is
 * no power chip or no battery on it; and whether the charger is currently
 * putting charge in. */
int board_battery_read(bool * charging);

/** Open the microphone at VOICE_RATE mono. Closes the speaker if it is open. */
bool board_mic_open(void);
void board_mic_close(void);

/**
 * Read up to `samples` 16-bit mono samples. Blocks until it has them or the
 * I2S read times out, so one call is roughly one block period. Returns how
 * many arrived, which is 0 on a failure.
 */
size_t board_mic_read(int16_t * into, size_t samples);

/** Open the speaker at the reply's own rate. Closes the microphone. */
bool board_speaker_open(int rate, int channels, int bits);
bool board_speaker_write(const void * pcm, size_t bytes);
void board_speaker_close(void);

#endif /* BOARD_H */
