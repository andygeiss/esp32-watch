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

/* The three models and the language the watch uses unless the platform says
 * otherwise. They were one decision twice until the watch started speaking to
 * a server it does not run: a model host serves several models and swaps
 * them, so which one answers is an operational fact and belongs beside the
 * address rather than in here. These stay as the defaults, because a default
 * written down once is still worth having.
 *
 * The brain has a second spelling as well as a default. VOICE_BRAIN_ECHO is
 * the word that turns the model off: the answer becomes the question said
 * back, which is the shortest path through the whole pipeline — a turn that
 * breaks there breaks everywhere, it needs no third service, and what comes
 * out of the speaker is exactly what the transcriber heard. It is a word
 * rather than an empty string because empty already means "the default", and
 * the default is a model now. `BRAIN=echo` is what the Go orchestrator calls
 * the same mode. */
#define VOICE_STT_MODEL   "parakeet-tdt-0.6b-v3"
#define VOICE_TTS_MODEL   "chatterbox-multilingual-v3"
#define VOICE_BRAIN_MODEL "Qwen3.8-27B-oQ4e-mtp"
#define VOICE_BRAIN_ECHO  "echo"
#define VOICE_LANGUAGE    "de"

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

/* How long nothing but exact zeros has to go on before it is a broken device
 * rather than a quiet moment. The same measurement says it: a room reads an
 * RMS in the tens and never reads zero, so a stream of zeros is a microphone
 * that opened and is not hearing. Long enough to be sure, short enough to be
 * told at start-up. */
#define VOICE_DEAD_MS 3000

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
    uint32_t dead_ms;
    size_t   samples;
    bool     heard;
    bool     dead_told;
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

/**
 * True the one time a turn can be sure the microphone is not working: nothing
 * but samples of exactly zero for VOICE_DEAD_MS. It is counted in wall time,
 * off voice_turn_wait(), so a device handing back no blocks at all counts the
 * same as one handing back silent ones.
 *
 * This is the difference between a quiet room and a dead device, which the
 * rest of the gate cannot tell apart — both simply never end a turn, and the
 * watch waits for a wake phrase that can never arrive.
 *
 * Call it once per block, beside voice_turn_wait(). It answers true once per
 * turn, so the platform logs a line rather than fifty a second; asleep there
 * is only ever the one turn, so that is once. What to say about it is the
 * platform's, because so is the device — this file does no logging.
 */
bool voice_turn_dead(voice_turn_t * turn);

/** Where the next block goes, as an offset into the caller's buffer. */
size_t voice_turn_at(const voice_turn_t * turn);

/** How much of the buffer is worth sending. Zero means nothing was said. */
size_t voice_turn_samples(const voice_turn_t * turn);

/* ------------------------------------------------------------------ */
/* The name, and the goodbye.                                                 */
/* ------------------------------------------------------------------ */

/* How much of a wake phrase list there is room for. Eight spellings is more
 * than the transcriber has ever produced for one name, and the bytes are a
 * static buffer because turn.c is not the file that owns an allocator. */
#define VOICE_WAKE_MAX   8
#define VOICE_WAKE_BYTES 256

/* What the assistant calls itself. `Kai` unless the platform says otherwise.
 *
 * This is a second setting beside the wake phrase and not derived from it,
 * which is the whole point. The wake list is what the *transcriber* writes
 * down for a name it has never been shown — the six built-in spellings
 * include "hey ky" and "hey chai" — so a name taken from it would have the
 * watch introducing itself as Chai. What it is called and how it is misheard
 * are two different facts, and only one of them is spoken aloud. */
#define VOICE_NAME       "Kai"
#define VOICE_NAME_BYTES 32

/**
 * Name the assistant. NULL or empty means VOICE_NAME. False when it did not
 * fit, and the default stays in force. It reaches the brain through the
 * system prompt and nowhere else — the wake phrase is what the microphone
 * listens for, and it is set separately.
 */
bool voice_name_set(const char * name);

/** What the assistant calls itself. Each platform logs it at start-up. */
const char * voice_name(void);

/**
 * What the watch answers to: a '|'-separated list of spellings, because the
 * transcriber has never been shown the name and does not write it down the
 * same way twice. Case and punctuation are taken off here, so "Hey Kai!" and
 * "hey kai" are the same phrase and whoever configures one need not know
 * which.
 *
 * NULL or an empty string means the built-in list, which is the only place
 * those spellings are written down — a configuration that wanted the default
 * would otherwise have to be a second copy of it.
 *
 * False means the list did not fit, or one of its phrases was empty; the
 * built-in list stays in force, and the platform has something to log. The
 * platform is where this comes from: a #define on the host, Kconfig on the
 * device, the same way the server address does.
 */
bool voice_wake_set(const char * phrases);

/**
 * What the watch is actually listening for, configured or default. Each
 * platform logs this at start-up, and it is the only way to tell a phrase
 * that was taken from one that did not fit and was quietly replaced by the
 * built-in list — the failure and the default look identical otherwise.
 *
 * The pointers are into this file's own storage; the next voice_wake_set()
 * overwrites what they point at.
 */
size_t       voice_wake_count(void);
const char * voice_wake_at(size_t i);

/**
 * What was said after the watch's name, or NULL if its name is not in there.
 * An empty string means the name and nothing else. The returned pointer is
 * into `heard`.
 */
const char * voice_after_wake(const char * heard);

/** True when the whole transcript is one of the goodbyes. */
bool voice_is_goodbye(const char * heard);

/* ------------------------------------------------------------------ */
/* Where the server is, and which models answer.                              */
/* ------------------------------------------------------------------ */

/* Room for one address and one model name. A host name is bounded by DNS at
 * 253 bytes and nothing here is close; the bytes are static buffers because
 * turn.c is not the file that owns an allocator — the same reason the wake
 * phrases are. */
#define VOICE_HOST_BYTES  128
#define VOICE_PORT_BYTES  8
#define VOICE_MODEL_BYTES 64
#define VOICE_KEY_BYTES   256

/** An address, taken apart far enough for either platform to dial it. */
typedef struct {
    bool tls;                      /**< https, so the transport needs one */
    char host[VOICE_HOST_BYTES];
    char port[VOICE_PORT_BYTES];   /**< always written out, 443 or 80 by default */
} voice_url_t;

/**
 * Splits `https://host[:port][/path]` into something to connect to. A bare
 * `host[:port]` is plaintext, which is what a server on the same desk is.
 * The path is ignored: every request here names its own, and a base URL that
 * carried one would have to be pasted onto them.
 *
 * False when there is nothing to dial — an empty string, or a scheme this
 * cannot speak. That is the platform's cue to stay a clock, not an error to
 * report: an unconfigured address is the ordinary state of a fresh clone.
 */
bool voice_url_parse(const char * url, voice_url_t * out);

/** Which models answer. A NULL or empty field means the default beside it. */
typedef struct {
    const char * stt;
    const char * brain;
    const char * tts;
    const char * language;
} voice_models_t;

/**
 * Name the models to ask. NULL means all four defaults, which is what a
 * platform that has nothing to say says — the defaults stay written down
 * exactly once, the same bargain voice_wake_set() makes.
 *
 * `brain` takes VOICE_BRAIN_ECHO as well as a model name, and is left empty
 * when it does; voice_models_get() reports it that way, so every caller asks
 * one question and the word is understood in exactly one place.
 *
 * False when a name did not fit, and the defaults stay in force for the ones
 * that did not; the platform has something to log and a turn still works.
 */
bool voice_models_set(const voice_models_t * models);

/**
 * The four names actually in force, defaults and all. Two jobs, which is why
 * it hands back all of them: the loop asks whether `brain` is empty, because
 * that is what VOICE_BRAIN_ECHO was turned into on the way in and there is
 * then nothing to POST; and each platform logs the set at start-up, which is
 * the first thing worth knowing when a turn comes back empty.
 *
 * The pointers are into this file's own storage and stay valid; the next
 * voice_models_set() overwrites what they point at.
 */
const voice_models_t * voice_models_get(void);

/* ------------------------------------------------------------------ */
/* The wire.                                                                  */
/* ------------------------------------------------------------------ */

/** The multipart boundary the transcription request is cut on. */
#define VOICE_BOUNDARY "----kaiwatch7f3a1c9e"

#define VOICE_STT_PATH   "/v1/audio/transcriptions"
#define VOICE_TTS_PATH   "/v1/audio/speech"
#define VOICE_BRAIN_PATH "/v1/chat/completions"
#define VOICE_STT_TYPE   "multipart/form-data; boundary=" VOICE_BOUNDARY
#define VOICE_TTS_TYPE   "application/json"
#define VOICE_BRAIN_TYPE "application/json"

/* What the brain is told before it hears anything, and how much it may say
 * back. The shape is about a watch rather than about a server, so it is the
 * same decision on both platforms and stays here: the reply leaves through a
 * speaker on someone's wrist, where a paragraph is a minute of talking and a
 * bulleted list is not a thing that can be said at all.
 *
 * The line about digits is the one that was paid for. Chatterbox cannot read
 * a number: given "Es ist 13:44 Uhr." it says something that transcribes back
 * as "Es ist 13,4 Uhr." — the right time, said wrong, which is worse than no
 * answer because it sounds like one. Told to write words instead, the same
 * model returns "Es ist dreizehn Uhr vierundvierzig.", and that transcribes
 * back as 13:44. So the digits are spelled out here rather than in the tools:
 * every language spells its own numbers and the brain already knows which one
 * it is answering in, where turn.c would need a speller per language to do
 * the same job worse. It is not only the clock either — any answer with a
 * number in it went out mangled, and the tools are simply what made that
 * happen every single time.
 *
 * The name in it is the one part that is configuration, because it is the
 * answer to "who are you?" and that is a thing an owner gets to decide.
 *
 * BYTES went from 512 to 640 when that line went in: the prompt is 420 bytes
 * with the longest name that fits, and a snprintf that runs out here does not
 * fail, it truncates — which would cut a rule off mid-sentence and leave the
 * brain following most of one. */
#define VOICE_BRAIN_MAX_TOKENS 256
#define VOICE_SYSTEM_BYTES     640
#define VOICE_SYSTEM_FMT                                                      \
    "You are %s, the voice of a wristwatch. Answer in the language the "      \
    "question was asked in. Keep it to one or two short sentences: every "    \
    "word is read aloud through a small speaker. Write numbers, times and "   \
    "dates as words and never as digits, because the voice that reads your "  \
    "answer aloud cannot say digits. No markdown, no lists, no emoji and no " \
    "stage directions — only what should be said."

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

/* ------------------------------------------------------------------ */
/* Tools: the questions the watch can answer and the brain cannot.            */
/* ------------------------------------------------------------------ */

/* The brain runs on a server in a rack. It does not know what time it is on
 * this wrist, it does not know which side of the planet the wrist is on, and
 * asked anyway it answers with an hour it made up rather than saying so —
 * which on a watch is the one wrong answer that matters. So the clock is
 * handed over as a tool: the brain asks, this file reads the same time() the
 * face is drawn from, and the brain says it back in the language the question
 * came in.
 *
 * These are the first two and the mechanism is the point. A tool is a name, a
 * sentence telling the brain when to reach for it, and a function that fills
 * in a short string — none of which is a device, so all of it is on this side
 * of the boundary and both builds get the same tools. What the watch knows
 * that a server cannot is the whole of what belongs here: the clock now, the
 * charge and the radio when there is a gauge to read them off. */

/* How many calls one reply may carry, and how many replies a turn asks for
 * before it insists on words.
 *
 * MAX is not 1, and that was measured rather than allowed for: asked "Welcher
 * Wochentag ist heute und wie spät ist es?", Qwen3.8-27B calls get_time and
 * get_date in the same message. A call left without a result of its own is a
 * conversation the server rejects, so handling only the first would break
 * exactly the question that most wants both tools.
 *
 * ROUNDS is what stops a model that has decided to keep asking. Three replies
 * is two rounds of tools and then an answer, which is one more round than
 * anything here needs. */
#define VOICE_TOOL_MAX    4
#define VOICE_TOOL_ROUNDS 3

/* One tool's answer, and one call's id, name and arguments as they come back
 * to be echoed. A date with its weekday is the longest answer there is; ids
 * are 29 characters from OpenAI and 46 from some of the model servers. */
#define VOICE_TOOL_BYTES      64
#define VOICE_TOOL_ID_BYTES   64
#define VOICE_TOOL_NAME_BYTES 32
#define VOICE_TOOL_ARGS_BYTES 96

/** Room for one round's worth of `get_time -> 09:41`, for the platform log. */
#define VOICE_TOOL_LOG_BYTES 160

/**
 * What this watch can be asked to look up, as `get_time, get_date`, for the
 * start-up log. It is written out here rather than handed over a name at a
 * time because otherwise both platform halves would keep the same little loop
 * and the same clamp — the same reason the wake phrases are only ever counted
 * from this side. Returns `out`.
 */
const char * voice_tool_list(char * out, size_t cap);

/**
 * One exchange with the brain — which is more than one request as soon as
 * there are tools. The brain asks for the clock, the clock answers, and the
 * whole conversation goes back with both in it, because a tool result is only
 * an answer to a call the conversation can still see. So the messages
 * accumulate here rather than being rebuilt out of `heard` each time.
 *
 * Zero it, hand it to voice_chat_start(), and give it back to
 * voice_chat_free(); the buffer comes from whichever allocator
 * voice_buf_alloc() was given, so on the device it is PSRAM like the rest.
 */
typedef struct {
    voice_buf_t messages;
    unsigned    rounds;
} voice_chat_t;

/** Open a conversation: the system prompt, then what was heard. */
bool voice_chat_start(voice_chat_t * chat, const char * heard);

/**
 * The whole body of the next chat completion request: the model named by
 * voice_models_set(), the tools above, and the conversation so far.
 *
 * Thinking is turned off in it and that is load-bearing, not a tuning knob. A
 * reasoning model left to think writes its scratchpad into the reply —
 * "Thinking: 1. Analyze the request …" — and every word of it would be read
 * aloud through the watch's speaker before the answer arrived. It is sent
 * both ways because only one of them works here; see the body in turn.c.
 *
 * It does not stream, and that is the other half of the same decision. The
 * chunker that cuts a reply at sentence seams is not translated yet, so there
 * is nothing here that could start speaking early; a stream would only be a
 * second parser for the same text.
 */
bool voice_chat_body(voice_chat_t * chat, voice_buf_t * body);

/**
 * Read one reply back. True when the brain asked for tools instead of
 * answering: the calls have been run, their results are in the conversation,
 * and the caller POSTs voice_chat_body() again. False when the brain
 * answered — read it with voice_brain_text() — and false again when the
 * rounds ran out, which is deliberately the same answer: whatever that last
 * reply had to say is said, and a reply that was only a call has nothing, so
 * the turn ends in the same silence any other broken one makes.
 *
 * That the calls are looked at before the words is load-bearing, and it was
 * measured. A reply can carry both: asked "Welcher Tag ist heute?",
 * Qwen3.8-27B calls get_date and writes "Ich muss das Datum erst von der Uhr
 * ablesen." beside it. Read the words first and the watch says the model
 * clearing its throat, then stops — never having asked the clock, on the one
 * question it was given a clock for.
 *
 * `ran` gets one round's `get_time -> 09:41` for the platform to log, which
 * is the only way to see whether the brain reached for the clock or guessed
 * at it. NULL asks for none.
 */
bool voice_chat_tools(voice_chat_t * chat, const char * json,
                      char * ran, size_t cap);

/** Give the conversation back. Safe on one that was only ever zeroed. */
void voice_chat_free(voice_chat_t * chat);

/**
 * The answer out of a chat completion reply. It reads the message's own
 * `content`, from `"message"` onward rather than from the top of the
 * document: a reasoning model returns `reasoning_content` beside it, and the
 * one that gets spoken must be the one the model meant to say.
 */
bool voice_brain_text(const char * json, char * out, size_t cap);

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
