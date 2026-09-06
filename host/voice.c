/**
 * @file voice.c
 * The voice loop: microphone -> Parakeet -> Chatterbox -> speaker.
 *
 * Host-only, like main.c. It is the simulator's stand-in for an audio path
 * the board does not have yet — there is no codec wired up and no wake word,
 * so the Mac's microphone and speakers do the job the ES8311 will do later.
 *
 * The answer is the question, said back word for word. That is a mode rather
 * than a placeholder: it is the shortest path through the whole pipeline, so
 * a turn that breaks here breaks everywhere, and what comes out of the
 * speaker is exactly what the transcriber heard. A language model belongs
 * behind the same seam later — reply() is the only function that would change.
 *
 * Three things are worth knowing before touching it:
 *
 * - **Everything here runs on its own thread.** A turn costs seconds and
 *   LVGL is single-threaded, so the loop cannot run in lv_timer_handler().
 *   It publishes two booleans through SDL atomics; main.c reads them in its
 *   status timer and hands them to ui_status_set(). Nothing else crosses.
 *
 * - **No libraries beyond SDL2.** SPEC.md allows LVGL and SDL2 and nothing
 *   else, so the HTTP client below is a socket, a request written by hand and
 *   a reply read back. That is not an inconvenience: the device will speak to
 *   the same two endpoints through esp_http_client, and a body built by hand
 *   ports where a libcurl call site would not.
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
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "voice.h"

/* Where the two models are. Constants rather than configuration: the
 * simulator reads none, and both services are the same oMLX server. */
#define VOICE_HOST      "127.0.0.1"
#define VOICE_PORT      "8000"
#define VOICE_STT_MODEL "parakeet-tdt-0.6b-v3"
#define VOICE_TTS_MODEL "chatterbox-multilingual-v3"
#define VOICE_LANGUAGE  "de"

/* The voice KAI borrows, relative to the working directory — `make run`
 * starts the binary from the repository root. Both are gitignored: the clip
 * is a recording of a person, and this repository is licensed. */
#define VOICE_REF_AUDIO "voices/kai.opus"
#define VOICE_REF_TEXT  "voices/kai.txt"

/* 16 kHz mono is what speech recognition wants, and sdl2-compat opens the
 * microphone at exactly that with no resampling on the way in. */
#define VOICE_RATE     16000
#define VOICE_BLOCK_MS 20

/* Where a turn ends. The threshold was measured rather than guessed: this
 * room reads a mean RMS of 42 and peaks at 82 with nobody talking, and speech
 * runs in the thousands, so 500 is clear of the noise floor and well under
 * the quietest word. Raise it in a louder room. */
#define VOICE_SILENCE_RMS   500
#define VOICE_SILENCE_MS    800
#define VOICE_MIN_SPEECH_MS 300
#define VOICE_MAX_TURN_MS   20000

/* Transcription answers in about a second and synthesis takes roughly as long
 * as the speech it produces, so the budget is the ceiling for a stalled server
 * rather than a working one. The socket gives up far sooner than that and the
 * read loop simply goes round again, which is what makes closing the window
 * during a request cost a second rather than two minutes: the loop notices the
 * shutdown between waits. */
#define VOICE_HTTP_TIMEOUT_S 120
#define VOICE_RECV_TIMEOUT_S 1

#define VOICE_MAX_SAMPLES ((size_t) VOICE_RATE * VOICE_MAX_TURN_MS / 1000)
#define VOICE_MAX_TEXT    4096

/* ------------------------------------------------------------------ */
/* A buffer that grows. Bodies here run from a few hundred bytes to the       */
/* 150 kB of WAV a sentence comes back as, and neither end is known in        */
/* advance.                                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    char * data;
    size_t len;
    size_t cap;
} buf_t;

static void buf_free(buf_t * b)
{
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

static bool buf_add(buf_t * b, const void * bytes, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t want = b->cap ? b->cap : 256;
        char * grown;
        while (want < b->len + n + 1) want *= 2;
        grown = realloc(b->data, want);
        if (grown == NULL) return false;
        b->data = grown;
        b->cap = want;
    }
    memcpy(b->data + b->len, bytes, n);
    b->len += n;
    b->data[b->len] = '\0'; /* so a body can be read as a string when it is one */
    return true;
}

static bool buf_str(buf_t * b, const char * s)
{
    return buf_add(b, s, strlen(s));
}

/* ------------------------------------------------------------------ */
/* Base64, for the clip the synthesiser clones. Encoded once at start-up:     */
/* it is the same twenty kilobytes on every request.                          */
/* ------------------------------------------------------------------ */

static char * base64(const unsigned char * in, size_t len)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char * out = malloc(4 * ((len + 2) / 3) + 1);
    size_t i, o = 0;

    if (out == NULL) return NULL;

    for (i = 0; i + 2 < len; i += 3) {
        uint32_t triple = ((uint32_t) in[i] << 16) | ((uint32_t) in[i + 1] << 8) | in[i + 2];
        out[o++] = alphabet[(triple >> 18) & 0x3F];
        out[o++] = alphabet[(triple >> 12) & 0x3F];
        out[o++] = alphabet[(triple >> 6) & 0x3F];
        out[o++] = alphabet[triple & 0x3F];
    }
    if (i < len) {
        uint32_t triple = (uint32_t) in[i] << 16;
        bool two = i + 1 < len;
        if (two) triple |= (uint32_t) in[i + 1] << 8;
        out[o++] = alphabet[(triple >> 18) & 0x3F];
        out[o++] = alphabet[(triple >> 12) & 0x3F];
        out[o++] = two ? alphabet[(triple >> 6) & 0x3F] : '=';
        out[o++] = '=';
    }
    out[o] = '\0';
    return out;
}

/* ------------------------------------------------------------------ */
/* JSON. One string field comes back and one goes out, so this is a scanner   */
/* rather than a parser — the same reduction the Go client makes by decoding  */
/* a struct with a single member in it.                                       */
/* ------------------------------------------------------------------ */

/* Writes s into b as a JSON string, quotes and all. */
static bool json_quote(buf_t * b, const char * s)
{
    if (!buf_str(b, "\"")) return false;
    for (; *s != '\0'; s++) {
        unsigned char c = (unsigned char) *s;
        char escape[8];
        switch (c) {
            case '"':  if (!buf_str(b, "\\\"")) return false; continue;
            case '\\': if (!buf_str(b, "\\\\")) return false; continue;
            case '\n': if (!buf_str(b, "\\n")) return false; continue;
            case '\r': if (!buf_str(b, "\\r")) return false; continue;
            case '\t': if (!buf_str(b, "\\t")) return false; continue;
            default: break;
        }
        if (c < 0x20) {
            snprintf(escape, sizeof(escape), "\\u%04x", c);
            if (!buf_str(b, escape)) return false;
            continue;
        }
        /* Anything else, UTF-8 included, travels as itself. */
        if (!buf_add(b, &c, 1)) return false;
    }
    return buf_str(b, "\"");
}

/* Copies the string value of key out of json. The server writes UTF-8
 * unescaped — FastAPI renders with ensure_ascii off — but \u is handled
 * anyway, because a server that changes its mind about that should not turn
 * an umlaut into a truncated reply. */
static bool json_string(const char * json, const char * key, char * out, size_t cap)
{
    char pattern[64];
    const char * at;
    size_t o = 0;

    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    at = strstr(json, pattern);
    if (at == NULL) return false;

    at += strlen(pattern);
    while (*at == ' ' || *at == '\t' || *at == '\n' || *at == '\r') at++;
    if (*at != ':') return false;
    at++;
    while (*at == ' ' || *at == '\t' || *at == '\n' || *at == '\r') at++;
    if (*at != '"') return false; /* null, a number, anything but a string */
    at++;

    for (; *at != '\0' && *at != '"' && o + 4 < cap; at++) {
        if (*at != '\\') {
            out[o++] = *at;
            continue;
        }
        at++;
        switch (*at) {
            case 'n': out[o++] = '\n'; break;
            case 'r': out[o++] = '\r'; break;
            case 't': out[o++] = '\t'; break;
            case 'b': out[o++] = '\b'; break;
            case 'f': out[o++] = '\f'; break;
            case 'u': {
                /* One code point, written back as UTF-8. Surrogate pairs are
                 * left as the replacement character: this field is a
                 * transcript, and no transcriber emits anything outside the
                 * basic plane. */
                unsigned code = 0;
                int digit;
                for (digit = 0; digit < 4 && at[1] != '\0'; digit++) {
                    char hex = *++at;
                    code <<= 4;
                    if (hex >= '0' && hex <= '9') code |= (unsigned) (hex - '0');
                    else if (hex >= 'a' && hex <= 'f') code |= (unsigned) (hex - 'a' + 10);
                    else if (hex >= 'A' && hex <= 'F') code |= (unsigned) (hex - 'A' + 10);
                    else return false;
                }
                if (code >= 0xD800 && code <= 0xDFFF) code = 0xFFFD;
                if (code < 0x80) {
                    out[o++] = (char) code;
                }
                else if (code < 0x800) {
                    out[o++] = (char) (0xC0 | (code >> 6));
                    out[o++] = (char) (0x80 | (code & 0x3F));
                }
                else {
                    out[o++] = (char) (0xE0 | (code >> 12));
                    out[o++] = (char) (0x80 | ((code >> 6) & 0x3F));
                    out[o++] = (char) (0x80 | (code & 0x3F));
                }
                break;
            }
            case '\0': return false;
            default: out[o++] = *at; break; /* \" and \\ and \/ are themselves */
        }
    }
    out[o] = '\0';
    return true;
}

/* ------------------------------------------------------------------ */
/* HTTP. One connection per request, closed at the end of it: a turn makes    */
/* two requests seconds apart, so a keep-alive pool would be bookkeeping for  */
/* nothing. Plaintext to localhost — no TLS, no redirects, and both services  */
/* answer with a Content-Length.                                              */
/* ------------------------------------------------------------------ */

/* The loop's state, up here because a request in flight has to be able to see
 * that the window was closed. */
static SDL_atomic_t awake;     /* the button: the assistant is up */
static SDL_atomic_t listening; /* published to the UI thread */
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
                      buf_t * out, char * mime, size_t mime_cap)
{
    struct addrinfo hints, * found = NULL;
    struct timeval timeout;
    char head[512];
    buf_t raw = { 0 };
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
        if (!buf_add(&raw, chunk, (size_t) got)) goto done;
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

    ok = buf_add(out, raw.data + header_len, want);

done:
    close(fd);
    buf_free(&raw);
    return ok;
}

/* ------------------------------------------------------------------ */
/* WAV. One header to write, so the recording can be posted as a file, and    */
/* two chunks to find, so the reply can be played at whatever rate it came    */
/* back at.                                                                   */
/* ------------------------------------------------------------------ */

static void put32(unsigned char * at, uint32_t value)
{
    at[0] = (unsigned char) value;
    at[1] = (unsigned char) (value >> 8);
    at[2] = (unsigned char) (value >> 16);
    at[3] = (unsigned char) (value >> 24);
}

static void put16(unsigned char * at, uint16_t value)
{
    at[0] = (unsigned char) value;
    at[1] = (unsigned char) (value >> 8);
}

static uint32_t get32(const unsigned char * at)
{
    return (uint32_t) at[0] | ((uint32_t) at[1] << 8) |
           ((uint32_t) at[2] << 16) | ((uint32_t) at[3] << 24);
}

static uint16_t get16(const unsigned char * at)
{
    return (uint16_t) ((uint32_t) at[0] | ((uint32_t) at[1] << 8));
}

/* The canonical 44-byte header, in front of the samples. */
static bool wav_write(buf_t * out, const int16_t * pcm, size_t samples, int rate)
{
    unsigned char header[44];
    uint32_t data_bytes = (uint32_t) (samples * sizeof(int16_t));

    memcpy(header + 0, "RIFF", 4);
    put32(header + 4, 36 + data_bytes);
    memcpy(header + 8, "WAVEfmt ", 8);
    put32(header + 16, 16);                      /* the fmt chunk's length */
    put16(header + 20, 1);                       /* PCM */
    put16(header + 22, 1);                       /* mono */
    put32(header + 24, (uint32_t) rate);
    put32(header + 28, (uint32_t) rate * 2);     /* bytes per second */
    put16(header + 32, 2);                       /* bytes per frame */
    put16(header + 34, 16);                      /* bits per sample */
    memcpy(header + 36, "data", 4);
    put32(header + 40, data_bytes);

    return buf_add(out, header, sizeof(header)) && buf_add(out, pcm, data_bytes);
}

/* The body of a named chunk, or NULL. It walks the chunks rather than
 * assuming the header is forty-four bytes long, because a writer is free to
 * put a LIST or a fact chunk in front of the samples and several do. */
static const unsigned char * wav_chunk(const unsigned char * file, size_t len,
                                       const char * want, size_t * chunk_len)
{
    size_t at = 12;

    if (len < 12 || memcmp(file, "RIFF", 4) != 0 || memcmp(file + 8, "WAVE", 4) != 0) {
        return NULL;
    }

    while (at + 8 <= len) {
        /* Signed, so the 0xFFFFFFFF a streaming writer leaves behind reads as
         * nonsense here rather than as four gigabytes. */
        int32_t size = (int32_t) get32(file + at + 4);
        size_t body = at + 8;

        if (memcmp(file + at, want, 4) == 0) {
            /* A writer that streamed its output never went back to correct the
             * length it wrote first, so a size that does not fit the file
             * means the rest of the file. */
            if (size <= 0 || body + (size_t) size > len) *chunk_len = len - body;
            else *chunk_len = (size_t) size;
            return file + body;
        }
        if (size <= 0 || body + (size_t) size > len) return NULL;
        at = body + (size_t) size;
        if (size % 2 == 1) at++; /* chunks are padded to an even length */
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* The two services.                                                          */
/* ------------------------------------------------------------------ */

#define VOICE_BOUNDARY "----kaiwatch7f3a1c9e"

/* What was said, or false when nothing was. Silence that tripped the gate is
 * the ordinary outcome of a door closing, not an error. */
static bool transcribe(const int16_t * pcm, size_t samples, char * out, size_t cap)
{
    buf_t body = { 0 }, reply = { 0 };
    bool ok = false;

    if (!buf_str(&body, "--" VOICE_BOUNDARY "\r\n"
                        "Content-Disposition: form-data; name=\"file\"; filename=\"turn.wav\"\r\n"
                        "Content-Type: audio/wav\r\n\r\n")) goto done;
    if (!wav_write(&body, pcm, samples, VOICE_RATE)) goto done;
    if (!buf_str(&body, "\r\n--" VOICE_BOUNDARY "\r\n"
                        "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
                        VOICE_STT_MODEL "\r\n"
                        "--" VOICE_BOUNDARY "--\r\n")) goto done;

    if (!http_post("/v1/audio/transcriptions",
                   "multipart/form-data; boundary=" VOICE_BOUNDARY,
                   body.data, body.len, &reply, NULL, 0)) goto done;

    ok = json_string(reply.data, "text", out, cap);
    if (!ok) SDL_Log("voice: no transcript in the reply");

done:
    buf_free(&body);
    buf_free(&reply);
    return ok;
}

static char * ref_audio;  /* the clip to clone, base64, encoded once */
static char * ref_text;   /* what that clip says */

/* text as audio, in whatever format the server chose to answer with. */
static bool synthesise(const char * text, buf_t * wav)
{
    buf_t body = { 0 };
    char mime[64];
    bool ok = false;

    if (!buf_str(&body, "{\"model\":\"" VOICE_TTS_MODEL "\",\"response_format\":\"wav\""
                        ",\"speed\":1.0,\"language\":\"" VOICE_LANGUAGE "\",\"input\":")) goto done;
    if (!json_quote(&body, text)) goto done;
    /* The clip and its words travel together or not at all: the server aligns
     * one against the other and rejects the audio on its own. */
    if (!buf_str(&body, ",\"ref_audio\":\"")) goto done;
    if (!buf_str(&body, ref_audio)) goto done;
    if (!buf_str(&body, "\",\"ref_text\":")) goto done;
    if (!json_quote(&body, ref_text)) goto done;
    if (!buf_str(&body, "}")) goto done;

    ok = http_post("/v1/audio/speech", "application/json",
                   body.data, body.len, wav, mime, sizeof(mime));
    if (ok && strncmp(mime, "audio/", 6) != 0) {
        SDL_Log("voice: the synthesiser answered %s, not audio", mime);
        ok = false;
    }

done:
    buf_free(&body);
    return ok;
}

/* ------------------------------------------------------------------ */
/* The loop.                                                                  */
/* ------------------------------------------------------------------ */

static SDL_AudioDeviceID microphone;
static SDL_AudioDeviceID speaker;
static SDL_AudioSpec     speaker_spec;

static SDL_Thread * worker;
static int16_t *    pcm;   /* one turn's recording */
static bool         ready; /* a clip, a microphone and a speaker are all there */

/* Loudness of one block, which is the whole of the endpoint detection: speech
 * is orders of magnitude above a quiet room, so nothing subtler is needed to
 * tell that a sentence has finished. */
static int block_rms(const int16_t * block, size_t samples)
{
    double sum = 0;
    size_t i;

    if (samples == 0) return 0;
    for (i = 0; i < samples; i++) sum += (double) block[i] * block[i];
    return (int) SDL_sqrt(sum / (double) samples);
}

/* Records until the speaker stops, and returns how many samples that was.
 * Zero means the turn was abandoned: the button was pressed, or nothing but
 * room noise arrived. */
static size_t record(void)
{
    size_t samples = 0;
    uint32_t quiet_ms = 0;
    bool heard = false;

    SDL_ClearQueuedAudio(microphone); /* whatever accumulated while idle */
    SDL_PauseAudioDevice(microphone, 0);
    SDL_AtomicSet(&listening, 1);

    while (SDL_AtomicGet(&awake) && SDL_AtomicGet(&running)) {
        uint32_t got;
        size_t block;
        int rms;

        SDL_Delay(VOICE_BLOCK_MS);

        got = SDL_DequeueAudio(microphone, pcm + samples,
                               (uint32_t) ((VOICE_MAX_SAMPLES - samples) * sizeof(int16_t)));
        block = got / sizeof(int16_t);
        if (block == 0) continue;

        rms = block_rms(pcm + samples, block);
        samples += block;

        if (rms >= VOICE_SILENCE_RMS) {
            heard = true;
            quiet_ms = 0;
        }
        else if (heard) {
            quiet_ms += (uint32_t) (block * 1000 / VOICE_RATE);
            if (quiet_ms >= VOICE_SILENCE_MS) break;
        }
        else if (samples > VOICE_RATE * 2) {
            /* Two seconds of nothing at all. Start over rather than let a
             * silent room fill the buffer. */
            samples = 0;
        }

        if (samples >= VOICE_MAX_SAMPLES) break;
    }

    SDL_PauseAudioDevice(microphone, 1);
    SDL_AtomicSet(&listening, 0);

    if (!heard) return 0;
    if (samples * 1000 / VOICE_RATE < VOICE_MIN_SPEECH_MS) return 0; /* a cough */
    return samples;
}

/* Plays a WAV, opening the speaker to match it. Returns when the last sample
 * has gone out, or at once when the assistant is sent away mid-sentence —
 * which is what makes Quit interrupt a reply. */
static void play(const unsigned char * file, size_t len)
{
    size_t fmt_len, data_len;
    const unsigned char * fmt = wav_chunk(file, len, "fmt ", &fmt_len);
    const unsigned char * data = wav_chunk(file, len, "data", &data_len);
    SDL_AudioSpec want;

    if (fmt == NULL || fmt_len < 16 || data == NULL || data_len == 0) {
        SDL_Log("voice: the reply is not a WAV this can play");
        return;
    }

    memset(&want, 0, sizeof(want));
    want.channels = (Uint8) get16(fmt + 2);
    want.freq = (int) get32(fmt + 4);
    want.format = get16(fmt + 14) == 8 ? AUDIO_U8 : AUDIO_S16LSB;
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
    if (SDL_QueueAudio(speaker, data, (Uint32) data_len) == 0) {
        SDL_PauseAudioDevice(speaker, 0);
        while (SDL_GetQueuedAudioSize(speaker) > 0 && SDL_AtomicGet(&running)) {
            if (!SDL_AtomicGet(&awake)) { /* Quit, mid-sentence */
                SDL_ClearQueuedAudio(speaker);
                break;
            }
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

static int loop(void * unused)
{
    (void) unused;

    while (SDL_AtomicGet(&running)) {
        char heard[VOICE_MAX_TEXT];
        buf_t wav = { 0 };
        size_t samples;

        if (!SDL_AtomicGet(&awake)) {
            SDL_Delay(50);
            continue;
        }

        samples = record();
        if (samples == 0) continue;

        if (!transcribe(pcm, samples, heard, sizeof(heard))) continue;
        if (heard[0] == '\0') continue; /* the transcriber heard no words */
        SDL_Log("voice: heard \"%s\"", heard);

        if (!SDL_AtomicGet(&awake)) continue;

        if (synthesise(reply(heard), &wav)) {
            play((const unsigned char *) wav.data, wav.len);
        }
        buf_free(&wav);
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

    ref_audio = base64(clip, clip_len);
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
    ready = true;
    SDL_Log("voice: listening through %s, speaking as %s",
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
    free(ref_audio);
    free(ref_text);
    pcm = NULL;
    ref_audio = ref_text = NULL;
    ready = false;
}

void voice_listen(bool on)
{
    if (!ready) return;
    SDL_AtomicSet(&awake, on ? 1 : 0);
}

bool voice_listening(void)
{
    return SDL_AtomicGet(&listening) != 0;
}

bool voice_speaking(void)
{
    return SDL_AtomicGet(&speaking) != 0;
}
