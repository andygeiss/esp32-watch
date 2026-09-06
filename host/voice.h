/**
 * @file voice.h
 * Host-only: the microphone, the two speech services, and the speaker.
 *
 * The simulator's stand-in for an audio path the board does not have yet.
 * It hears through SDL, transcribes with Parakeet, says the words back, and
 * plays the answer through SDL again — the shortest route through the whole
 * pipeline, so a turn that breaks here breaks everywhere.
 *
 * Nothing in here may be reached from ui.c. The loop publishes two booleans,
 * main.c copies them into ui_status_t, and the UI never learns where they
 * came from — the same crossing ui_status_set() already makes for the radio
 * and the charge. See the host/device boundary in CLAUDE.md.
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
 * Wake the assistant or send it away. The button drives this: the loop takes
 * turns for as long as it is awake, and closes the microphone when it is not.
 */
void voice_listen(bool on);

/** The microphone is open. */
bool voice_listening(void);

/** The speaker is playing. */
bool voice_speaking(void);

#endif /* VOICE_H */
