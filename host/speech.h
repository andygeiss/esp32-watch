/**
 * @file speech.h
 * The host's half of the three speech services: where they are, how to reach
 * them, and the three questions a turn asks them.
 *
 * This is the seam voice.c used to hold inside itself. The split is between
 * the two things that half was doing: **speech.c is the three services** —
 * the environment they are configured from, the socket and the TLS under it,
 * the clip a voice is cloned from, and transcribe/reply/synthesise; **voice.c
 * is the two devices and the thread** — the SDL microphone, the SDL speaker,
 * the gate, and the loop that takes turns.
 *
 * It came apart when the benchmark went in. `bench.c` has to ask the same
 * three services the same three questions, with the same bodies out of
 * voice/turn.c over the same transport, or it measures something the watch
 * does not do — the same reason turn.c exists at all. Anything else would
 * have been a second HTTP client, and a second HTTP client is a second set of
 * numbers.
 *
 * It costs one thing worth naming. firmware/main/voice.c is still one file
 * doing both jobs, so the two platform halves are no longer quite the mirror
 * CLAUDE.md describes: the loop in each is still the same pass in the same
 * order, but the host's three services now live next door. The device would
 * split the same way the day it wants a benchmark of its own.
 */

#ifndef SPEECH_H
#define SPEECH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "turn.h"

/* Everything about the server, out of the environment, read once at start-up.
 * The device reads the same seven settings out of menuconfig and calls the
 * same three setters with them, which is why they are named alike: what the
 * simulator is told and what the watch is told have to be the one list, or
 * the simulator stops standing in for anything.
 *
 * The environment rather than a header because one of them is a secret. A key
 * compiled in is a key committed, and this repository is public; the rest
 * follow it so that configuring the simulator is one kind of act and not two.
 * `make run` inherits the shell, so an exported variable is already there. */
#define VOICE_ENV_URL      "WATCH_VOICE_URL"
#define VOICE_ENV_KEY      "WATCH_VOICE_KEY"
#define VOICE_ENV_STT      "WATCH_STT_MODEL"
#define VOICE_ENV_BRAIN    "WATCH_BRAIN_MODEL"
#define VOICE_ENV_TTS      "WATCH_TTS_MODEL"
#define VOICE_ENV_LANGUAGE "WATCH_LANGUAGE"
#define VOICE_ENV_WAKE     "WATCH_WAKE_PHRASE"
#define VOICE_ENV_NAME     "WATCH_NAME"

/* An oMLX on this machine, which is what a simulator usually has in front of
 * it, and the one address that needs no TLS and no key. */
#define VOICE_URL_DEFAULT "http://127.0.0.1:8000"


/* The clip whose voice the assistant borrows, relative to the working
 * directory — `make run` starts the binary from the repository root. Its
 * words are read from the .txt beside it, which is the convention the Go
 * orchestrator uses for the same pair: one path to configure, and a
 * transcript that cannot be pointed at the wrong recording.
 *
 * voices/ is gitignored, so a fresh clone has neither and the watch is a
 * clock until one is put there. tools/gen_voice.sh makes the default. */
#define VOICE_ENV_CLIP    "WATCH_VOICE_CLIP"
#define VOICE_CLIP_DEFAULT "voices/female.wav"

/**
 * Read the environment, work out the address, and load the clip whose voice
 * the assistant borrows. False means there is nowhere to ask or nothing to
 * speak with, which is a clock rather than an error — the same answer the
 * firmware gives an unconfigured SSID.
 */
bool speech_start(void);

/** Give back the clip and the TLS context. */
void speech_stop(void);

/**
 * What makes a request in flight give up early. The simulator hands over its
 * own "is the window still open?", so closing it costs a second rather than
 * the two-minute HTTP budget; the benchmark hands over nothing, having no
 * window to close. NULL is the default and means a request runs to its end.
 */
void speech_cancel_set(bool (*cancelled)(void));

/** What was said, or false when nothing was. */
bool speech_transcribe(const int16_t * pcm, size_t samples, char * out, size_t cap);

/** What the assistant answers, tools and all. */
bool speech_reply(const char * heard, char * out, size_t cap);

/** `text` as audio, in whatever format the server chose to answer with. */
bool speech_synthesise(const char * text, voice_buf_t * wav);

#endif /* SPEECH_H */
