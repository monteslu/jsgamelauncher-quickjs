/*
 * HTTP/1.1 client, backing fetch() and XHR for remote URLs.
 *
 * Runs on a worker thread per request: the transport is blocking, and doing this
 * on the frame thread would stall the game for the whole round trip. JS starts a
 * request, then polls it once per frame from the same hook that drains
 * WebSocket messages — which is what lets fetch return a real Promise instead of
 * blocking.
 *
 * Deliberately small: it does what a game needs (GET/POST/PUT/DELETE, headers, a
 * body, redirects, chunked responses) and nothing else. No connection reuse, no
 * HTTP/2, no cookie jar. Each of those is a correctness surface, and a game
 * calling a leaderboard endpoint does not need them.
 */
#include "host.h"
#include "net_tls.h"

#include <SDL2/SDL.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define MAX_REQUESTS   16
/* A response larger than this is refused rather than accumulated. Content-Length
   comes from the network and must never be trusted as an allocation size. */
#define MAX_BODY_BYTES (64u << 20)
#define MAX_REDIRECTS  5

typedef struct {
    int          used;
    volatile int done;          /* set by the thread, polled by JS */
    volatile int failed;
    SDL_Thread  *thread;

    /* Request, owned by the worker once started. */
    char  *method;
    char  *url;
    char  *headers;             /* pre-joined "K: V\r\n" block */
    char  *body;
    size_t body_len;

    /* Response. */
    int    status;
    char  *status_text;
    char  *resp_headers;
    char  *resp_body;
    size_t resp_len;
    char   err[256];
} Req;

static Req g_reqs[MAX_REQUESTS];

/* ------------------------------------------------------------------- utils -- */

static char *dupstr(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *d = (char *)malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

/* Split "https://host:port/path" without a URL library. */
static int parse_url(const char *url, int *secure, char *host, size_t hostlen,
                     int *port, char *path, size_t pathlen)
{
    const char *p = url;
    if (!strncmp(p, "https://", 8))      { *secure = 1; *port = 443; p += 8; }
    else if (!strncmp(p, "http://", 7))  { *secure = 0; *port = 80;  p += 7; }
    else return 0;

    const char *slash = strchr(p, '/');
    const char *hostend = slash ? slash : p + strlen(p);

    const char *colon = memchr(p, ':', (size_t)(hostend - p));
    if (colon) {
        *port = atoi(colon + 1);
        hostend = colon;
    }
    size_t hl = (size_t)(hostend - p);
    if (hl == 0 || hl >= hostlen) return 0;
    memcpy(host, p, hl);
    host[hl] = 0;

    if (slash) snprintf(path, pathlen, "%s", slash);
    else       snprintf(path, pathlen, "/");
    return 1;
}

/* Read until the header terminator, keeping any body bytes that arrive with it.
   Reading byte-by-byte past the headers would be slow; over-reading and
   remembering the remainder is what keeps the body intact. */
static int read_headers(JsglqStream *s, char **out, size_t *out_len,
                        char **leftover, size_t *leftover_len)
{
    size_t cap = 8192, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) return 0;

    for (;;) {
        if (len + 1024 > cap) {
            cap *= 2;
            char *bigger = (char *)realloc(buf, cap);
            if (!bigger) { free(buf); return 0; }
            buf = bigger;
        }
        int r = jsglq_stream_recv(s, buf + len, (int)(cap - len - 1));
        if (r <= 0) { free(buf); return 0; }
        len += (size_t)r;
        buf[len] = 0;

        char *end = strstr(buf, "\r\n\r\n");
        if (end) {
            size_t hlen = (size_t)(end - buf) + 4;
            *leftover_len = len - hlen;
            *leftover = (char *)malloc(*leftover_len + 1);
            if (*leftover) {
                memcpy(*leftover, buf + hlen, *leftover_len);
                (*leftover)[*leftover_len] = 0;
            } else {
                *leftover_len = 0;
            }
            buf[hlen] = 0;
            *out = buf;
            *out_len = hlen;
            return 1;
        }
        if (len > (1u << 20)) { free(buf); return 0; }   /* absurd header block */
    }
}

static const char *find_header(const char *headers, const char *name)
{
    size_t nlen = strlen(name);
    for (const char *p = headers; p && *p; ) {
        const char *eol = strstr(p, "\r\n");
        if (!eol) break;
        /* Header names are case-insensitive. */
        if ((size_t)(eol - p) > nlen && !strncasecmp(p, name, nlen) && p[nlen] == ':') {
            const char *v = p + nlen + 1;
            while (*v == ' ') v++;
            return v;
        }
        p = eol + 2;
    }
    return NULL;
}

/* ------------------------------------------------------------------ worker -- */

static int http_thread(void *data)
{
    Req *r = (Req *)data;

    char url[2048];
    snprintf(url, sizeof(url), "%s", r->url);

    for (int redirect = 0; redirect <= MAX_REDIRECTS; redirect++) {
        int secure = 0, port = 80;
        char host[256], path[1600];
        if (!parse_url(url, &secure, host, sizeof(host), &port, path, sizeof(path))) {
            snprintf(r->err, sizeof(r->err), "unsupported URL: %s", url);
            r->failed = 1; r->done = 1; return 0;
        }
        if (secure && !jsglq_tls_available()) {
            snprintf(r->err, sizeof(r->err),
                     "https:// requires TLS, which this build does not include");
            r->failed = 1; r->done = 1; return 0;
        }

        JsglqStream *s = jsglq_stream_connect(host, port, secure,
                                              r->err, sizeof(r->err));
        if (!s) { r->failed = 1; r->done = 1; return 0; }

        /* Request. Connection: close because there is no connection reuse — and
           saying so is what lets a server delimit the body by closing. */
        char head[4096];
        int n = snprintf(head, sizeof(head),
            "%s %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "User-Agent: jsgamelauncher-quickjs\r\n"
            "Accept: */*\r\n"
            "Connection: close\r\n"
            "%s"
            "%s"
            "\r\n",
            r->method, path, host,
            r->headers ? r->headers : "",
            r->body_len ? "" : "");
        if (n <= 0 || n >= (int)sizeof(head)) {
            snprintf(r->err, sizeof(r->err), "request headers too large");
            jsglq_stream_close(s); r->failed = 1; r->done = 1; return 0;
        }
        /* Content-Length must precede the blank line, so splice it in when there
           is a body rather than appending after the terminator. */
        if (r->body_len) {
            char with_len[4096];
            char *term = strstr(head, "\r\n\r\n");
            if (term) {
                *term = 0;
                snprintf(with_len, sizeof(with_len), "%s\r\nContent-Length: %zu\r\n\r\n",
                         head, r->body_len);
                snprintf(head, sizeof(head), "%s", with_len);
                n = (int)strlen(head);
            }
        }

        if (jsglq_stream_send(s, head, n) != n) {
            snprintf(r->err, sizeof(r->err), "failed to send request");
            jsglq_stream_close(s); r->failed = 1; r->done = 1; return 0;
        }
        if (r->body_len &&
            jsglq_stream_send(s, r->body, (int)r->body_len) != (int)r->body_len) {
            snprintf(r->err, sizeof(r->err), "failed to send request body");
            jsglq_stream_close(s); r->failed = 1; r->done = 1; return 0;
        }

        char *headers = NULL, *leftover = NULL;
        size_t hlen = 0, leftover_len = 0;
        if (!read_headers(s, &headers, &hlen, &leftover, &leftover_len)) {
            snprintf(r->err, sizeof(r->err), "no response from %s", host);
            jsglq_stream_close(s); r->failed = 1; r->done = 1; return 0;
        }

        int status = 0;
        char stext[64] = "";
        sscanf(headers, "HTTP/%*s %d %63[^\r\n]", &status, stext);

        /* Follow redirects here rather than surfacing them: fetch() reports the
           final response, and a game following 302s by hand is not parity. */
        if ((status == 301 || status == 302 || status == 303 ||
             status == 307 || status == 308) && redirect < MAX_REDIRECTS) {
            const char *loc = find_header(headers, "Location");
            if (loc) {
                char next[2048];
                const char *eol = strstr(loc, "\r\n");
                size_t ll = eol ? (size_t)(eol - loc) : strlen(loc);
                if (ll < sizeof(next)) {
                    memcpy(next, loc, ll); next[ll] = 0;
                    /* Relative Location, which servers are allowed to send. */
                    if (next[0] == '/') {
                        char abs[2048];
                        snprintf(abs, sizeof(abs), "%s://%s:%d%s",
                                 secure ? "https" : "http", host, port, next);
                        snprintf(url, sizeof(url), "%s", abs);
                    } else {
                        snprintf(url, sizeof(url), "%s", next);
                    }
                    free(headers); free(leftover);
                    jsglq_stream_close(s);
                    continue;
                }
            }
        }

        /* Body: chunked, Content-Length, or read-until-close. */
        size_t cap = leftover_len + 8192, len = leftover_len;
        char *body = (char *)malloc(cap + 1);
        if (!body) {
            free(headers); free(leftover); jsglq_stream_close(s);
            snprintf(r->err, sizeof(r->err), "out of memory");
            r->failed = 1; r->done = 1; return 0;
        }
        if (leftover_len) memcpy(body, leftover, leftover_len);
        free(leftover);

        const char *cl = find_header(headers, "Content-Length");
        size_t want = cl ? (size_t)strtoull(cl, NULL, 10) : 0;
        if (cl && want > MAX_BODY_BYTES) {
            free(headers); free(body); jsglq_stream_close(s);
            snprintf(r->err, sizeof(r->err), "response too large (%zu bytes)", want);
            r->failed = 1; r->done = 1; return 0;
        }

        for (;;) {
            if (cl && len >= want) break;
            if (len + 4096 > cap) {
                if (cap * 2 > MAX_BODY_BYTES) {
                    free(headers); free(body); jsglq_stream_close(s);
                    snprintf(r->err, sizeof(r->err), "response exceeded %u bytes",
                             MAX_BODY_BYTES);
                    r->failed = 1; r->done = 1; return 0;
                }
                cap *= 2;
                char *bigger = (char *)realloc(body, cap + 1);
                if (!bigger) { free(headers); free(body); jsglq_stream_close(s);
                               snprintf(r->err, sizeof(r->err), "out of memory");
                               r->failed = 1; r->done = 1; return 0; }
                body = bigger;
            }
            int got = jsglq_stream_recv(s, body + len, (int)(cap - len));
            if (got <= 0) break;             /* clean close ends the body */
            len += (size_t)got;
        }
        body[len] = 0;
        jsglq_stream_close(s);

        /* Chunked transfer-encoding, decoded in place: each chunk is a hex
           length, CRLF, data, CRLF, terminated by a zero-length chunk. */
        const char *te = find_header(headers, "Transfer-Encoding");
        if (te && !strncasecmp(te, "chunked", 7)) {
            size_t in = 0, out = 0;
            while (in < len) {
                char *endptr = NULL;
                unsigned long chunk = strtoul(body + in, &endptr, 16);
                if (!endptr || endptr == body + in) break;
                const char *crlf = strstr(endptr, "\r\n");
                if (!crlf) break;
                size_t data_at = (size_t)(crlf - body) + 2;
                if (chunk == 0) break;
                if (data_at + chunk > len) break;
                memmove(body + out, body + data_at, chunk);
                out += chunk;
                in = data_at + chunk + 2;    /* skip the chunk's trailing CRLF */
            }
            len = out;
            body[len] = 0;
        }

        r->status = status;
        r->status_text = dupstr(stext);
        r->resp_headers = headers;
        r->resp_body = body;
        r->resp_len = len;
        r->done = 1;
        return 0;
    }

    snprintf(r->err, sizeof(r->err), "too many redirects");
    r->failed = 1;
    r->done = 1;
    return 0;
}

/* --------------------------------------------------------------------- JS --- */

static int req_alloc(void)
{
    for (int i = 0; i < MAX_REQUESTS; i++) if (!g_reqs[i].used) return i;
    return -1;
}

static void req_free(Req *r)
{
    free(r->method); free(r->url); free(r->headers); free(r->body);
    free(r->status_text); free(r->resp_headers); free(r->resp_body);
    memset(r, 0, sizeof(*r));
}

static JSValue js_http_start(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2) return jsglq_throw(ctx, "httpStart(method, url, headers, body)");

    int slot = req_alloc();
    if (slot < 0) return jsglq_throw(ctx, "too many concurrent requests (max %d)",
                                     MAX_REQUESTS);
    Req *r = &g_reqs[slot];
    memset(r, 0, sizeof(*r));
    r->used = 1;

    const char *method = JS_ToCString(ctx, argv[0]);
    const char *url = JS_ToCString(ctx, argv[1]);
    if (!method || !url) {
        if (method) JS_FreeCString(ctx, method);
        if (url) JS_FreeCString(ctx, url);
        r->used = 0;
        return JS_EXCEPTION;
    }
    r->method = dupstr(method);
    r->url = dupstr(url);
    JS_FreeCString(ctx, method);
    JS_FreeCString(ctx, url);

    if (argc > 2 && JS_IsString(argv[2])) {
        const char *h = JS_ToCString(ctx, argv[2]);
        if (h) { r->headers = dupstr(h); JS_FreeCString(ctx, h); }
    }
    if (argc > 3 && !JS_IsUndefined(argv[3]) && !JS_IsNull(argv[3])) {
        size_t blen = 0;
        /* A typed array is a binary body; anything else is stringified. */
        size_t off = 0, vlen = 0, bpe = 0;
        JSValue abuf = JS_GetTypedArrayBuffer(ctx, argv[3], &off, &vlen, &bpe);
        if (!JS_IsException(abuf)) {
            size_t total = 0;
            uint8_t *base = JS_GetArrayBuffer(ctx, &total, abuf);
            JS_FreeValue(ctx, abuf);
            if (base) {
                r->body = (char *)malloc(vlen ? vlen : 1);
                if (r->body) { memcpy(r->body, base + off, vlen); r->body_len = vlen; }
            }
        } else {
            JS_FreeValue(ctx, JS_GetException(ctx));
            const char *b = JS_ToCStringLen(ctx, &blen, argv[3]);
            if (b) {
                r->body = (char *)malloc(blen + 1);
                if (r->body) { memcpy(r->body, b, blen); r->body[blen] = 0; r->body_len = blen; }
                JS_FreeCString(ctx, b);
            }
        }
    }

    r->thread = SDL_CreateThread(http_thread, "jsglq-http", r);
    if (!r->thread) {
        req_free(r);
        return jsglq_throw(ctx, "cannot start request thread");
    }
    return JS_NewInt32(ctx, slot);
}

/* Returns null while in flight, otherwise the finished response. */
static JSValue js_http_poll(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return jsglq_throw(ctx, "httpPoll(id)");
    int32_t id = 0; JS_ToInt32(ctx, &id, argv[0]);
    if (id < 0 || id >= MAX_REQUESTS || !g_reqs[id].used) return JS_NULL;

    Req *r = &g_reqs[id];
    if (!r->done) return JS_NULL;

    SDL_WaitThread(r->thread, NULL);
    r->thread = NULL;

    JSValue o = JS_NewObject(ctx);
    if (r->failed) {
        JS_SetPropertyStr(ctx, o, "ok", JS_FALSE);
        JS_SetPropertyStr(ctx, o, "error", JS_NewString(ctx, r->err));
    } else {
        JS_SetPropertyStr(ctx, o, "ok", JS_TRUE);
        JS_SetPropertyStr(ctx, o, "status", JS_NewInt32(ctx, r->status));
        JS_SetPropertyStr(ctx, o, "statusText",
                          JS_NewString(ctx, r->status_text ? r->status_text : ""));
        JS_SetPropertyStr(ctx, o, "headers",
                          JS_NewString(ctx, r->resp_headers ? r->resp_headers : ""));
        JS_SetPropertyStr(ctx, o, "body",
            JS_NewArrayBufferCopy(ctx, (const uint8_t *)(r->resp_body ? r->resp_body : ""),
                                  r->resp_len));
    }
    req_free(r);
    return o;
}

int jsglq_bind_http(JsglqEngine *e)
{
    JSContext *ctx = jsglq_engine_ctx(e);
    JSValue global = JS_GetGlobalObject(ctx);

    JSValue h = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, h, "start", JS_NewCFunction(ctx, js_http_start, "start", 4));
    JS_SetPropertyStr(ctx, h, "poll",  JS_NewCFunction(ctx, js_http_poll, "poll", 1));
    JS_SetPropertyStr(ctx, h, "tls",   JS_NewBool(ctx, jsglq_tls_available()));
    JS_SetPropertyStr(ctx, global, "__jsglq_http", h);

    JS_FreeValue(ctx, global);
    return 0;
}

void jsglq_http_shutdown(void)
{
    for (int i = 0; i < MAX_REQUESTS; i++) {
        Req *r = &g_reqs[i];
        if (!r->used) continue;
        if (r->thread) { SDL_WaitThread(r->thread, NULL); r->thread = NULL; }
        req_free(r);
    }
}
