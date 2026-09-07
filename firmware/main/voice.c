/**
 * @file voice.c
 * The device's half of the voice loop: the board's codecs, esp_http_client,
 * and a FreeRTOS task to run them on.
 *
 * The counterpart of host/voice.c, and deliberately the same shape — the same
 * five functions out of voice.h, the same record/transcribe/answer/play pass,
 * the same three booleans published to the loop that draws. Everything that
 * is not a device or a socket is in voice/turn.c, which both halves compile
 * unchanged: the words the watch answers to, the gate that ends a turn, and
 * the two request bodies. That is the point of that file. A watch that woke
 * on a different word than the simulator does would make the simulator
 * worthless, and there is no way to get there from here.
 *
 * Four things are worth knowing:
 *
 * - **It runs in a task of its own.** A turn costs seconds and LVGL is single
 *   threaded, so this cannot live in app_main's loop. It publishes three
 *   booleans through atomics; main.c reads them in its timers and hands two
 *   to ui_status_set() and one to ui_view_set(). Nothing else crosses, which
 *   is why there is still no lock anywhere near LVGL.
 *
 * - **Everything big lives in PSRAM.** A turn's recording is 640 kB and a
 *   reply another 150 kB or so, and the ESP32-S3 has 512 kB of internal SRAM
 *   in total. voice_buf_alloc() points turn.c's buffers at the same place.
 *
 * - **Three things have to be true before it says a word**: an SSID, so there
 *   is a network; a server, so there is somewhere to ask; and the reference
 *   clip compiled in, so the synthesiser has a voice to borrow. Any of them
 *   missing and the watch is a clock and nothing else — the same answer the
 *   simulator gives when voices/kai.opus is not there.
 *
 * - **The answer comes from a chat model when one is configured**, and is the
 *   question said back when none is. The echo needs no third service and is
 *   the shortest path through the pipeline; both live behind reply(), exactly
 *   as they do on the host.
 *
 * - **The microphone is open the whole time**, so every utterance in the room
 *   goes to the transcriber. That is what the host does because a Mac has no
 *   wake-word engine; here it is a choice, and the one worth revisiting
 *   first. ESP-SR would hear the name locally and open a connection only
 *   then — but its models do not include this watch's name, so that is a
 *   trained model away rather than a flag away.
 */

#include "voice.h"

#include <stdatomic.h>
#include <string.h>

#include "board.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net.h"
#include "sdkconfig.h"
#include "turn.h"

static const char * TAG = "voice";

/* The task. A turn's recording and its bodies are all in PSRAM, so what is
 * left for the stack is esp_http_client and the TLS handshake under it, which
 * want several kilobytes between them.
 *
 * The two transcripts are not on it. They are VOICE_MAX_TEXT each, which is
 * the whole of the old 8 kB before a single call was made — the task would
 * have smashed its stack on its first turn, and this firmware has never run
 * on hardware, so nothing had said so yet. They are static below instead:
 * there is exactly one voice task, so there is exactly one of each. */
#define VOICE_TASK_STACK 12288
#define VOICE_TASK_PRIO  4

/* Long enough for the synthesiser, which takes about as long as the speech it
 * produces. The transcriber answers in about a second. */
#define VOICE_HTTP_TIMEOUT_MS 120000

/* One block of microphone audio, the unit the gate in turn.c works in. */
#define VOICE_BLOCK_SAMPLES ((size_t) VOICE_RATE * VOICE_BLOCK_MS / 1000)

/* The clip KAI borrows its voice from, linked in by main/CMakeLists.txt when
 * voices/kai.opus is in the tree. It is gitignored — a recording of a person
 * in a licensed repository — so a fresh clone builds without it and the watch
 * simply never speaks. */
#if CONFIG_WATCH_HAS_VOICE
extern const uint8_t kai_opus_start[] asm("_binary_kai_opus_start");
extern const uint8_t kai_opus_end[]   asm("_binary_kai_opus_end");
extern const uint8_t kai_txt_start[]  asm("_binary_kai_txt_start");
extern const uint8_t kai_txt_end[]    asm("_binary_kai_txt_end");
#endif

static atomic_bool awake;     /* the name has been heard, the goodbye has not */
static atomic_bool recording; /* the microphone is being read */
static atomic_bool speaking;
static atomic_bool running;

static TaskHandle_t worker;
static int16_t *    pcm;       /* one turn's recording, in PSRAM */
static char *       ref_audio; /* the clip to clone, base64 */
static char *       ref_text;  /* what that clip says */

/* ------------------------------------------------------------------ */
/* PSRAM, for turn.c's buffers as well as ours.                               */
/* ------------------------------------------------------------------ */

static void * psram_grow(void * block, size_t bytes)
{
    return heap_caps_realloc(block, bytes, MALLOC_CAP_SPIRAM);
}

static void psram_release(void * block)
{
    heap_caps_free(block);
}

static const voice_alloc_t PSRAM = { psram_grow, psram_release };

/* ------------------------------------------------------------------ */
/* HTTP. One connection per request, the same as the host's socket: a turn    */
/* makes three requests seconds apart, so keeping one alive would be          */
/* bookkeeping for nothing.                                                   */
/* ------------------------------------------------------------------ */

/* The server, and what it wants to see. Parsed once, at start-up, by the same
 * function the host uses — so an address that works in the simulator works
 * here and is written down the same way. */
static voice_url_t server;

/* POSTs body to path and reads the whole reply into out. Reports the server's
 * own status on a failure — all three services answer a bad request with a
 * sentence saying what was wrong, and it is the fastest route to the cause. */
static bool http_post(const char * path, const char * content_type,
                      const void * body, size_t body_len, voice_buf_t * out)
{
    char url[VOICE_HOST_BYTES + 64];
    char auth[VOICE_KEY_BYTES + 8];
    esp_http_client_handle_t client;
    esp_http_client_config_t config = { 0 };
    int status, written;
    int64_t length;
    bool ok = false;

    /* Built from the parsed parts rather than pasted onto the configured
     * string: a trailing slash or a path someone left on the address cannot
     * turn into a 404 that way, and the port is always the one turn.c settled
     * on. */
    snprintf(url, sizeof(url), "%s://%s:%s%s",
             server.tls ? "https" : "http", server.host, server.port, path);
    config.url = url;
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = VOICE_HTTP_TIMEOUT_MS;
    /* The certificate bundle ESP-IDF already compiles in — sdkconfig has
     * CONFIG_MBEDTLS_CERTIFICATE_BUNDLE on. Without this an https address
     * fails the handshake with nothing but "esp-tls" in the log. */
    if (server.tls) config.crt_bundle_attach = esp_crt_bundle_attach;

    client = esp_http_client_init(&config);
    if (client == NULL) return false;

    esp_http_client_set_header(client, "Content-Type", content_type);

    /* No key, no header: an "Authorization: Bearer " with nothing behind it
     * is a rejected request rather than an unauthenticated one. */
    if (CONFIG_WATCH_VOICE_KEY[0] != '\0') {
        snprintf(auth, sizeof auth, "Bearer %s", CONFIG_WATCH_VOICE_KEY);
        esp_http_client_set_header(client, "Authorization", auth);
    }

    if (esp_http_client_open(client, (int) body_len) != ESP_OK) {
        ESP_LOGE(TAG, "%s is not answering — is the server up and reachable?", url);
        goto done;
    }

    written = esp_http_client_write(client, body, (int) body_len);
    if (written < 0 || (size_t) written != body_len) {
        ESP_LOGE(TAG, "sending to %s failed", path);
        goto done;
    }

    length = esp_http_client_fetch_headers(client);
    status = esp_http_client_get_status_code(client);

    /* Read to the end whatever the status: the body of a failure is the
     * message that says why, and it goes in the log below. */
    for (;;) {
        char chunk[2048];
        int got = esp_http_client_read(client, chunk, sizeof(chunk));
        if (got <= 0) break;
        if (!voice_buf_add(out, chunk, (size_t) got)) goto done;
        if (!atomic_load(&running)) goto done;
        if (length > 0 && (int64_t) out->len >= length) break;
    }

    if (status != 200) {
        ESP_LOGE(TAG, "%s: HTTP %d: %.200s", path, status,
                 out->data != NULL ? out->data : "");
        if (status == 401 || status == 403) {
            ESP_LOGE(TAG, "the server wants a key — set CONFIG_WATCH_VOICE_KEY");
        }
        voice_buf_free(out);
        goto done;
    }
    ok = out->len > 0;

done:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ok;
}

/* ------------------------------------------------------------------ */
/* The two services. Both bodies are built in voice/turn.c; all that is left  */
/* here is putting them on the wire.                                          */
/* ------------------------------------------------------------------ */

static bool transcribe(const int16_t * samples_at, size_t samples, char * out, size_t cap)
{
    voice_buf_t body = { 0 }, reply = { 0 };
    bool ok = false;

    if (!voice_stt_body(&body, samples_at, samples)) goto done;
    if (!http_post(VOICE_STT_PATH, VOICE_STT_TYPE, body.data, body.len, &reply)) goto done;

    ok = voice_stt_text(reply.data, out, cap);
    if (!ok) ESP_LOGW(TAG, "no transcript in the reply");

done:
    voice_buf_free(&body);
    voice_buf_free(&reply);
    return ok;
}

static bool synthesise(const char * text, voice_buf_t * wav)
{
    voice_buf_t body = { 0 };
    bool ok = false;

    if (ref_audio == NULL) goto done; /* nothing to clone, so nothing to say */
    if (!voice_tts_body(&body, text, ref_audio, ref_text)) goto done;

    ok = http_post(VOICE_TTS_PATH, VOICE_TTS_TYPE, body.data, body.len, wav);

done:
    voice_buf_free(&body);
    return ok;
}

/* ------------------------------------------------------------------ */
/* The devices.                                                               */
/* ------------------------------------------------------------------ */

/* Records one utterance and returns how many samples it was.
 *
 * `patience_ms` is how long it will wait for the first word before giving up;
 * VOICE_WAIT_FOREVER waits, which is what the loop asks for while the
 * assistant is asleep and there is nothing to time out of. Everything about
 * when a turn starts and ends is voice/turn.c's; all that is here is working
 * the device, exactly as in host/voice.c.
 *
 * Zero back means nothing worth sending: the patience ran out, what arrived
 * was too short to be a word, or the watch is shutting down. */
static size_t record(uint32_t patience_ms)
{
    voice_turn_t turn;

    voice_turn_start(&turn, patience_ms);

    if (!board_mic_open()) return 0;
    atomic_store(&recording, true);

    while (atomic_load(&running)) {
        size_t at = voice_turn_at(&turn);
        size_t room = VOICE_MAX_SAMPLES - at;
        size_t want = room < VOICE_BLOCK_SAMPLES ? room : VOICE_BLOCK_SAMPLES;
        size_t got;

        if (voice_turn_wait(&turn) == VOICE_NOTHING) break;
        if (voice_turn_dead(&turn)) {
            /* The simulator lists the inputs it can see here, because a Mac
             * has several and picks for you. This board has one, so what is
             * left to suspect is the gain and the wiring behind it. */
            ESP_LOGW(TAG, "the microphones have delivered nothing but zeros for %d s — "
                          "suspect BOARD_MIC_GAIN_DB, or the ES7210 behind it",
                     VOICE_DEAD_MS / 1000);
        }

        /* board_mic_read blocks until it has the whole block, so a working
         * codec paces this loop the way SDL_Delay paces the simulator's. */
        got = board_mic_read(pcm + at, want);
        if (got == 0) {
            /* Nothing came back at all, which is a codec that has stopped
             * rather than a quiet room — a quiet room still delivers silence.
             * Without this wait the task would spin at full speed, and while
             * the assistant is asleep there is no patience to run out and end
             * it. */
            vTaskDelay(pdMS_TO_TICKS(VOICE_BLOCK_MS));
            continue;
        }
        if (voice_turn_feed(&turn, pcm + at, got) == VOICE_DONE) break;
    }

    atomic_store(&recording, false);

    return voice_turn_samples(&turn);
}

/* Plays a WAV, opening the speaker to match it. Returns when the last sample
 * has gone out, or at once when the watch is shutting down.
 *
 * Nothing else cuts a reply short. Talking over the assistant means being
 * heard while it is speaking, which needs acoustic echo cancellation this
 * half-duplex loop has none of — the same reason the microphone and speaker
 * corners are two flags rather than one mode. ESP-SR gives the S3 that, and
 * the interrupt comes back with it. */
static void play(const unsigned char * file, size_t len)
{
    voice_wav_t wav;
    size_t at = 0;

    if (!voice_wav_open(file, len, &wav)) {
        ESP_LOGW(TAG, "the reply is not a WAV this can play");
        return;
    }

    /* The server has answered 24 kHz mono every time, but it says so in the
     * header rather than promising it, so the watch follows the file. Opening
     * the speaker closes the microphone: one I2S bus, one clock. */
    if (!board_speaker_open(wav.rate, wav.channels, wav.bits)) return;

    atomic_store(&speaking, true);
    while (at < wav.bytes && atomic_load(&running)) {
        size_t chunk = wav.bytes - at;
        if (chunk > 4096) chunk = 4096;
        if (!board_speaker_write(wav.pcm + at, chunk)) break;
        at += chunk;
    }
    atomic_store(&speaking, false);

    board_speaker_close();
}

/* What the assistant answers, into out. The same two modes the simulator has,
 * behind the same function: the question said back when no brain is
 * configured, and a chat model's answer when one is. A failure is silence
 * rather than an echo — a watch that repeats the question when the model
 * could not be reached looks like it answered. */
static bool reply(const char * heard, char * out, size_t cap)
{
    voice_buf_t body = { 0 }, answer = { 0 };
    bool ok = false;

    if (voice_models_get()->brain[0] == '\0') {
        snprintf(out, cap, "%s", heard);
        return true;
    }

    if (!voice_brain_body(&body, heard)) goto done;
    if (!http_post(VOICE_BRAIN_PATH, VOICE_BRAIN_TYPE, body.data, body.len, &answer)) goto done;

    ok = voice_brain_text(answer.data, out, cap);
    if (!ok) ESP_LOGW(TAG, "no answer in what %s sent back", voice_models_get()->brain);

done:
    voice_buf_free(&body);
    voice_buf_free(&answer);
    return ok;
}

/* One pass is one utterance. Asleep, the only thing that matters about it is
 * whether the watch's name is in there; awake, all of it is something to
 * answer, except the goodbye. Both questions are voice/turn.c's to answer, so
 * this asks them in the same order as host/voice.c and gets the same replies. */
static void loop(void * unused)
{
    (void) unused;

    while (atomic_load(&running)) {
        /* Static, not automatic: 8 kB of transcripts do not fit on a task
         * stack that also has to carry a TLS handshake. One task, one of
         * each — see VOICE_TASK_STACK. */
        static char heard[VOICE_MAX_TEXT];
        static char answer[VOICE_MAX_TEXT];
        voice_buf_t wav = { 0 };
        const char * say;
        size_t samples;

        /* No radio, no assistant: both services are on the other end of it.
         * The watch keeps time and draws its corners meanwhile. */
        if (!net_is_up()) {
            board_mic_close();
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        /* Awake, a stretch with nothing said is the end of the session: with
         * nothing to press, there has to be a way back to the clock that
         * needs nothing said at all. Asleep, there is nothing to leave. */
        samples = record(atomic_load(&awake) ? VOICE_IDLE_MS : VOICE_WAIT_FOREVER);
        if (samples == 0) {
            if (atomic_load(&awake) && atomic_load(&running)) {
                ESP_LOGI(TAG, "nothing said for %d s — back to the clock",
                         VOICE_IDLE_MS / 1000);
                atomic_store(&awake, false);
            }
            continue;
        }

        if (!transcribe(pcm, samples, heard, sizeof(heard))) continue;
        if (heard[0] == '\0') continue; /* the transcriber heard no words */

        say = heard;

        if (!atomic_load(&awake)) {
            const char * rest = voice_after_wake(heard);
            if (rest == NULL) {
                /* The room talking, not the watch. Logged rather than
                 * dropped: choosing which spellings to listen for means
                 * reading what the transcriber actually produced, and silence
                 * here is what makes a watch that will not wake impossible to
                 * diagnose. */
                ESP_LOGI(TAG, "not for me: \"%s\"", heard);
                continue;
            }
            ESP_LOGI(TAG, "woken by \"%s\"", heard);
            atomic_store(&awake, true);
            if (*rest == '\0') continue; /* its name and nothing after it */
            say = rest; /* "Hey Kai, hallo" is a turn whose answer is "hallo" */
        }
        else if (voice_is_goodbye(heard)) {
            ESP_LOGI(TAG, "\"%s\" — back to the clock", heard);
            atomic_store(&awake, false);
            continue;
        }
        else {
            ESP_LOGI(TAG, "heard \"%s\"", heard);
        }

        if (!reply(say, answer, sizeof answer)) continue;
        if (voice_models_get()->brain[0] != '\0') ESP_LOGI(TAG, "answering \"%s\"", answer);

        if (synthesise(answer, &wav)) {
            play((const unsigned char *) wav.data, wav.len);
        }
        voice_buf_free(&wav);
    }

    board_mic_close();
    board_speaker_close();
    worker = NULL;
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* Start-up.                                                                  */
/* ------------------------------------------------------------------ */

/* Reads the clip KAI borrows its voice from, out of flash. Without it the
 * synthesiser answers 500 to everything, so the loop simply does not start —
 * the same answer this firmware gives an unconfigured SSID. */
static bool load_voice(void)
{
#if CONFIG_WATCH_HAS_VOICE
    size_t clip_len = (size_t) (kai_opus_end - kai_opus_start);
    size_t words_len = (size_t) (kai_txt_end - kai_txt_start);

    ref_audio = voice_base64(kai_opus_start, clip_len);
    if (ref_audio == NULL) return false;

    /* The transcript is a line in a file, so it arrives with a newline on it. */
    while (words_len > 0) {
        char last = (char) kai_txt_start[words_len - 1];
        if (last != '\n' && last != '\r' && last != ' ') break;
        words_len--;
    }
    ref_text = psram_grow(NULL, words_len + 1);
    if (ref_text == NULL) return false;
    memcpy(ref_text, kai_txt_start, words_len);
    ref_text[words_len] = '\0';

    ESP_LOGI(TAG, "the voice to borrow is %u bytes of opus", (unsigned) clip_len);
    return true;
#else
    ESP_LOGW(TAG, "built without voices/kai.opus — the assistant stays asleep");
    return false;
#endif
}

/* Everything the platform knows and turn.c does not: where the server is,
 * which models answer and what the watch is called. The host reads the same
 * six settings out of the environment and calls the same two setters with
 * them, so what the watch is told and what the simulator is told is one list.
 *
 * False means there is nowhere to ask, which is a clock and not an error. */
static bool configure(void)
{
    voice_models_t models;

    if (!voice_url_parse(CONFIG_WATCH_VOICE_URL, &server)) {
        ESP_LOGW(TAG, "no server configured — the watch stays a clock");
        return false;
    }

    /* Empty rather than the default spelled out again: menuconfig says
     * nothing and turn.c uses the one list it holds. */
    models.stt = CONFIG_WATCH_STT_MODEL;
    models.brain = CONFIG_WATCH_BRAIN_MODEL;
    models.tts = CONFIG_WATCH_TTS_MODEL;
    models.language = CONFIG_WATCH_LANGUAGE;
    if (!voice_models_set(&models)) {
        ESP_LOGW(TAG, "a model name does not fit — using the default for it");
    }
    if (!voice_name_set(CONFIG_WATCH_NAME)) {
        ESP_LOGW(TAG, "that name does not fit — the assistant is still %s", voice_name());
    }
    if (!voice_wake_set(CONFIG_WATCH_WAKE_PHRASE)) {
        ESP_LOGW(TAG, "that wake phrase does not fit — listening for the default");
    }
    /* Both on one line, because they are two settings and forgetting the
     * second is the mistake: a watch renamed only in the wake phrase still
     * answers "who are you?" with the name in its system prompt. */
    if (voice_wake_count() == 1) {
        ESP_LOGI(TAG, "called %s, answering to \"%s\"", voice_name(), voice_wake_at(0));
    }
    else {
        ESP_LOGI(TAG, "called %s, answering to \"%s\" and %u other spellings of it",
                 voice_name(), voice_wake_at(0), (unsigned) (voice_wake_count() - 1));
    }

    /* The key is never logged, only whether there is one. */
    ESP_LOGI(TAG, "%s://%s:%s, %s key",
             server.tls ? "https" : "http", server.host, server.port,
             CONFIG_WATCH_VOICE_KEY[0] != '\0' ? "with a" : "with no");
    return true;
}

void voice_start(void)
{
    /* turn.c's buffers before anything else asks it for one: a turn's body is
     * the 640 kB recording plus a WAV header, and internal SRAM is 512 kB in
     * total. */
    voice_buf_alloc(&PSRAM);

    if (!configure()) return;
    if (!load_voice()) return;
    if (!board_audio_init()) {
        ESP_LOGE(TAG, "no audio — the watch stays a clock");
        return;
    }

    pcm = heap_caps_malloc(VOICE_MAX_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (pcm == NULL) {
        ESP_LOGE(TAG, "no room for a turn's recording");
        return;
    }

    atomic_store(&running, true);
    if (xTaskCreate(loop, "kai-voice", VOICE_TASK_STACK, NULL,
                    VOICE_TASK_PRIO, &worker) != pdPASS) {
        ESP_LOGE(TAG, "no task");
        atomic_store(&running, false);
        return;
    }

    ESP_LOGI(TAG, "waiting to be called, hearing through %s and speaking as %s",
             voice_models_get()->stt, voice_models_get()->tts);
    if (voice_models_get()->brain[0] != '\0') {
        ESP_LOGI(TAG, "thinking with %s", voice_models_get()->brain);
    }
    else {
        ESP_LOGI(TAG, "brain is \"%s\" — the answer is the question said back",
                 VOICE_BRAIN_ECHO);
    }
}

void voice_stop(void)
{
    atomic_store(&running, false);
    atomic_store(&awake, false);
    while (worker != NULL) vTaskDelay(pdMS_TO_TICKS(10));
}

bool voice_awake(void)
{
    return atomic_load(&awake);
}

/* Awake as well as recording. The microphone is open the whole time the loop
 * runs, but a corner that is always lit says nothing — this one means the
 * next thing said is meant for the assistant. */
bool voice_listening(void)
{
    return atomic_load(&recording) && atomic_load(&awake);
}

bool voice_speaking(void)
{
    return atomic_load(&speaking);
}
