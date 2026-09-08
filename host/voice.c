/**
 * @file voice.c
 * The host's half of the voice loop: an SDL microphone, a socket, an SDL
 * speaker, and a thread to run them on.
 *
 * Host-only, like main.c. It is the simulator's stand-in for the board's own
 * codec — the Mac's microphone and speakers do the job the ES8311 and the
 * ES7210 do on the watch, and firmware/main/voice.c is this same file written
 * against those.
 *
 * Everything that is not a device lives in voice/turn.c: the gate that ends a
 * turn, the name the watch answers to, the goodbye, and the two request
 * bodies. Both halves compile that file unchanged, so the simulator and the
 * watch cannot end up listening for different words or asking the server
 * different questions. Only what genuinely differs is here.
 *
 * The answer comes from a chat model when one is configured, and is the
 * question said back when none is. The echo is a mode rather than a
 * placeholder: it is the shortest path through the whole pipeline, so a turn
 * that breaks there breaks everywhere, and what comes out of the speaker is
 * exactly what the transcriber heard. Both live behind reply(), which is the
 * only function that knows the difference.
 *
 * There is nothing to press. The watch wakes when its own name turns up in a
 * transcript and goes back to the clock on a goodbye or a stretch of silence,
 * so the microphone is open from start-up and every utterance in the room is
 * transcribed.
 *
 * Three things are worth knowing before touching it:
 *
 * - **Everything here runs on its own thread.** A turn costs seconds and
 *   LVGL is single-threaded, so the loop cannot run in lv_timer_handler().
 *   It publishes three booleans through SDL atomics; main.c reads them in its
 *   timers and hands two to ui_status_set() and one to ui_view_set(). Nothing
 *   else crosses.
 *
 * - **A socket and OpenSSL, and nothing above them.** SPEC.md allows LVGL,
 *   SDL2 and OpenSSL, so the HTTP client below is still a request written by
 *   hand and a reply read back — OpenSSL replaces send() and recv() and
 *   nothing else. That is not an inconvenience: the device speaks to the same
 *   three endpoints through esp_http_client, and a body built by hand ports
 *   where a libcurl call site would not, which is why voice/turn.c builds all
 *   three bodies and neither platform does.
 *
 *   TLS is here because the server may not be on this desk. omlx.ai-at-home.de
 *   answers 308 on port 80, so a watch that talks to it over the internet has
 *   no plaintext option; an oMLX on the same machine still does, and an
 *   http:// address skips all of this.
 *
 * - **chatterbox-multilingual-v3 ships no voice conditionals**, so it answers
 *   500 to every request that carries no clip to clone. the clip named by
 *   WATCH_VOICE_CLIP and the transcript beside it are what it borrows a voice
 *   from, they are gitignored, and without them the loop stays quiet —
 *   the same choice the firmware makes about an unconfigured SSID.
 */

#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "speech.h"
#include "turn.h"
#include "voice.h"

/* ------------------------------------------------------------------ */
/* The loop's state. A request in flight has to be able to see that the window
 * was closed, which is what window_closed() below hands to speech.c. */
static SDL_atomic_t awake;     /* the name has been heard, the goodbye has not */
static SDL_atomic_t recording; /* the microphone is dequeuing */
static SDL_atomic_t speaking;
static SDL_atomic_t running;

/* Handed to speech_cancel_set() so a request gives up when the close box is
 * hit, rather than holding the process open for the rest of the HTTP budget.
 * It is the only thing speech.c knows about this file. */
static bool window_closed(void)
{
    return SDL_AtomicGet(&running) == 0;
}



/* ------------------------------------------------------------------ */
/* ------------------------------------------------------------------ */
/* The loop.                                                                  */
/* ------------------------------------------------------------------ */

static SDL_AudioDeviceID microphone;
static SDL_AudioDeviceID speaker;
static SDL_AudioSpec     speaker_spec;

static SDL_Thread * worker;
static int16_t *    pcm; /* one turn's recording */

/* What there is to listen to. SDL opens the system default and will not say
 * which device that is: SDL_GetDefaultAudioInfo answers with the literal words
 * "System default" on sdl2-compat, which is what Homebrew's sdl2 now is. So
 * the log lists the candidates instead and leaves the naming to System
 * Settings, which is where the choice is made anyway. The device half needs
 * none of this — its microphones are soldered on. */
static void log_inputs(void)
{
    int n = SDL_GetNumAudioDevices(1);
    int i;

    if (n <= 0) {
        SDL_Log("voice: SDL can see no inputs at all");
        return;
    }
    for (i = 0; i < n; i++) {
        SDL_Log("voice:   input %d: %s", i, SDL_GetAudioDeviceName(i, 1));
    }
}

/* Records one utterance and returns how many samples it was.
 *
 * `patience_ms` is how long it will wait for the first word before giving up;
 * VOICE_WAIT_FOREVER waits, which is what the loop asks for while the
 * assistant is asleep and there is nothing to time out of. Everything about
 * when a turn starts and ends is voice/turn.c's; all that is here is working
 * the device.
 *
 * Zero back means nothing worth sending: the patience ran out, what arrived
 * was too short to be a word, or the loop is shutting down. */
static size_t record(uint32_t patience_ms)
{
    voice_turn_t turn;

    voice_turn_start(&turn, patience_ms);

    SDL_ClearQueuedAudio(microphone); /* whatever accumulated while idle */
    SDL_PauseAudioDevice(microphone, 0);
    SDL_AtomicSet(&recording, 1);

    while (SDL_AtomicGet(&running)) {
        size_t at = voice_turn_at(&turn);
        uint32_t got;

        SDL_Delay(VOICE_BLOCK_MS);

        /* Wall time rather than samples, so a device that has stopped
         * delivering still hands the deadline back. */
        if (voice_turn_wait(&turn) == VOICE_NOTHING) break;
        if (voice_turn_dead(&turn)) {
            SDL_Log("voice: the default input has delivered nothing but zeros for "
                    "%d s. A room never reads zero, so it is not hearing — pick one "
                    "of the inputs above, in System Settings > Sound > Input.",
                    VOICE_DEAD_MS / 1000);
        }

        got = SDL_DequeueAudio(microphone, pcm + at,
                               (uint32_t) ((VOICE_MAX_SAMPLES - at) * sizeof(int16_t)));
        if (voice_turn_feed(&turn, pcm + at, got / sizeof(int16_t)) == VOICE_DONE) break;
    }

    SDL_PauseAudioDevice(microphone, 1);
    SDL_AtomicSet(&recording, 0);

    return voice_turn_samples(&turn);
}

/* Plays a WAV, opening the speaker to match it. Returns when the last sample
 * has gone out, or at once when the window is closed.
 *
 * Nothing else cuts a reply short. Talking over the assistant means being
 * heard while it is speaking, which needs the acoustic echo cancellation this
 * half-duplex loop has none of — the same reason the microphone and speaker
 * corners are two flags rather than one mode. ESP-SR gives the S3 that, and
 * the interrupt comes back with it. */
static void play(const unsigned char * file, size_t len)
{
    voice_wav_t wav;
    SDL_AudioSpec want;

    if (!voice_wav_open(file, len, &wav)) {
        SDL_Log("voice: the reply is not a WAV this can play");
        return;
    }

    memset(&want, 0, sizeof(want));
    want.channels = (Uint8) wav.channels;
    want.freq = wav.rate;
    want.format = wav.bits == 8 ? AUDIO_U8 : AUDIO_S16LSB;
    want.samples = 2048;

    /* The server has answered 24 kHz mono every time, but it says so in the
     * header rather than promising it, so the device follows the file. */
    if (speaker == 0 || speaker_spec.freq != want.freq ||
        speaker_spec.channels != want.channels || speaker_spec.format != want.format) {
        if (speaker != 0) SDL_CloseAudioDevice(speaker);
        speaker = SDL_OpenAudioDevice(NULL, 0, &want, &speaker_spec, 0);
        if (speaker == 0) {
            SDL_Log("voice: no speaker: %s", SDL_GetError());
            return;
        }
    }

    SDL_AtomicSet(&speaking, 1);
    SDL_ClearQueuedAudio(speaker);
    if (SDL_QueueAudio(speaker, wav.pcm, (Uint32) wav.bytes) == 0) {
        SDL_PauseAudioDevice(speaker, 0);
        while (SDL_GetQueuedAudioSize(speaker) > 0 && SDL_AtomicGet(&running)) {
            SDL_Delay(VOICE_BLOCK_MS);
        }
    }
    SDL_PauseAudioDevice(speaker, 1);
    SDL_AtomicSet(&speaking, 0);
}

/* One pass is one utterance. Asleep, the only thing that matters about it is
 * whether the watch's name is in there; awake, all of it is something to
 * answer, except the goodbye. Both questions are voice/turn.c's to answer, so
 * the firmware's loop asks them in the same order and gets the same replies.
 *
 * The microphone never closes, so every sentence spoken in the room goes to
 * the transcriber, which is the price of having no wake-word engine here. */
static int loop(void * unused)
{
    (void) unused;

    while (SDL_AtomicGet(&running)) {
        char heard[VOICE_MAX_TEXT];
        char answer[VOICE_MAX_TEXT];
        voice_buf_t wav = { 0 };
        const char * say;
        size_t samples;

        /* Awake, a stretch with nothing said is the end of the session: with
         * nothing to press, there has to be a way back to the clock that
         * needs nothing said at all. Asleep, there is nothing to leave. */
        samples = record(SDL_AtomicGet(&awake) ? VOICE_IDLE_MS : VOICE_WAIT_FOREVER);
        if (samples == 0) {
            if (SDL_AtomicGet(&awake) && SDL_AtomicGet(&running)) {
                SDL_Log("voice: nothing said for %d s — back to the clock",
                        VOICE_IDLE_MS / 1000);
                SDL_AtomicSet(&awake, 0);
            }
            continue;
        }

        if (!speech_transcribe(pcm, samples, heard, sizeof(heard))) continue;
        if (heard[0] == '\0') continue; /* the transcriber heard no words */

        say = heard;

        if (!SDL_AtomicGet(&awake)) {
            const char * rest = voice_after_wake(heard);
            if (rest == NULL) {
                /* The room talking, not the watch. Logged rather than
                 * dropped: the transcriber has never been shown this name and
                 * writes down whatever sounded closest, so choosing the
                 * spellings to listen for means reading what it actually
                 * produced. Silence here is what makes a watch that will not
                 * wake impossible to diagnose. */
                SDL_Log("voice: not for me: \"%s\"", heard);
                continue;
            }
            SDL_Log("voice: woken by \"%s\"", heard);
            SDL_AtomicSet(&awake, 1);
            if (*rest == '\0') continue; /* its name and nothing after it */
            say = rest; /* "Hey Kai, hallo" is a turn whose answer is "hallo" */
        }
        else if (voice_is_goodbye(heard)) {
            SDL_Log("voice: \"%s\" — back to the clock", heard);
            SDL_AtomicSet(&awake, 0);
            continue;
        }
        else {
            SDL_Log("voice: heard \"%s\"", heard);
        }

        if (!speech_reply(say, answer, sizeof answer)) continue;
        if (voice_models_get()->brain[0] != '\0') SDL_Log("voice: answering \"%s\"", answer);

        if (speech_synthesise(answer, &wav)) {
            play((const unsigned char *) wav.data, wav.len);
        }
        voice_buf_free(&wav);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Start-up.                                                                  */
/* ------------------------------------------------------------------ */


void voice_start(void)
{
    SDL_AudioSpec want, have;

    speech_cancel_set(window_closed);
    if (!speech_start()) return;

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        SDL_Log("voice: no audio: %s", SDL_GetError());
        return;
    }

    pcm = malloc(VOICE_MAX_SAMPLES * sizeof(int16_t));
    if (pcm == NULL) return;

    memset(&want, 0, sizeof(want));
    want.freq = VOICE_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 1024;
    microphone = SDL_OpenAudioDevice(NULL, 1, &want, &have, 0);
    if (microphone == 0) {
        SDL_Log("voice: no microphone: %s", SDL_GetError());
        return;
    }
    if (have.freq != VOICE_RATE || have.channels != 1) {
        /* Nothing here resamples, so a device that will not open at 16 kHz
         * mono would be transcribed at the wrong speed. */
        SDL_Log("voice: the microphone opened at %d Hz, %d channels, not %d mono",
                have.freq, have.channels, VOICE_RATE);
        SDL_CloseAudioDevice(microphone);
        microphone = 0;
        return;
    }
    SDL_PauseAudioDevice(microphone, 1);
    SDL_Log("voice: listening through the system default at %d Hz mono", have.freq);
    log_inputs();

    SDL_AtomicSet(&running, 1);
    worker = SDL_CreateThread(loop, "kai-voice", NULL);
    if (worker == NULL) {
        SDL_Log("voice: no thread: %s", SDL_GetError());
        SDL_AtomicSet(&running, 0);
        return;
    }
    SDL_Log("voice: waiting to be called, hearing through %s and speaking as %s",
            voice_models_get()->stt, voice_models_get()->tts);
    if (voice_models_get()->brain[0] != '\0') {
        char tools[VOICE_TOOL_LOG_BYTES];

        SDL_Log("voice: thinking with %s, and it can ask for %s",
                voice_models_get()->brain, voice_tool_list(tools, sizeof tools));
    }
    else {
        SDL_Log("voice: %s=%s — the answer is the question said back",
                VOICE_ENV_BRAIN, VOICE_BRAIN_ECHO);
    }
}

void voice_stop(void)
{
    SDL_AtomicSet(&running, 0);
    SDL_AtomicSet(&awake, 0);
    if (worker != NULL) {
        SDL_WaitThread(worker, NULL);
        worker = NULL;
    }
    if (microphone != 0) SDL_CloseAudioDevice(microphone);
    if (speaker != 0) SDL_CloseAudioDevice(speaker);
    microphone = speaker = 0;
    free(pcm);
    pcm = NULL;
    speech_stop();
}

bool voice_awake(void)
{
    return SDL_AtomicGet(&awake) != 0;
}

/* Awake as well as recording. The microphone is open the whole time the loop
 * runs, but a corner that is always lit says nothing — this one means the
 * next thing said is meant for the assistant. */
bool voice_listening(void)
{
    return SDL_AtomicGet(&recording) != 0 && SDL_AtomicGet(&awake) != 0;
}

bool voice_speaking(void)
{
    return SDL_AtomicGet(&speaking) != 0;
}
