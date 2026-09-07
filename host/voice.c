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
 *   500 to every request that carries no clip to clone. voices/kai.opus and
 *   the transcript beside it are what it borrows a voice from, they are
 *   gitignored, and without them the loop stays quiet rather than failing —
 *   the same choice the firmware makes about an unconfigured SSID.
 */

#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>

#include <errno.h>
#include <netdb.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "turn.h"
#include "voice.h"

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


/* The voice KAI borrows, relative to the working directory — `make run`
 * starts the binary from the repository root. Both are gitignored: the clip
 * is a recording of a person, and this repository is licensed. */
#define VOICE_REF_AUDIO "voices/kai.opus"
#define VOICE_REF_TEXT  "voices/kai.txt"

/* Transcription answers in about a second, a chat model in several, and
 * synthesis takes roughly as long as the speech it produces, so the budget is
 * the ceiling for a stalled server rather than a working one. The socket gives
 * up far sooner than that and the read loop simply goes round again, which is
 * what makes closing the window during a request cost a second rather than two
 * minutes: the loop notices the shutdown between waits. */
#define VOICE_HTTP_TIMEOUT_S 120
#define VOICE_RECV_TIMEOUT_S 1

/* ------------------------------------------------------------------ */
/* HTTP. One connection per request, closed at the end of it: a turn makes    */
/* three requests seconds apart, so a keep-alive pool would be bookkeeping     */
/* for nothing. No redirects — the address is asked for exactly what it        */
/* publishes — and all three services answer with a Content-Length.            */
/* ------------------------------------------------------------------ */

/* The loop's state, up here because a request in flight has to be able to see
 * that the window was closed. */
static SDL_atomic_t awake;     /* the name has been heard, the goodbye has not */
static SDL_atomic_t recording; /* the microphone is dequeuing */
static SDL_atomic_t speaking;
static SDL_atomic_t running;

/* The server, and what it wants to see. Both are read once, at start-up. */
static voice_url_t server;
static char        api_key[VOICE_KEY_BYTES];
static SSL_CTX *   tls;

/* One connection. The socket is always there; the SSL sits on top of it when
 * the address said https, and every byte of a request goes through the two
 * functions below so the request itself is written in exactly one place
 * whichever it is. */
typedef struct {
    int   fd;
    SSL * ssl;
} conn_t;

static void conn_close(conn_t * c)
{
    if (c->ssl != NULL) {
        SSL_shutdown(c->ssl);
        SSL_free(c->ssl);
        c->ssl = NULL;
    }
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
}

/* The most recent OpenSSL failure as a sentence, because "SSL_connect failed"
 * on its own has sent people to the wrong end of the problem before. */
static const char * tls_error(void)
{
    static char text[160];
    unsigned long code = ERR_get_error();

    if (code == 0) return strerror(errno);
    ERR_error_string_n(code, text, sizeof text);
    return text;
}

static bool conn_open(conn_t * c)
{
    struct addrinfo hints, * found = NULL;
    struct timeval timeout;
    int rc;

    c->fd = -1;
    c->ssl = NULL;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    rc = getaddrinfo(server.host, server.port, &hints, &found);
    if (rc != 0 || found == NULL) {
        SDL_Log("voice: %s: %s", server.host, gai_strerror(rc));
        return false;
    }

    c->fd = socket(found->ai_family, found->ai_socktype, found->ai_protocol);
    if (c->fd < 0) {
        freeaddrinfo(found);
        return false;
    }

    /* A short read timeout rather than a long one: the loop goes round on it
     * and looks at whether the window was closed, which is what makes closing
     * it during a request cost a second rather than two minutes. */
    timeout.tv_sec = VOICE_RECV_TIMEOUT_S;
    timeout.tv_usec = 0;
    setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    timeout.tv_sec = VOICE_HTTP_TIMEOUT_S;
    setsockopt(c->fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    if (connect(c->fd, found->ai_addr, found->ai_addrlen) != 0) {
        SDL_Log("voice: %s:%s is not answering — is there a server there?",
                server.host, server.port);
        freeaddrinfo(found);
        conn_close(c);
        return false;
    }
    freeaddrinfo(found);

    if (!server.tls) return true;

    c->ssl = SSL_new(tls);
    if (c->ssl == NULL) {
        conn_close(c);
        return false;
    }
    /* Two calls, two different jobs, and leaving either out is a quiet
     * failure. SNI is how a proxy that fronts several names knows which
     * certificate to present — without it Caddy answers with the wrong site
     * or none. set1_host is what makes OpenSSL check that the certificate it
     * got actually belongs to the name we asked for; verification without it
     * proves only that some CA signed something. */
    SSL_set_tlsext_host_name(c->ssl, server.host);
    SSL_set1_host(c->ssl, server.host);
    SSL_set_fd(c->ssl, c->fd);

    if (SSL_connect(c->ssl) != 1) {
        long verify = SSL_get_verify_result(c->ssl);
        if (verify != X509_V_OK) {
            SDL_Log("voice: %s: the certificate did not check out: %s",
                    server.host, X509_verify_cert_error_string(verify));
        }
        else {
            SDL_Log("voice: %s: TLS failed: %s", server.host, tls_error());
        }
        conn_close(c);
        return false;
    }
    return true;
}

static bool conn_send(conn_t * c, const void * bytes, size_t len)
{
    const char * at = bytes;

    while (len > 0) {
        int sent = c->ssl != NULL
                       ? SSL_write(c->ssl, at, (int) len)
                       : (int) send(c->fd, at, len, 0);
        if (sent <= 0) return false;
        at += sent;
        len -= (size_t) sent;
    }
    return true;
}

/* Bytes, or 0 for the end of the stream, -1 for nothing yet, -2 for broken.
 * The three are kept apart because the read loop treats them differently: it
 * goes round on "nothing yet" to look at whether the window was closed, and a
 * stream that ended is the ordinary way a reply finishes here. */
#define CONN_AGAIN  (-1)
#define CONN_BROKEN (-2)

static int conn_recv(conn_t * c, void * bytes, size_t len)
{
    int got;

    if (c->ssl == NULL) {
        ssize_t n = recv(c->fd, bytes, len, 0);
        if (n >= 0) return (int) n;
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return CONN_AGAIN;
        return CONN_BROKEN;
    }

    got = SSL_read(c->ssl, bytes, (int) len);
    if (got > 0) return got;

    switch (SSL_get_error(c->ssl, got)) {
        case SSL_ERROR_ZERO_RETURN:
            return 0; /* closed properly, with a close_notify */
        case SSL_ERROR_WANT_READ:
        case SSL_ERROR_WANT_WRITE:
            return CONN_AGAIN;
        case SSL_ERROR_SYSCALL:
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return CONN_AGAIN;
            /* Closed without a close_notify, which plenty of servers do. The
             * body is already in hand, so this is the end of it and not a
             * failure to report. */
            if (errno == 0) return 0;
            return CONN_BROKEN;
        default:
            return CONN_BROKEN;
    }
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
 * failure: it is the fastest route to the cause, and all three services answer
 * a bad request with a sentence saying what was wrong. */
static bool http_post(const char * path, const char * content_type,
                      const void * body, size_t body_len,
                      voice_buf_t * out, char * mime, size_t mime_cap)
{
    conn_t conn;
    char head[1024];
    char host_header[VOICE_HOST_BYTES + VOICE_PORT_BYTES + 2];
    char auth[VOICE_KEY_BYTES + 32];
    voice_buf_t raw = { 0 };
    const char * split;
    char length_header[32];
    size_t header_len, want;
    int status = 0, waited;
    bool ok = false;

    if (!conn_open(&conn)) return false;

    /* The port comes off when it is the scheme's own, which is what every
     * other client sends and what a proxy matching on the name expects. */
    if ((server.tls && strcmp(server.port, "443") == 0) ||
        (!server.tls && strcmp(server.port, "80") == 0)) {
        snprintf(host_header, sizeof host_header, "%s", server.host);
    }
    else {
        snprintf(host_header, sizeof host_header, "%s:%s", server.host, server.port);
    }

    /* No key, no header. A server that wants none is not sent an empty one:
     * an "Authorization: Bearer " with nothing behind it is a rejected
     * request rather than an unauthenticated one. */
    if (api_key[0] != '\0') {
        snprintf(auth, sizeof auth, "Authorization: Bearer %s\r\n", api_key);
    }
    else {
        auth[0] = '\0';
    }

    snprintf(head, sizeof(head),
             "POST %s HTTP/1.1\r\n"
             "Host: %s\r\n"
             "%s"
             "Content-Type: %s\r\n"
             "Content-Length: %zu\r\n"
             "Accept: */*\r\n"
             "Connection: close\r\n"
             "\r\n",
             path, host_header, auth, content_type, body_len);

    if (!conn_send(&conn, head, strlen(head)) || !conn_send(&conn, body, body_len)) {
        SDL_Log("voice: sending to %s failed", path);
        conn_close(&conn);
        return false;
    }

    /* Connection: close, so the reply ends at end of file and there is no
     * chunked framing to unpick. */
    for (waited = 0; waited < VOICE_HTTP_TIMEOUT_S; ) {
        char chunk[8192];
        int got = conn_recv(&conn, chunk, sizeof(chunk));
        if (got == 0) break;
        if (got == CONN_BROKEN) goto done;
        if (got == CONN_AGAIN) {
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
        if (status == 401 || status == 403) {
            SDL_Log("voice: the server wants a key — set %s", VOICE_ENV_KEY);
        }
        goto done;
    }

    /* Content-Length when it is there, the rest of the stream when it is
     * not. All three services send one. */
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
    conn_close(&conn);
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

/* What the assistant answers, into out.
 *
 * With no brain configured this is the question said back, word for word and
 * with nothing in front of it: the point of that mode is to hear what the
 * microphone and the transcriber actually produced, and a preamble is both an
 * extra sentence to sit through and a way to miss that the transcript was
 * wrong. It is also the only mode that needs no third service.
 *
 * With one configured, the same words go to a chat model and its answer comes
 * back. A failure here is silence rather than an echo: a watch that repeats
 * the question when the model could not be reached looks like it answered. */
static bool reply(const char * heard, char * out, size_t cap)
{
    voice_buf_t body = { 0 }, answer = { 0 };
    bool ok = false;

    if (voice_models_get()->brain[0] == '\0') {
        snprintf(out, cap, "%s", heard);
        return true;
    }

    if (!voice_brain_body(&body, heard)) goto done;
    if (!http_post(VOICE_BRAIN_PATH, VOICE_BRAIN_TYPE, body.data, body.len,
                   &answer, NULL, 0)) goto done;

    ok = voice_brain_text(answer.data, out, cap);
    if (!ok) SDL_Log("voice: no answer in what %s sent back", voice_models_get()->brain);

done:
    voice_buf_free(&body);
    voice_buf_free(&answer);
    return ok;
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

        if (!transcribe(pcm, samples, heard, sizeof(heard))) continue;
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

        if (!reply(say, answer, sizeof answer)) continue;
        if (voice_models_get()->brain[0] != '\0') SDL_Log("voice: answering \"%s\"", answer);

        if (synthesise(answer, &wav)) {
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

/* One setting, or the fallback when it is unset or empty. Empty is treated as
 * unset throughout: an exported variable someone cleared should mean the same
 * as one they never wrote. */
static const char * env(const char * name, const char * fallback)
{
    const char * value = getenv(name);

    return (value != NULL && value[0] != '\0') ? value : fallback;
}

/* Everything the platform knows and turn.c does not: where the server is,
 * what it wants to see, which models answer and what the watch is called.
 * The device does the same six reads out of menuconfig.
 *
 * False means there is nowhere to ask, which is a clock and not an error —
 * the same answer a missing clip gives. */
static bool configure(void)
{
    voice_models_t models;
    const char * url = env(VOICE_ENV_URL, VOICE_URL_DEFAULT);

    if (!voice_url_parse(url, &server)) {
        SDL_Log("voice: %s is not an address this can dial (%s) — the watch stays a clock",
                url, VOICE_ENV_URL);
        return false;
    }
    snprintf(api_key, sizeof api_key, "%s", env(VOICE_ENV_KEY, ""));

    /* NULL rather than the default spelled out again: an unset variable says
     * nothing and turn.c uses the one list it holds. */
    models.stt = env(VOICE_ENV_STT, NULL);
    models.brain = env(VOICE_ENV_BRAIN, NULL);
    models.tts = env(VOICE_ENV_TTS, NULL);
    models.language = env(VOICE_ENV_LANGUAGE, NULL);
    if (!voice_models_set(&models)) {
        SDL_Log("voice: a model name does not fit — using the default for it");
    }
    if (!voice_name_set(env(VOICE_ENV_NAME, NULL))) {
        SDL_Log("voice: that name does not fit — the assistant is still %s", voice_name());
    }
    if (!voice_wake_set(env(VOICE_ENV_WAKE, NULL))) {
        SDL_Log("voice: that wake phrase does not fit — listening for the default");
    }
    /* Both on one line, because they are two settings and forgetting the
     * second is the mistake: a watch renamed only in the wake phrase still
     * answers "who are you?" with the name in its system prompt. Said out
     * loud, a phrase that did not fit and the default it was replaced by also
     * stop looking identical. */
    if (voice_wake_count() == 1) {
        SDL_Log("voice: called %s, answering to \"%s\"", voice_name(), voice_wake_at(0));
    }
    else {
        SDL_Log("voice: called %s, answering to \"%s\" and %zu other spellings of it",
                voice_name(), voice_wake_at(0), voice_wake_count() - 1);
    }

    if (server.tls) {
        tls = SSL_CTX_new(TLS_client_method());
        if (tls == NULL) {
            SDL_Log("voice: no TLS: %s", tls_error());
            return false;
        }
        /* Verification on, and the system CA store behind it. Off, an https
         * address would prove nothing at all — which is worse than the
         * plaintext it replaced, because it looks like it proved something. */
        SSL_CTX_set_verify(tls, SSL_VERIFY_PEER, NULL);
        SSL_CTX_set_min_proto_version(tls, TLS1_2_VERSION);
        if (SSL_CTX_set_default_verify_paths(tls) != 1) {
            SDL_Log("voice: no CA store, so no certificate can be checked: %s",
                    tls_error());
            return false;
        }
    }

    /* The key is never logged, only whether there is one — this is the log a
     * screenshot goes into. */
    SDL_Log("voice: %s://%s:%s, %s key",
            server.tls ? "https" : "http", server.host, server.port,
            api_key[0] != '\0' ? "with a" : "with no");
    return true;
}

void voice_start(void)
{
    SDL_AudioSpec want, have;

    if (!configure()) return;

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
        SDL_Log("voice: thinking with %s", voice_models_get()->brain);
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
    voice_free(ref_audio);
    free(ref_text);
    pcm = NULL;
    ref_audio = ref_text = NULL;
    if (tls != NULL) {
        SSL_CTX_free(tls);
        tls = NULL;
    }
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
