/**
 * @file turn.c
 * The portable half of the voice loop. See turn.h.
 *
 * The C standard library and nothing else — no SDL, no ESP-IDF, no sockets,
 * no logging. `make check` compiles this file alone and fails if `nm` finds
 * anything undefined outside libc, the same gate ui.c goes through.
 */

#include "turn.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* The buffer.                                                                */
/* ------------------------------------------------------------------ */

static void * default_grow(void * block, size_t bytes)
{
    return realloc(block, bytes);
}

static void default_release(void * block)
{
    free(block);
}

static voice_alloc_t allocator = { default_grow, default_release };

void voice_buf_alloc(const voice_alloc_t * alloc)
{
    allocator = alloc != NULL ? *alloc : (voice_alloc_t) { default_grow, default_release };
}

bool voice_buf_add(voice_buf_t * buf, const void * bytes, size_t n)
{
    if (buf->len + n + 1 > buf->cap) {
        size_t want = buf->cap ? buf->cap : 256;
        char * grown;
        while (want < buf->len + n + 1) want *= 2;
        grown = allocator.grow(buf->data, want);
        if (grown == NULL) return false;
        buf->data = grown;
        buf->cap = want;
    }
    memcpy(buf->data + buf->len, bytes, n);
    buf->len += n;
    buf->data[buf->len] = '\0'; /* so a body can be read as a string when it is one */
    return true;
}

bool voice_buf_str(voice_buf_t * buf, const char * s)
{
    return voice_buf_add(buf, s, strlen(s));
}

void voice_buf_free(voice_buf_t * buf)
{
    allocator.release(buf->data);
    buf->data = NULL;
    buf->len = buf->cap = 0;
}

void voice_free(void * block)
{
    allocator.release(block);
}

/* ------------------------------------------------------------------ */
/* The gate. Loudness is the whole of the endpoint detection: speech is       */
/* orders of magnitude above a quiet room, so nothing subtler is needed to    */
/* tell that a sentence has finished.                                         */
/* ------------------------------------------------------------------ */

/* Integer, and summing into a 64-bit accumulator, so this is the same
 * arithmetic on a Mac and on a chip whose FPU is single precision. A block is
 * 320 samples, so the sum cannot come near overflowing. */
static int block_rms(const int16_t * block, size_t n)
{
    uint64_t sum = 0;
    uint32_t root = 0;
    uint32_t bit;
    size_t i;

    if (n == 0) return 0;
    for (i = 0; i < n; i++) sum += (uint64_t) ((int32_t) block[i] * block[i]);
    sum /= n;

    /* Integer square root, bit by bit. sum is at most 32767^2, so 16 bits. */
    for (bit = 1u << 30; bit != 0; bit >>= 2) {
        if (sum >= (uint64_t) (root + bit)) {
            sum -= root + bit;
            root = (root >> 1) + bit;
        }
        else {
            root >>= 1;
        }
    }
    return (int) root;
}

void voice_turn_start(voice_turn_t * turn, uint32_t patience_ms)
{
    memset(turn, 0, sizeof(*turn));
    turn->patience_ms = patience_ms;
}

voice_gate_t voice_turn_wait(voice_turn_t * turn)
{
    if (turn->heard || turn->patience_ms == VOICE_WAIT_FOREVER) return VOICE_MORE;

    turn->waited_ms += VOICE_BLOCK_MS;
    return turn->waited_ms >= turn->patience_ms ? VOICE_NOTHING : VOICE_MORE;
}

voice_gate_t voice_turn_feed(voice_turn_t * turn, const int16_t * block, size_t n)
{
    int rms;

    if (n == 0) return VOICE_MORE;

    rms = block_rms(block, n);
    turn->samples += n;

    if (rms >= VOICE_SILENCE_RMS) {
        turn->heard = true;
        turn->quiet_ms = 0;
    }
    else if (turn->heard) {
        turn->quiet_ms += (uint32_t) (n * 1000 / VOICE_RATE);
        if (turn->quiet_ms >= VOICE_SILENCE_MS) return VOICE_DONE;
    }
    else if (turn->samples > VOICE_RATE * 2) {
        /* Two seconds of nothing at all. Start over rather than let a silent
         * room fill the buffer. */
        turn->samples = 0;
    }

    return turn->samples >= VOICE_MAX_SAMPLES ? VOICE_DONE : VOICE_MORE;
}

size_t voice_turn_at(const voice_turn_t * turn)
{
    return turn->samples;
}

size_t voice_turn_samples(const voice_turn_t * turn)
{
    if (!turn->heard) return 0;
    if (turn->samples * 1000 / VOICE_RATE < VOICE_MIN_SPEECH_MS) return 0; /* a cough */
    return turn->samples;
}

/* ------------------------------------------------------------------ */
/* The name, and the goodbye. There is nothing to press, so both of them are  */
/* words, and words arrive here only as a transcript.                         */
/* ------------------------------------------------------------------ */

/* KAI is a name the transcriber has never been shown, so it writes down
 * whichever German word sounded closest, and not the same one every time.
 * More than one spelling is the point of the table; add to it from what turns
 * up in the log rather than guessing at the phonetics. This is the default:
 * voice_wake_set() below puts whatever the platform was configured with in
 * front of it, and an empty configuration means this list rather than a
 * second copy of it.
 *
 * It is the greeting that is matched, not the name on its own: Kaiser,
 * Kaimauer and a good many other ordinary German words start the same way,
 * and a watch that wakes up on the news is worse than one that misses a call.
 */
static const char * const WAKE[] = {
    "hey kai", "hey kay", "hey ky", "hey chai", "hei kai", "hi kai",
};

/* Said on its own, this ends the session. Whole transcripts rather than
 * substrings: "stopp mal die Musik" is something to answer, not an
 * instruction to go away. tschüss/tschüs and stop/stopp are one word each:
 * the transcriber picks a spelling and there is no telling which. */
static const char * const GOODBYE[] = {
    "tschüss", "tschüs", "quit", "stop", "stopp",
};

#define VOICE_COUNT(a) (sizeof(a) / sizeof((a)[0]))

/* Lower-cases byte for byte, so every offset into the copy is an offset into
 * the original. Only ASCII moves: the ü of tschüss is two bytes above 127 and
 * comes through untouched, which is what the table above is written for. */
static void lower(const char * in, char * out, size_t cap)
{
    size_t i;

    for (i = 0; i + 1 < cap && in[i] != '\0'; i++) {
        out[i] = (char) tolower((unsigned char) in[i]);
    }
    out[i] = '\0';
}

/* The configured spellings, cleaned, laid end to end in one static buffer.
 * Empty means WAKE above: the default is written down once, and a platform
 * that wants it says nothing rather than repeating it. */
static char   wake_buf[VOICE_WAKE_BYTES];
static char * wake_set[VOICE_WAKE_MAX];
static size_t wake_n;

static size_t wake_count(void)
{
    return wake_n != 0 ? wake_n : VOICE_COUNT(WAKE);
}

static const char * wake_at(size_t i)
{
    return wake_n != 0 ? wake_set[i] : WAKE[i];
}

/* Letters, digits and single spaces survive; ASCII punctuation does not, so a
 * phrase typed as someone would say it matches a transcript. Bytes above 127
 * come through untouched — the ü of a German phrase is two of them, the same
 * reason lower() leaves them alone. */
static bool wake_keep(unsigned char c)
{
    return c >= 0x80 || c == ' ' || (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/* One phrase into wake_buf at *at, terminated, with the spaces at either end
 * and any run in the middle collapsed. False if it does not fit or is empty. */
static bool wake_clean(const char * in, size_t len, size_t * at)
{
    size_t start = *at;
    size_t i;

    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char) in[i];

        if (!wake_keep(c)) continue;
        /* No space in front, and never two in a row. */
        if (c == ' ' && (*at == start || wake_buf[*at - 1] == ' ')) continue;
        if (*at + 1 >= sizeof(wake_buf)) return false;
        wake_buf[(*at)++] = (char) tolower(c);
    }
    while (*at > start && wake_buf[*at - 1] == ' ') (*at)--; /* nor behind */

    if (*at == start) return false; /* an empty phrase wakes on every word */
    wake_buf[(*at)++] = '\0';
    return true;
}

bool voice_wake_set(const char * phrases)
{
    size_t at = 0;
    size_t n = 0;

    wake_n = 0; /* the built-in list, unless a whole one is read below */
    if (phrases == NULL) return true;

    while (*phrases != '\0') {
        size_t len = 0;
        size_t start = at;
        bool more;

        /* Split by hand rather than with strchr: this file's undefined
         * symbols are a checked list, and one more would be one more. */
        while (phrases[len] != '\0' && phrases[len] != '|') len++;
        more = phrases[len] == '|';

        if (n == VOICE_WAKE_MAX) return false;
        if (!wake_clean(phrases, len, &at)) return false;

        wake_set[n++] = wake_buf + start;
        phrases += more ? len + 1 : len;
    }

    wake_n = n;
    return true;
}

const char * voice_after_wake(const char * heard)
{
    char lowered[VOICE_MAX_TEXT];
    const char * rest = NULL;
    size_t i;

    lower(heard, lowered, sizeof(lowered));

    for (i = 0; i < wake_count(); i++) {
        const char * phrase = wake_at(i);
        const char * at = strstr(lowered, phrase);
        if (at == NULL) continue;
        at += strlen(phrase);
        if (rest == NULL || at < rest) rest = at;
    }
    if (rest == NULL) return NULL;

    rest = heard + (rest - lowered); /* lower() kept the offsets */
    while (*rest == ' ' || *rest == ',' || *rest == '.' ||
           *rest == '!' || *rest == '?') {
        rest++;
    }
    return rest;
}

bool voice_is_goodbye(const char * heard)
{
    char lowered[VOICE_MAX_TEXT];
    const char * word = lowered;
    size_t len, i;

    lower(heard, lowered, sizeof(lowered));

    while (*word == ' ') word++;
    for (len = strlen(word); len > 0; len--) {
        char c = word[len - 1];
        if (c != ' ' && c != ',' && c != '.' && c != '!' && c != '?') break;
    }

    for (i = 0; i < VOICE_COUNT(GOODBYE); i++) {
        if (strlen(GOODBYE[i]) == len && strncmp(word, GOODBYE[i], len) == 0) return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Base64, for the clip the synthesiser clones.                               */
/* ------------------------------------------------------------------ */

char * voice_base64(const unsigned char * in, size_t len)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char * out = allocator.grow(NULL, 4 * ((len + 2) / 3) + 1);
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

/* Writes s into buf as a JSON string, quotes and all. */
static bool json_quote(voice_buf_t * buf, const char * s)
{
    if (!voice_buf_str(buf, "\"")) return false;
    for (; *s != '\0'; s++) {
        unsigned char c = (unsigned char) *s;
        char escape[8];
        switch (c) {
            case '"':  if (!voice_buf_str(buf, "\\\"")) return false; continue;
            case '\\': if (!voice_buf_str(buf, "\\\\")) return false; continue;
            case '\n': if (!voice_buf_str(buf, "\\n")) return false; continue;
            case '\r': if (!voice_buf_str(buf, "\\r")) return false; continue;
            case '\t': if (!voice_buf_str(buf, "\\t")) return false; continue;
            default: break;
        }
        if (c < 0x20) {
            snprintf(escape, sizeof(escape), "\\u%04x", c);
            if (!voice_buf_str(buf, escape)) return false;
            continue;
        }
        /* Anything else, UTF-8 included, travels as itself. */
        if (!voice_buf_add(buf, &c, 1)) return false;
    }
    return voice_buf_str(buf, "\"");
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

bool voice_stt_text(const char * json, char * out, size_t cap)
{
    return json_string(json, "text", out, cap);
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
static bool wav_write(voice_buf_t * out, const int16_t * pcm, size_t samples, int rate)
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

    return voice_buf_add(out, header, sizeof(header)) &&
           voice_buf_add(out, pcm, data_bytes);
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

bool voice_wav_open(const unsigned char * file, size_t len, voice_wav_t * wav)
{
    size_t fmt_len = 0, data_len = 0;
    const unsigned char * fmt = wav_chunk(file, len, "fmt ", &fmt_len);
    const unsigned char * data = wav_chunk(file, len, "data", &data_len);

    if (fmt == NULL || fmt_len < 16 || data == NULL || data_len == 0) return false;

    wav->channels = (int) get16(fmt + 2);
    wav->rate = (int) get32(fmt + 4);
    wav->bits = (int) get16(fmt + 14);
    wav->pcm = data;
    wav->bytes = data_len;
    return true;
}

/* ------------------------------------------------------------------ */
/* The two request bodies. Built here rather than in either platform, so the  */
/* Mac and the watch cannot end up asking the same server two different       */
/* questions.                                                                 */
/* ------------------------------------------------------------------ */

bool voice_stt_body(voice_buf_t * body, const int16_t * pcm, size_t samples)
{
    return voice_buf_str(body,
               "--" VOICE_BOUNDARY "\r\n"
               "Content-Disposition: form-data; name=\"file\"; filename=\"turn.wav\"\r\n"
               "Content-Type: audio/wav\r\n\r\n") &&
           wav_write(body, pcm, samples, VOICE_RATE) &&
           voice_buf_str(body,
               "\r\n--" VOICE_BOUNDARY "\r\n"
               "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
               VOICE_STT_MODEL "\r\n"
               "--" VOICE_BOUNDARY "--\r\n");
}

bool voice_tts_body(voice_buf_t * body, const char * text,
                    const char * ref_audio, const char * ref_text)
{
    return voice_buf_str(body,
               "{\"model\":\"" VOICE_TTS_MODEL "\",\"response_format\":\"wav\""
               ",\"speed\":1.0,\"language\":\"" VOICE_LANGUAGE "\",\"input\":") &&
           json_quote(body, text) &&
           voice_buf_str(body, ",\"ref_audio\":\"") &&
           voice_buf_str(body, ref_audio) &&
           voice_buf_str(body, "\",\"ref_text\":") &&
           json_quote(body, ref_text) &&
           voice_buf_str(body, "}");
}
