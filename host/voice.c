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
 * The answer is the question, said back word for word. That is a mode rather
 * than a placeholder: it is the shortest path through the whole pipeline, so
 * a turn that breaks here breaks everywhere, and what comes out of the
 * speaker is exactly what the transcriber heard. A language model belongs
 * behind the same seam later — reply() is the only function that would change.
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
 * - **No libraries beyond SDL2.** SPEC.md allows LVGL and SDL2 and nothing
 *   else, so the HTTP client below is a socket, a request written by hand and
 *   a reply read back. That is not an inconvenience: the device speaks to the
 *   same two endpoints through esp_http_client, and a body built by hand ports
 *   where a libcurl call site would not — which is why voice/turn.c builds
 *   both bodies and neither platform does.
 *
 * - **chatterbox-multilingual-v3 ships no voice conditionals**, so it answers
 *   500 to every request that carries no clip to clone. voices/kai.opus and
 *   the transcript beside it are what it borrows a voice from, they are
 *   gitignored, and without them the loop stays quiet rather than failing —
 *   the same choice the firmware makes about an unconfigured SSID.
 */

#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>

#include <errno.h>
#include <netdb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "turn.h"
#include "voice.h"

/* Where the server is. The two model names and the language are not here:
 * they are the same decision on both platforms, so they live in turn.h. */
#define VOICE_HOST "127.0.0.1"
#define VOICE_PORT "8000"

/* The voice KAI borrows, relative to the working directory — `make run`
 * starts the binary from the repository root. Both are gitignored: the clip
 * is a recording of a person, and this repository is licensed. */
#define VOICE_REF_AUDIO "voices/kai.opus"
#define VOICE_REF_TEXT  "voices/kai.txt"

/* Transcription answers in about a second and synthesis takes roughly as long
 * as the speech it produces, so the budget is the ceiling for a stalled server
 * rather than a working one. The socket gives up far sooner than that and the
 * read loop simply goes round again, which is what makes closing the window
 * during a request cost a second rather than two minutes: the loop notices the
 * shutdown between waits. */
#define VOICE_HTTP_TIMEOUT_S 120
#define VOICE_RECV_TIMEOUT_S 1

/* ------------------------------------------------------------------ */
/* HTTP. One connection per request, closed at the end of it: a turn makes    */
/* two requests seconds apart, so a keep-alive pool would be bookkeeping for  */
/* nothing. Plaintext to localhost — no TLS, no redirects, and both services  */
/* answer with a Content-Length.                                              */
/* ------------------------------------------------------------------ */

/* The loop's state, up here because a request in flight has to be able to see
 * that the window was closed. */
static SDL_atomic_t awake;     /* the name has been heard, the goodbye has not */
static SDL_atomic_t recording; /* the microphone is dequeuing */
static SDL_atomic_t speaking;
static SDL_atomic_t running;

static bool send_all(int fd, const void * bytes, size_t len)
{
    const char * at = bytes;
    while (len > 0) {
        ssize_t sent = send(fd, at, len, 0);
        if (sent <= 0) return false;
        at += sent;
        len -= (size_t) sent;
    }
    return true;
}

/* The value of one header, lower-cased comparison, into out. */
static bool header_value(const char * headers, const char * name, char * out, size_t cap)
{
    size_t name_len = strlen(name);
    const char * line = headers;

    while ((line = strchr(line, '\n')) != NULL) {
        size_t i = 0;
        line++;
        if (strncasecmp(line, name, name_len) != 0) continue;
        if (line[name_len] != ':') continue;
        line += name_len + 1;
        while (*line == ' ' || *line == '\t') line++;
        while (line[i] != '\0' && line[i] != '\r' && line[i] != '\n' && i + 1 < cap) {
            out[i] = line[i];
            i++;
        }
        out[i] = '\0';
        return true;
    }
    return false;
}

/* POSTs body to path and returns the reply body in out, with its content type
 * in mime. Reports the server's own status and the head of its message on a
 * failure: it is the fastest route to the cause, and both services answer a
 * bad request with a sentence saying what was wrong. */
static bool http_post(const char * path, const char * content_type,
                      const void * body, size_t body_len,
                      voice_buf_t * out, char * mime, size_t mime_cap)
{
    struct addrinfo hints, * found = NULL;
    struct timeval timeout;
    char head[512];
    voice_buf_t raw = { 0 };
    const char * split;
    char length_header[32];
    size_t header_len, want;
    int fd, status = 0, rc, waited;
    bool ok = false;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    rc = getaddrinfo(VOICE_HOST, VOICE_PORT, &hints, &found);
    if (rc != 0 || found == NULL) {
        SDL_Log("voice: %s: %s", VOICE_HOST, gai_strerror(rc));
        return false;
    }

    fd = socket(found->ai_family, found->ai_socktype, found->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(found);
        return false;
    }

    timeout.tv_sec = VOICE_RECV_TIMEOUT_S;
    timeout.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    timeout.tv_sec = VOICE_HTTP_TIMEOUT_S; /* localhost never blocks on a send */
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    if (connect(fd, found->ai_addr, found->ai_addrlen) != 0) {
        SDL_Log("voice: %s:%s is not answering — is oMLX running?", VOICE_HOST, VOICE_PORT);
        freeaddrinfo(found);
        close(fd);
        return false;
    }
    freeaddrinfo(found);

    snprintf(head, sizeof(head),
             "POST %s HTTP/1.1\r\n"
             "Host: " VOICE_HOST ":" VOICE_PORT "\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %zu\r\n"
             "Accept: */*\r\n"
             "Connection: close\r\n"
             "\r\n",
             path, content_type, body_len);

    if (!send_all(fd, head, strlen(head)) || !send_all(fd, body, body_len)) {
        SDL_Log("voice: sending to %s failed", path);
        close(fd);
        return false;
    }

    /* Connection: close, so the reply ends at end of file and there is no
     * chunked framing to unpick. */
    for (waited = 0; waited < VOICE_HTTP_TIMEOUT_S; ) {
        char chunk[8192];
        ssize_t got = recv(fd, chunk, sizeof(chunk), 0);
        if (got == 0) break;
        if (got < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) goto done;
            /* Nothing yet. Go round, unless the window has been closed — this
             * is the only place a request in flight can be abandoned. */
            waited += VOICE_RECV_TIMEOUT_S;
            if (!SDL_AtomicGet(&running)) goto done;
            continue;
        }
        if (!voice_buf_add(&raw, chunk, (size_t) got)) goto done;
    }
    if (waited >= VOICE_HTTP_TIMEOUT_S) {
        SDL_Log("voice: %s gave up after %d s", path, VOICE_HTTP_TIMEOUT_S);
        goto done;
    }

    if (raw.len < 12 || sscanf(raw.data, "HTTP/%*d.%*d %d", &status) != 1) {
        SDL_Log("voice: %s answered something that is not HTTP", path);
        goto done;
    }

    split = strstr(raw.data, "\r\n\r\n");
    if (split == NULL) goto done;
    header_len = (size_t) (split - raw.data) + 4;

    if (status != 200) {
        SDL_Log("voice: %s: HTTP %d: %.200s", path, status, raw.data + header_len);
        goto done;
    }

    /* Content-Length when it is there, the rest of the stream when it is
     * not. Both services send one. */
    want = raw.len - header_len;
    if (header_value(raw.data, "content-length", length_header, sizeof(length_header))) {
        size_t claimed = strtoul(length_header, NULL, 10);
        if (claimed < want) want = claimed;
    }
    if (mime != NULL && !header_value(raw.data, "content-type", mime, mime_cap)) {
        mime[0] = '\0';
    }

    ok = voice_buf_add(out, raw.data + header_len, want);

done:
    close(fd);
    voice_buf_free(&raw);
    return ok;
}

/* ------------------------------------------------------------------ */
/* The two services. Both bodies are built in voice/turn.c; all that is left  */
/* here is putting them on a socket.                                          */
/* ------------------------------------------------------------------ */

static char * ref_audio; /* the clip to clone, base64, encoded once */
static char * ref_text;  /* what that clip says */

/* What was said, or false when nothing was. Silence that tripped the gate is
 * the ordinary outcome of a door closing, not an error. */
static bool transcribe(const int16_t * pcm, size_t samples, char * out, size_t cap)
{
    voice_buf_t body = { 0 }, reply = { 0 };
    bool ok = false;

    if (!voice_stt_body(&body, pcm, samples)) goto done;
    if (!http_post(VOICE_STT_PATH, VOICE_STT_TYPE, body.data, body.len,
                   &reply, NULL, 0)) goto done;

    ok = voice_stt_text(reply.data, out, cap);
    if (!ok) SDL_Log("voice: no transcript in the reply");

done:
    voice_buf_free(&body);
    voice_buf_free(&reply);
    return ok;
}

/* text as audio, in whatever format the server chose to answer with. */
static bool synthesise(const char * text, voice_buf_t * wav)
{
    voice_buf_t body = { 0 };
    char mime[64];
    bool ok = false;

    if (!voice_tts_body(&body, text, ref_audio, ref_text)) goto done;

    ok = http_post(VOICE_TTS_PATH, VOICE_TTS_TYPE, body.data, body.len,
                   wav, mime, sizeof(mime));
    if (ok && strncmp(mime, "audio/", 6) != 0) {
        SDL_Log("voice: the synthesiser answered %s, not audio", mime);
        ok = false;
    }

done:
    voice_buf_free(&body);
    return ok;
}

/* ------------------------------------------------------------------ */
/* The loop.                                                                  */
/* ------------------------------------------------------------------ */

static SDL_AudioDeviceID microphone;
static SDL_AudioDeviceID speaker;
static SDL_AudioSpec     speaker_spec;

static SDL_Thread * worker;
static int16_t *    pcm; /* one turn's recording */

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

/* What the assistant answers. Word for word, with nothing in front of it: the
 * point of this mode is to hear what the microphone and the transcriber
 * actually produced, and a preamble is both an extra sentence to sit through
 * and a way to miss that the transcript was wrong.
 *
 * This is the seam a language model goes behind. */
static const char * reply(const char * heard)
{
    return heard;
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

        if (!transcribe(pcm, samples, heard, sizeof(heard))) continue;
        if (heard[0] == '\0') continue; /* the transcriber heard no words */

        say = heard;

        if (!SDL_AtomicGet(&awake)) {
            const char * rest = voice_after_wake(heard);
            if (rest == NULL) continue; /* the room talking, not the watch */
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

        if (synthesise(reply(say), &wav)) {
            play((const unsigned char *) wav.data, wav.len);
        }
        voice_buf_free(&wav);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Start-up.                                                                  */
/* ------------------------------------------------------------------ */

/* The whole of a file, NUL-terminated, or NULL. */
static unsigned char * slurp(const char * path, size_t * len)
{
    unsigned char * bytes;
    long size;
    FILE * file = fopen(path, "rb");

    if (file == NULL) return NULL;
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0) {
        fclose(file);
        return NULL;
    }
    rewind(file);

    bytes = malloc((size_t) size + 1);
    if (bytes == NULL) {
        fclose(file);
        return NULL;
    }
    if (fread(bytes, 1, (size_t) size, file) != (size_t) size) {
        free(bytes);
        fclose(file);
        return NULL;
    }
    fclose(file);
    bytes[size] = '\0';
    if (len != NULL) *len = (size_t) size;
    return bytes;
}

/* Reads the clip KAI borrows its voice from. Without it the synthesiser
 * answers 500 to everything, so the loop simply does not start — the same
 * answer the firmware gives an unconfigured SSID. */
static bool load_voice(void)
{
    size_t clip_len = 0;
    unsigned char * clip = slurp(VOICE_REF_AUDIO, &clip_len);
    unsigned char * words;
    size_t i;

    if (clip == NULL) {
        SDL_Log("voice: no %s — the assistant stays asleep", VOICE_REF_AUDIO);
        return false;
    }
    words = slurp(VOICE_REF_TEXT, NULL);
    if (words == NULL) {
        SDL_Log("voice: no %s — the clip's words are needed too", VOICE_REF_TEXT);
        free(clip);
        return false;
    }

    /* The transcript is a line in a file, so it arrives with a newline on it. */
    for (i = strlen((char *) words); i > 0; i--) {
        if (words[i - 1] != '\n' && words[i - 1] != '\r' && words[i - 1] != ' ') break;
        words[i - 1] = '\0';
    }

    ref_audio = voice_base64(clip, clip_len);
    ref_text = (char *) words;
    free(clip);
    return ref_audio != NULL;
}

void voice_start(void)
{
    SDL_AudioSpec want, have;

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        SDL_Log("voice: no audio: %s", SDL_GetError());
        return;
    }
    if (!load_voice()) return;

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

    SDL_AtomicSet(&running, 1);
    worker = SDL_CreateThread(loop, "kai-voice", NULL);
    if (worker == NULL) {
        SDL_Log("voice: no thread: %s", SDL_GetError());
        SDL_AtomicSet(&running, 0);
        return;
    }
    SDL_Log("voice: waiting to be called, hearing through %s and speaking as %s",
            VOICE_STT_MODEL, VOICE_TTS_MODEL);
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
    voice_free(ref_audio);
    free(ref_text);
    pcm = NULL;
    ref_audio = ref_text = NULL;
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
