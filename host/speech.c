/**
 * @file speech.c
 * The host's half of the three speech services. See speech.h for why this is
 * a file of its own and what is left next door in voice.c.
 *
 * Host-only, like voice.c. Nothing in here is a device: it is an address out
 * of the environment, a socket with OpenSSL on top when the address said
 * https, the clip a voice is cloned from, and the three requests a turn puts
 * on the wire. Every one of those bodies is built in voice/turn.c and none of
 * them is built here, which is what lets the simulator, the benchmark and the
 * watch all ask the same questions.
 *
 * - **A socket and OpenSSL, and nothing above them.** SPEC.md allows LVGL,
 *   SDL2 and OpenSSL, so the HTTP client below is still a request written by
 *   hand and a reply read back — OpenSSL replaces send() and recv() and
 *   nothing else. That is not an inconvenience: the device speaks to the same
 *   three endpoints through esp_http_client, and a body built by hand ports
 *   where a libcurl call site would not.
 *
 *   TLS is here because the server may not be on this desk. omlx.ai-at-home.de
 *   answers 308 on port 80, so a watch that talks to it over the internet has
 *   no plaintext option; an oMLX on the same machine still does, and an
 *   http:// address skips all of this.
 *
 * - **chatterbox-multilingual-v3 ships no voice conditionals**, so it answers
 *   500 to every request that carries no clip to clone. The clip named by
 *   WATCH_VOICE_CLIP and the transcript beside it are what it borrows a voice
 *   from, they are gitignored, and without them the loop stays quiet — the
 *   same choice the firmware makes about an unconfigured SSID.
 *
 * SDL is here for SDL_Log and nothing else. Every host program already links
 * it, and the alternative was a logging seam of its own for the sake of one
 * function.
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

#include "speech.h"
#include "turn.h"


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

/* The server, and what it wants to see. Both are read once, at start-up. */
static voice_url_t server;
static char        api_key[VOICE_KEY_BYTES];
static SSL_CTX *   tls;
/* What says a request should give up early. See speech_cancel_set(). */
static bool (*cancelled)(void);

void speech_cancel_set(bool (*fn)(void))
{
    cancelled = fn;
}

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
            if (cancelled != NULL && cancelled()) goto done;
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
bool speech_transcribe(const int16_t * pcm, size_t samples, char * out, size_t cap)
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
bool speech_synthesise(const char * text, voice_buf_t * wav)
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
bool speech_reply(const char * heard, char * out, size_t cap)
{
    voice_chat_t chat = { 0 };
    voice_buf_t body = { 0 }, answer = { 0 };
    char tools[VOICE_TOOL_LOG_BYTES];
    bool ok = false;

    if (voice_models_get()->brain[0] == '\0') {
        snprintf(out, cap, "%s", heard);
        return true;
    }

    if (!voice_chat_start(&chat, heard)) goto done;

    /* One question is more than one request as soon as the brain can ask the
     * watch what time it is. It is voice_chat_tools() that caps the rounds,
     * so this cannot run away, and the buffers are given back at the top of
     * each pass rather than the bottom because the last pass's answer is what
     * gets read out below. */
    for (;;) {
        voice_buf_free(&body);
        voice_buf_free(&answer);
        if (!voice_chat_body(&chat, &body)) goto done;
        if (!http_post(VOICE_BRAIN_PATH, VOICE_BRAIN_TYPE, body.data, body.len,
                       &answer, NULL, 0)) goto done;
        if (!voice_chat_tools(&chat, answer.data, tools, sizeof tools)) break;
        SDL_Log("voice: %s", tools);
    }

    ok = voice_brain_text(answer.data, out, cap);
    if (!ok) SDL_Log("voice: no answer in what %s sent back", voice_models_get()->brain);

done:
    voice_buf_free(&body);
    voice_buf_free(&answer);
    voice_chat_free(&chat);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Start-up.                                                                  */
/* ------------------------------------------------------------------ */

/* One setting, or the fallback when it is unset or empty. Empty is treated as
 * unset throughout: an exported variable someone cleared should mean the same
 * as one they never wrote. */
static const char * env(const char * name, const char * fallback)
{
    const char * value = getenv(name);

    return (value != NULL && value[0] != '\0') ? value : fallback;
}

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
/* The transcript beside a clip: the same path with its extension replaced by
 * .txt. Written into `out` because the caller wants both names for its log. */
static void beside(const char * clip, char * out, size_t cap)
{
    size_t len = strlen(clip);
    size_t cut = len;

    while (cut > 0 && clip[cut - 1] != '.' && clip[cut - 1] != '/') cut--;
    if (cut == 0 || clip[cut - 1] == '/') cut = len; /* no extension to replace */
    else cut--;

    if (cut + 5 > cap) cut = cap > 5 ? cap - 5 : 0;
    memcpy(out, clip, cut);
    memcpy(out + cut, ".txt", 5);
}

static bool load_voice(void)
{
    const char * clip_path = env(VOICE_ENV_CLIP, VOICE_CLIP_DEFAULT);
    char text_path[512];
    size_t clip_len = 0;
    unsigned char * clip;
    unsigned char * words;
    size_t i;

    beside(clip_path, text_path, sizeof text_path);

    clip = slurp(clip_path, &clip_len);
    if (clip == NULL) {
        SDL_Log("voice: no %s — the assistant stays asleep", clip_path);
        return false;
    }
    words = slurp(text_path, NULL);
    if (words == NULL) {
        SDL_Log("voice: no %s — the clip's words are needed too", text_path);
        free(clip);
        return false;
    }
    SDL_Log("voice: borrowing a voice from %s, %zu kB", clip_path, clip_len / 1024);

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

/* Both halves of getting ready, in the order their log lines read best: where
 * the server is and who the watch is, then the clip it speaks with. Either
 * one failing is a watch that stays a clock, so they answer the same way. */
bool speech_start(void)
{
    return configure() && load_voice();
}

void speech_stop(void)
{
    voice_free(ref_audio);
    free(ref_text);
    ref_audio = ref_text = NULL;
    if (tls != NULL) {
        SSL_CTX_free(tls);
        tls = NULL;
    }
}
