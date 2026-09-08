/**
 * @file bench.c
 * Where a turn's seconds go, measured against the server that is configured.
 *
 * "It is slow" is not a thing that can be fixed. Which of the three services
 * is slow, and by how much, is — and the answer moves with the model, the
 * network and the length of the reference clip, so it has to be measurable
 * again rather than written down once.
 *
 * It is the fourth host, and it exists on the same terms as the other three:
 * it asks the same three services the same three questions, with the bodies
 * built by voice/turn.c and put on the wire by host/speech.c, which is what
 * makes the numbers the watch's own rather than curl's. A benchmark with its
 * own HTTP client would drift from the loop it claims to measure exactly the
 * way a second copy of the wake phrase would.
 *
 * No microphone and no speaker. The recording is voices/female.wav, which is
 * already there for the synthesiser to clone and is a known five seconds of
 * German — so the transcription has a right answer, and the run says whether
 * it got it. The reply is played nowhere; only its length is looked at.
 *
 * A turn measured here is one wake-less turn: transcribe, think, speak. It
 * skips the waiting, which is the one part of a real turn that is not the
 * server's fault.
 */

#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "speech.h"
#include "turn.h"

/* Three is enough to see a median and cheap enough to run while waiting: a
 * turn against a remote brain is ten seconds, so five runs is a minute. */
#define BENCH_RUNS_DEFAULT 3
#define BENCH_RUNS_MAX     20

/* What the benchmark says, which has to be a thing the brain answers without
 * reaching for a tool — the point is to time the three services, and a tool
 * call would time four requests as three. The question the clip already asks
 * is the honest choice: it is what the transcriber will hand over anyway. */
#define BENCH_FALLBACK "Hallo, wie geht es dir?"

typedef struct {
    double stt_ms;
    double brain_ms;
    double tts_ms;
    double total_ms;
} run_t;

static double now_ms(void)
{
    return (double) SDL_GetPerformanceCounter() * 1000.0
           / (double) SDL_GetPerformanceFrequency();
}

/* The whole of a file, so the clip can be read without borrowing speech.c's
 * private one. */
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

/* How many seconds of audio a WAV holds, for the real-time factor. */
static double wav_seconds(const voice_wav_t * wav)
{
    long rate = wav->rate * wav->channels * (wav->bits / 8);

    return rate > 0 ? (double) wav->bytes / (double) rate : 0.0;
}

static int by_size(const void * a, const void * b)
{
    double x = *(const double *) a, y = *(const double *) b;

    return x < y ? -1 : x > y ? 1 : 0;
}

static double median(const double * values, size_t n)
{
    double sorted[BENCH_RUNS_MAX];

    memcpy(sorted, values, n * sizeof(double));
    qsort(sorted, n, sizeof(double), by_size);
    return n % 2 ? sorted[n / 2] : (sorted[n / 2 - 1] + sorted[n / 2]) / 2.0;
}

static void column(const run_t * runs, size_t n, size_t offset, double * out)
{
    size_t i;

    for (i = 0; i < n; i++) out[i] = *(const double *) ((const char *) &runs[i] + offset);
}

int main(int argc, char ** argv)
{
    const char * clip_path = getenv(VOICE_ENV_CLIP);
    unsigned char * clip;
    size_t clip_len = 0;
    voice_wav_t heard_wav;
    run_t runs[BENCH_RUNS_MAX];
    double col[BENCH_RUNS_MAX];
    long want = BENCH_RUNS_DEFAULT;
    size_t n = 0, i;
    double said_s = 0.0, spoke_s = 0.0;
    char transcript[VOICE_MAX_TEXT] = "";
    char answer[VOICE_MAX_TEXT] = "";

    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (argc > 1) {
        want = strtol(argv[1], NULL, 10);
        if (want < 1 || want > BENCH_RUNS_MAX) {
            fprintf(stderr, "bench: runs must be 1 to %d\n", BENCH_RUNS_MAX);
            return 2;
        }
    }

    if (!speech_start()) {
        fprintf(stderr, "bench: nothing to measure — see the lines above\n");
        return 1;
    }

    /* The same clip the synthesiser clones, read a second time as something
     * to transcribe. It is the one recording this repository is sure of. */
    if (clip_path == NULL || clip_path[0] == '\0') clip_path = VOICE_CLIP_DEFAULT;
    clip = slurp(clip_path, &clip_len);
    if (clip == NULL || !voice_wav_open(clip, clip_len, &heard_wav)) {
        fprintf(stderr, "bench: %s is not a WAV this can read\n", clip_path);
        free(clip);
        return 1;
    }
    if (heard_wav.rate != VOICE_RATE || heard_wav.channels != 1) {
        fprintf(stderr, "bench: %s is %d Hz, %d channels — it has to be %d mono\n",
                clip_path, heard_wav.rate, heard_wav.channels, VOICE_RATE);
        free(clip);
        return 1;
    }
    said_s = wav_seconds(&heard_wav);

    printf("\n  %s, %s\n", clip_path, voice_models_get()->brain[0] != '\0'
                                          ? voice_models_get()->brain
                                          : "no brain (echo)");
    printf("  %.1f s of speech, %ld run%s\n\n", said_s, want, want == 1 ? "" : "s");
    printf("  run          stt        brain          tts        turn\n");
    printf("  ----------------------------------------------------------\n");

    for (i = 0; i < (size_t) want; i++) {
        voice_buf_t spoken = { 0 };
        voice_wav_t back;
        run_t r;
        double t0;

        t0 = now_ms();
        if (!speech_transcribe((const int16_t *) heard_wav.pcm,
                               heard_wav.bytes / sizeof(int16_t),
                               transcript, sizeof transcript)) {
            fprintf(stderr, "bench: the transcriber did not answer\n");
            break;
        }
        r.stt_ms = now_ms() - t0;

        /* Whatever came back, so the brain is asked a real transcript; the
         * fallback is only for a transcriber that answered with nothing. */
        t0 = now_ms();
        if (!speech_reply(transcript[0] != '\0' ? transcript : BENCH_FALLBACK,
                          answer, sizeof answer)) {
            fprintf(stderr, "bench: the brain did not answer\n");
            break;
        }
        r.brain_ms = now_ms() - t0;

        t0 = now_ms();
        if (!speech_synthesise(answer, &spoken)) {
            fprintf(stderr, "bench: the synthesiser did not answer\n");
            voice_buf_free(&spoken);
            break;
        }
        r.tts_ms = now_ms() - t0;
        if (voice_wav_open((const unsigned char *) spoken.data, spoken.len, &back)) {
            spoke_s = wav_seconds(&back);
        }
        voice_buf_free(&spoken);

        r.total_ms = r.stt_ms + r.brain_ms + r.tts_ms;
        runs[n++] = r;
        printf("  %3zu   %8.0f ms  %8.0f ms  %8.0f ms  %8.0f ms\n",
               i + 1, r.stt_ms, r.brain_ms, r.tts_ms, r.total_ms);
        fflush(stdout);
    }

    if (n == 0) {
        free(clip);
        speech_stop();
        return 1;
    }

    printf("  ----------------------------------------------------------\n");
    column(runs, n, offsetof(run_t, stt_ms), col);
    printf("  med   %8.0f ms", median(col, n));
    column(runs, n, offsetof(run_t, brain_ms), col);
    printf("  %8.0f ms", median(col, n));
    column(runs, n, offsetof(run_t, tts_ms), col);
    printf("  %8.0f ms", median(col, n));
    column(runs, n, offsetof(run_t, total_ms), col);
    printf("  %8.0f ms\n\n", median(col, n));

    /* What the numbers are of, so a row can be read a week later. The
     * real-time factor is the one that says whether the chunker would help:
     * above 1.0 the watch is still talking when the next reply could have
     * started. */
    column(runs, n, offsetof(run_t, tts_ms), col);
    printf("  heard   \"%s\"\n", transcript);
    printf("  said    \"%s\"\n", answer);
    printf("  spoke   %.1f s of audio in %.1f s, %.2fx real time\n\n",
           spoke_s, median(col, n) / 1000.0,
           spoke_s > 0.0 ? median(col, n) / 1000.0 / spoke_s : 0.0);

    free(clip);
    speech_stop();
    return 0;
}
