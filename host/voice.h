/**
 * @file voice.h
 * Host-only: the microphone, the two speech services, and the speaker.
 *
 * The simulator's stand-in for an audio path the board does not have yet.
 * It hears through SDL, transcribes with Parakeet, says the words back, and
 * plays the answer through SDL again — the shortest route through the whole
 * pipeline, so a turn that breaks here breaks everywhere.
 *
 * The microphone is open from start-up, because there is no wake-word engine
 * on a Mac: the transcriber is the wake-word engine, and the watch wakes when
 * its own name comes back in a transcript.
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
 * there is no reference clip and no server: the loop stays quiet and the two
 * corners stay dim, which is the reading dimming is for.
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
