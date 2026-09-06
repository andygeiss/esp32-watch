/**
 * @file voice.h
 * The voice loop, as its two platforms agree to present it.
 *
 * Five functions, implemented twice: host/voice.c against an SDL microphone,
 * a socket and an SDL speaker, and firmware/main/voice.c against the board's
 * ES7210, esp_http_client and its ES8311. Each main.c calls exactly this and
 * never learns which one it linked. turn.h beside this is the other half —
 * everything about a turn that is not a device, compiled into both.
 *
 * The microphone is open from start-up and the transcriber is the wake-word
 * engine: the watch wakes when its own name comes back in a transcript, and
 * goes back to the clock on a goodbye or a stretch of silence. There is
 * nothing to press on either face.
 *
 * Nothing in here may be reached from ui.c. The loop publishes three
 * booleans, main.c copies two into ui_status_t and hands the third to
 * ui_view_set(), and the UI never learns where any of them came from — the
 * same crossing ui_status_set() already makes for the radio and the charge.
 * See the host/device boundary in CLAUDE.md.
 */

#ifndef VOICE_H
#define VOICE_H

#include <stdbool.h>

/**
 * Open the audio devices and start the worker thread. Safe to call when
 * there is no reference clip and no server: the loop stays quiet, the watch
 * stays a clock, and the two corners stay dim, which is the reading dimming
 * is for. The firmware adds one more way to be quiet — no WiFi.
 */
void voice_start(void);

/** Stop the worker thread and close the devices. */
void voice_stop(void);

/**
 * The assistant is awake: its name has been heard, and it has neither been
 * said goodbye to nor run out of patience. This is what the assistant's face
 * is up for — main.c polls it into ui_view_set().
 */
bool voice_awake(void);

/**
 * The microphone is open for you. It is open for the watch's name the whole
 * time the loop runs, but a corner that is always lit says nothing, so this
 * is true only while the assistant is awake and taking a turn.
 */
bool voice_listening(void);

/** The speaker is playing. */
bool voice_speaking(void);

#endif /* VOICE_H */
