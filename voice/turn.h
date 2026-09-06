/**
 * @file turn.h
 * The portable half of the voice loop: everything about a turn that is not a
 * device.
 *
 * `ui/` is the portable half of the face; this is the portable half of the
 * ear. The rule is the same and one notch stricter — `ui/` may call LVGL,
 * this may call nothing but the C standard library. No SDL, no ESP-IDF, no
 * sockets, no logging: it computes, and hands the answer back.
 *
 * What is left over is what genuinely differs between a Mac and a watch: the
 * audio device, the HTTP transport, and the thread the loop runs on. Those
 * live in host/voice.c and firmware/main/voice.c, which are the two platform
 * halves of this file and are deliberately the same shape.
 *
 * Everything here is shared because everything here could otherwise drift,
 * and a watch that answers to a different name than the simulator does is
 * exactly what the simulator exists to prevent.
 */

#ifndef TURN_H
#define TURN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Where the two models are. The host and the device speak to the same oMLX
 * server, so the model names and the language are one decision, not two. */
#define VOICE_STT_MODEL "parakeet-tdt-0.6b-v3"
#define VOICE_TTS_MODEL "chatterbox-multilingual-v3"
#define VOICE_LANGUAGE  "de"

/* 16 kHz mono is what speech recognition wants. Nothing on either side
 * resamples, so a device that will not open at this rate is refused rather
 * than transcribed at the wrong speed. */
#define VOICE_RATE     16000
#define VOICE_BLOCK_MS 20

/* Where a turn ends. The threshold was measured rather than guessed: a quiet
 * room reads a mean RMS of 42 and peaks at 82, and speech runs in the
 * thousands, so 500 is clear of the noise floor and well under the quietest
 * word. Raise it in a louder room. */
#define VOICE_SILENCE_RMS   500
#define VOICE_SILENCE_MS    800
#define VOICE_MIN_SPEECH_MS 300
#define VOICE_MAX_TURN_MS   20000

/* How long the assistant waits to be spoken to before going back to the
 * clock. With nothing to press, this is the way out that needs nothing said
 * at all. VOICE_WAIT_FOREVER is the other case, asleep, where there is
 * nothing to time out of. */
#define VOICE_IDLE_MS      30000
#define VOICE_WAIT_FOREVER 0

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
} voice_buf_t;

/** Where a buffer gets its memory. NULL is realloc/free, which is the host. */
typedef struct {
    void * (*grow)(void * block, size_t bytes);
    void   (*release)(void * block);
} voice_alloc_t;

/**
 * Point a buffer at an allocator other than the C library's. The device does
 * this once, at start-up: a turn's recording is 640 kB and its reply another
 * 150 kB, which is more than the ESP32-S3 has of internal SRAM, so both have
 * to come out of PSRAM.
 */
void voice_buf_alloc(const voice_alloc_t * alloc);

bool voice_buf_add(voice_buf_t * buf, const void * bytes, size_t n);
bool voice_buf_str(voice_buf_t * buf, const char * s);
void voice_buf_free(voice_buf_t * buf);

/** Release anything this file handed back — voice_base64()'s string. */
void voice_free(void * block);

/* ------------------------------------------------------------------ */
/* The gate: where a turn begins and ends.                                    */
/* ------------------------------------------------------------------ */

typedef enum {
    VOICE_MORE,    /**< still recording */
    VOICE_DONE,    /**< a sentence finished */
    VOICE_NOTHING, /**< the patience ran out with nobody speaking */
} voice_gate_t;

/** How far through one turn's recording the gate is. Owned by the caller. */
typedef struct {
    uint32_t patience_ms;
    uint32_t quiet_ms;
    uint32_t waited_ms;
    size_t   samples;
    bool     heard;
} voice_turn_t;

/** Begin a turn. `patience_ms` is the wait for the first word, or FOREVER. */
void voice_turn_start(voice_turn_t * turn, uint32_t patience_ms);

/**
 * One block period went by. Call this once per VOICE_BLOCK_MS, before asking
 * the device for audio, so that a device which has stopped delivering still
 * hands the deadline back.
 */
voice_gate_t voice_turn_wait(voice_turn_t * turn);

/**
 * Take one block of audio, which the caller has already written at
 * `voice_turn_at(turn)`. Returns VOICE_DONE when the sentence has ended.
 */
voice_gate_t voice_turn_feed(voice_turn_t * turn, const int16_t * block, size_t n);

/** Where the next block goes, as an offset into the caller's buffer. */
size_t voice_turn_at(const voice_turn_t * turn);

/** How much of the buffer is worth sending. Zero means nothing was said. */
size_t voice_turn_samples(const voice_turn_t * turn);

/* ------------------------------------------------------------------ */
/* The name, and the goodbye.                                                 */
/* ------------------------------------------------------------------ */

/**
 * What was said after the watch's name, or NULL if its name is not in there.
 * An empty string means the name and nothing else. The returned pointer is
 * into `heard`.
 */
const char * voice_after_wake(const char * heard);

/** True when the whole transcript is one of the goodbyes. */
bool voice_is_goodbye(const char * heard);

/* ------------------------------------------------------------------ */
/* The wire.                                                                  */
/* ------------------------------------------------------------------ */

/** The multipart boundary the transcription request is cut on. */
#define VOICE_BOUNDARY "----kaiwatch7f3a1c9e"

#define VOICE_STT_PATH "/v1/audio/transcriptions"
#define VOICE_TTS_PATH "/v1/audio/speech"
#define VOICE_STT_TYPE "multipart/form-data; boundary=" VOICE_BOUNDARY
#define VOICE_TTS_TYPE "application/json"

/** The whole body of a transcription request: one WAV in a multipart form. */
bool voice_stt_body(voice_buf_t * body, const int16_t * pcm, size_t samples);

/**
 * The whole body of a synthesis request. `ref_audio` is the clip to clone,
 * already base64; `ref_text` is what that clip says. The two travel together
 * or not at all — the server aligns one against the other and rejects the
 * audio on its own.
 */
bool voice_tts_body(voice_buf_t * body, const char * text,
                    const char * ref_audio, const char * ref_text);

/** The transcript out of a transcription reply. */
bool voice_stt_text(const char * json, char * out, size_t cap);

/** Base64, for the clip. Encoded once: it is the same bytes on every request. */
char * voice_base64(const unsigned char * in, size_t len);

/* ------------------------------------------------------------------ */
/* WAV, so the reply can be played at whatever rate it came back at.          */
/* ------------------------------------------------------------------ */

typedef struct {
    int rate;
    int channels;
    int bits;
    const unsigned char * pcm;
    size_t bytes;
} voice_wav_t;

/** Reads a WAV's format and finds its samples. False if it is not one. */
bool voice_wav_open(const unsigned char * file, size_t len, voice_wav_t * wav);

#endif /* TURN_H */
