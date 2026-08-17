/*
 * WebSocket (RFC 6455) over SDL_net.
 *
 * SDL_net rather than raw sockets so there is no Winsock/BSD split to maintain:
 * SDLNet_TCP_Open / Send / Recv behave identically on all six targets, and the
 * library is packaged everywhere we build.
 *
 * ws:// ONLY. wss:// needs TLS, which would mean vendoring a crypto library and
 * a certificate store; a WebSocket that silently downgraded to plaintext would
 * be far worse than one that refuses, so wss:// throws by name.
 *
 * Threading: each connection owns a thread that blocks in SDLNet_TCP_Recv and
 * pushes decoded messages onto a queue. The JS side drains that queue once per
 * frame. Doing it inline instead would either block the frame loop on recv or
 * require a non-blocking state machine for a protocol that is naturally
 * stream-shaped.
 */
#include "host.h"

#ifdef JSGLQ_HAVE_WEBSOCKET

#include <SDL2/SDL.h>
#include <SDL2/SDL_net.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define MAX_SOCKETS      8
#define RECV_CHUNK       4096
/* A frame larger than this is refused rather than allocated. Games exchange
   game state, not media; an unbounded length field from the network is an
   obvious way to be told to allocate 2 GB. */
#define MAX_FRAME_BYTES  (8u << 20)

typedef struct Msg {
    struct Msg *next;
    char       *data;      /* NUL-terminated for text; length carried separately */
    size_t      len;
    int         is_binary;
} Msg;

typedef enum {
    WS_CONNECTING = 0,
    WS_OPEN       = 1,
    WS_CLOSING    = 2,
    WS_CLOSED     = 3
} WsState;

typedef struct {
    int          used;
    TCPsocket    sock;
    SDL_Thread  *thread;
    SDL_mutex   *lock;
    volatile int state;          /* WsState; read by JS, written by the thread */
    volatile int stop;           /* set by JS to ask the thread to exit */
    char         host[256];
    char         path[512];
    int          port;
    char         err[256];       /* failure reason, for the error event */

    Msg         *head, *tail;    /* inbound queue, guarded by lock */

    /* Outbound frames are written directly from the JS thread under the lock:
       SDLNet_TCP_Send is blocking, but a send that would block is bounded by
       the socket buffer and games send small messages. */
} Ws;

static Ws  g_ws[MAX_SOCKETS];
static int g_net_ready = 0;

/* ------------------------------------------------------------------ base64 -- */

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void base64_encode(const unsigned char *in, size_t len, char *out)
{
    size_t i = 0, o = 0;
    for (; i + 2 < len; i += 3) {
        unsigned v = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
        out[o++] = B64[(v >> 18) & 63]; out[o++] = B64[(v >> 12) & 63];
        out[o++] = B64[(v >> 6) & 63];  out[o++] = B64[v & 63];
    }
    if (i < len) {
        unsigned v = in[i] << 16;
        int rem = (int)(len - i);
        if (rem == 2) v |= in[i + 1] << 8;
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = rem == 2 ? B64[(v >> 6) & 63] : '=';
        out[o++] = '=';
    }
    out[o] = 0;
}

/* ------------------------------------------------------------------- queue -- */

static void queue_push(Ws *w, const char *data, size_t len, int is_binary)
{
    Msg *m = (Msg *)calloc(1, sizeof(Msg));
    if (!m) return;
    m->data = (char *)malloc(len + 1);
    if (!m->data) { free(m); return; }
    memcpy(m->data, data, len);
    m->data[len] = 0;
    m->len = len;
    m->is_binary = is_binary;

    SDL_LockMutex(w->lock);
    if (w->tail) w->tail->next = m; else w->head = m;
    w->tail = m;
    SDL_UnlockMutex(w->lock);
}

static void queue_clear(Ws *w)
{
    SDL_LockMutex(w->lock);
    Msg *m = w->head;
    while (m) { Msg *n = m->next; free(m->data); free(m); m = n; }
    w->head = w->tail = NULL;
    SDL_UnlockMutex(w->lock);
}

/* ------------------------------------------------------------- frame codec -- */

/*
 * Write one client frame. Client frames MUST be masked (RFC 6455 §5.3); a
 * server is required to close the connection on an unmasked client frame, which
 * presents as a connection that opens and immediately dies.
 */
static int ws_send_frame(Ws *w, int opcode, const unsigned char *payload, size_t len)
{
    unsigned char header[14];
    size_t hlen = 0;
    header[hlen++] = (unsigned char)(0x80 | (opcode & 0x0F));   /* FIN + opcode */

    if (len < 126) {
        header[hlen++] = (unsigned char)(0x80 | len);
    } else if (len <= 0xFFFF) {
        header[hlen++] = 0x80 | 126;
        header[hlen++] = (unsigned char)((len >> 8) & 0xFF);
        header[hlen++] = (unsigned char)(len & 0xFF);
    } else {
        header[hlen++] = 0x80 | 127;
        for (int i = 7; i >= 0; i--)
            header[hlen++] = (unsigned char)((((uint64_t)len) >> (i * 8)) & 0xFF);
    }

    unsigned char mask[4];
    for (int i = 0; i < 4; i++) mask[i] = (unsigned char)(rand() & 0xFF);
    memcpy(header + hlen, mask, 4);
    hlen += 4;

    int ok = 1;
    SDL_LockMutex(w->lock);
    if (SDLNet_TCP_Send(w->sock, header, (int)hlen) != (int)hlen) ok = 0;
    if (ok && len) {
        unsigned char buf[1024];
        size_t sent = 0;
        while (sent < len && ok) {
            size_t chunk = len - sent;
            if (chunk > sizeof(buf)) chunk = sizeof(buf);
            for (size_t i = 0; i < chunk; i++)
                buf[i] = payload[sent + i] ^ mask[(sent + i) & 3];
            if (SDLNet_TCP_Send(w->sock, buf, (int)chunk) != (int)chunk) ok = 0;
            sent += chunk;
        }
    }
    SDL_UnlockMutex(w->lock);
    return ok;
}

/* Read exactly n bytes, or fail. SDLNet_TCP_Recv returns short reads. */
static int recv_exact(TCPsocket s, unsigned char *buf, size_t n, volatile int *stop)
{
    size_t got = 0;
    while (got < n) {
        if (stop && *stop) return 0;
        int r = SDLNet_TCP_Recv(s, buf + got, (int)(n - got));
        if (r <= 0) return 0;
        got += (size_t)r;
    }
    return 1;
}

/* ---------------------------------------------------------------- handshake -- */

static int ws_handshake(Ws *w)
{
    /* 16 random bytes, base64'd. The server's Sec-WebSocket-Accept is a SHA-1 of
       this plus a fixed GUID; verifying it needs SHA-1, and skipping the check
       only costs us detection of a badly-behaved server, not security (there is
       no TLS here either way). We still send a correct, unique key. */
    unsigned char nonce[16];
    for (int i = 0; i < 16; i++) nonce[i] = (unsigned char)(rand() & 0xFF);
    char key[32];
    base64_encode(nonce, sizeof(nonce), key);

    char req[1200];
    int n = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n",
        w->path, w->host, w->port, key);
    if (n <= 0 || n >= (int)sizeof(req)) {
        snprintf(w->err, sizeof(w->err), "request too long");
        return 0;
    }
    if (SDLNet_TCP_Send(w->sock, req, n) != n) {
        snprintf(w->err, sizeof(w->err), "failed to send handshake");
        return 0;
    }

    /* Read headers one byte at a time up to the blank line. Slow, but it is
       exactly once per connection and avoids buffering past the header into
       frame data — which would silently drop the first frame. */
    char resp[2048];
    size_t len = 0;
    while (len + 1 < sizeof(resp)) {
        if (w->stop) return 0;
        int r = SDLNet_TCP_Recv(w->sock, resp + len, 1);
        if (r <= 0) { snprintf(w->err, sizeof(w->err), "connection closed during handshake"); return 0; }
        len++;
        if (len >= 4 && memcmp(resp + len - 4, "\r\n\r\n", 4) == 0) break;
    }
    resp[len] = 0;

    if (strncmp(resp, "HTTP/1.1 101", 12) != 0 && strncmp(resp, "HTTP/1.0 101", 12) != 0) {
        /* Report the status line, which is what tells a developer whether they
           hit the wrong path or a plain HTTP server. */
        char *eol = strstr(resp, "\r\n");
        int slen = eol ? (int)(eol - resp) : (int)len;
        if (slen > 120) slen = 120;
        snprintf(w->err, sizeof(w->err), "handshake failed: %.*s", slen, resp);
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ thread -- */

static int ws_thread(void *data)
{
    Ws *w = (Ws *)data;

    IPaddress ip;
    if (SDLNet_ResolveHost(&ip, w->host, (Uint16)w->port) < 0) {
        snprintf(w->err, sizeof(w->err), "cannot resolve %s", w->host);
        w->state = WS_CLOSED;
        return 0;
    }
    w->sock = SDLNet_TCP_Open(&ip);
    if (!w->sock) {
        snprintf(w->err, sizeof(w->err), "cannot connect to %s:%d", w->host, w->port);
        w->state = WS_CLOSED;
        return 0;
    }
    if (!ws_handshake(w)) {
        SDLNet_TCP_Close(w->sock);
        w->sock = NULL;
        w->state = WS_CLOSED;
        return 0;
    }

    w->state = WS_OPEN;

    unsigned char *frag = NULL;      /* accumulated continuation payload */
    size_t frag_len = 0;
    int frag_binary = 0;

    while (!w->stop) {
        unsigned char h[2];
        if (!recv_exact(w->sock, h, 2, &w->stop)) break;

        int fin    = (h[0] & 0x80) != 0;
        int opcode =  h[0] & 0x0F;
        int masked = (h[1] & 0x80) != 0;
        uint64_t plen = h[1] & 0x7F;

        if (plen == 126) {
            unsigned char e[2];
            if (!recv_exact(w->sock, e, 2, &w->stop)) break;
            plen = ((uint64_t)e[0] << 8) | e[1];
        } else if (plen == 127) {
            unsigned char e[8];
            if (!recv_exact(w->sock, e, 8, &w->stop)) break;
            plen = 0;
            for (int i = 0; i < 8; i++) plen = (plen << 8) | e[i];
        }
        if (plen > MAX_FRAME_BYTES) {
            snprintf(w->err, sizeof(w->err), "frame too large (%llu bytes)",
                     (unsigned long long)plen);
            break;
        }

        unsigned char mask[4] = {0,0,0,0};
        if (masked && !recv_exact(w->sock, mask, 4, &w->stop)) break;

        unsigned char *payload = NULL;
        if (plen) {
            payload = (unsigned char *)malloc((size_t)plen + 1);
            if (!payload) break;
            if (!recv_exact(w->sock, payload, (size_t)plen, &w->stop)) { free(payload); break; }
            if (masked) for (uint64_t i = 0; i < plen; i++) payload[i] ^= mask[i & 3];
            payload[plen] = 0;
        }

        if (opcode == 0x8) {                    /* close */
            free(payload);
            w->state = WS_CLOSING;
            ws_send_frame(w, 0x8, NULL, 0);     /* echo the close */
            break;
        } else if (opcode == 0x9) {             /* ping -> pong */
            ws_send_frame(w, 0xA, payload, (size_t)plen);
            free(payload);
            continue;
        } else if (opcode == 0xA) {             /* pong */
            free(payload);
            continue;
        }

        if (opcode == 0x1 || opcode == 0x2) {   /* text / binary */
            frag_binary = (opcode == 0x2);
            if (fin) {
                queue_push(w, payload ? (char *)payload : "", (size_t)plen, frag_binary);
                free(payload);
                continue;
            }
            free(frag);
            frag = payload;                     /* start a fragmented message */
            frag_len = (size_t)plen;
            continue;
        }

        if (opcode == 0x0) {                    /* continuation */
            unsigned char *bigger = (unsigned char *)realloc(frag, frag_len + (size_t)plen + 1);
            if (!bigger) { free(payload); break; }
            frag = bigger;
            if (plen) memcpy(frag + frag_len, payload, (size_t)plen);
            frag_len += (size_t)plen;
            frag[frag_len] = 0;
            free(payload);
            if (fin) {
                queue_push(w, (char *)frag, frag_len, frag_binary);
                free(frag); frag = NULL; frag_len = 0;
            }
            continue;
        }

        free(payload);                          /* unknown opcode: ignore */
    }

    free(frag);
    if (w->sock) { SDLNet_TCP_Close(w->sock); w->sock = NULL; }
    w->state = WS_CLOSED;
    return 0;
}

/* --------------------------------------------------------------------- JS --- */

static int slot_alloc(void)
{
    for (int i = 0; i < MAX_SOCKETS; i++) if (!g_ws[i].used) return i;
    return -1;
}

static JSValue js_ws_connect(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 3) return jsglq_throw(ctx, "wsConnect(host, port, path)");

    if (!g_net_ready) {
        if (SDLNet_Init() < 0)
            return jsglq_throw(ctx, "SDLNet_Init failed: %s", SDLNet_GetError());
        g_net_ready = 1;
    }

    int slot = slot_alloc();
    if (slot < 0) return jsglq_throw(ctx, "too many open WebSockets (max %d)", MAX_SOCKETS);

    const char *host = JS_ToCString(ctx, argv[0]);
    if (!host) return JS_EXCEPTION;
    int32_t port = 80;
    JS_ToInt32(ctx, &port, argv[1]);
    const char *path = JS_ToCString(ctx, argv[2]);
    if (!path) { JS_FreeCString(ctx, host); return JS_EXCEPTION; }

    Ws *w = &g_ws[slot];
    memset(w, 0, sizeof(*w));
    w->used  = 1;
    w->state = WS_CONNECTING;
    w->port  = port;
    snprintf(w->host, sizeof(w->host), "%s", host);
    snprintf(w->path, sizeof(w->path), "%s", path);
    JS_FreeCString(ctx, host);
    JS_FreeCString(ctx, path);

    w->lock = SDL_CreateMutex();
    if (!w->lock) { w->used = 0; return jsglq_throw(ctx, "cannot create socket mutex"); }

    w->thread = SDL_CreateThread(ws_thread, "jsglq-ws", w);
    if (!w->thread) {
        SDL_DestroyMutex(w->lock);
        w->used = 0;
        return jsglq_throw(ctx, "cannot start socket thread");
    }
    return JS_NewInt32(ctx, slot);
}

static JSValue js_ws_state(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return jsglq_throw(ctx, "wsState(id)");
    int32_t id = 0; JS_ToInt32(ctx, &id, argv[0]);
    if (id < 0 || id >= MAX_SOCKETS || !g_ws[id].used) return JS_NewInt32(ctx, WS_CLOSED);
    return JS_NewInt32(ctx, g_ws[id].state);
}

static JSValue js_ws_error(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return jsglq_throw(ctx, "wsError(id)");
    int32_t id = 0; JS_ToInt32(ctx, &id, argv[0]);
    if (id < 0 || id >= MAX_SOCKETS || !g_ws[id].used) return JS_NULL;
    if (!g_ws[id].err[0]) return JS_NULL;
    return JS_NewString(ctx, g_ws[id].err);
}

/* Drain the inbound queue. Returns an array of {data, binary}. */
static JSValue js_ws_recv(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return jsglq_throw(ctx, "wsRecv(id)");
    int32_t id = 0; JS_ToInt32(ctx, &id, argv[0]);
    JSValue arr = JS_NewArray(ctx);
    if (id < 0 || id >= MAX_SOCKETS || !g_ws[id].used) return arr;

    Ws *w = &g_ws[id];
    SDL_LockMutex(w->lock);
    Msg *m = w->head;
    w->head = w->tail = NULL;
    SDL_UnlockMutex(w->lock);

    uint32_t n = 0;
    while (m) {
        Msg *next = m->next;
        JSValue o = JS_NewObject(ctx);
        if (m->is_binary) {
            JSValue ab = JS_NewArrayBufferCopy(ctx, (const uint8_t *)m->data, m->len);
            JS_SetPropertyStr(ctx, o, "data", ab);
        } else {
            JS_SetPropertyStr(ctx, o, "data", JS_NewStringLen(ctx, m->data, m->len));
        }
        JS_SetPropertyStr(ctx, o, "binary", JS_NewBool(ctx, m->is_binary));
        JS_SetPropertyUint32(ctx, arr, n++, o);
        free(m->data); free(m);
        m = next;
    }
    return arr;
}

static JSValue js_ws_send(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2) return jsglq_throw(ctx, "wsSend(id, data)");
    int32_t id = 0; JS_ToInt32(ctx, &id, argv[0]);
    if (id < 0 || id >= MAX_SOCKETS || !g_ws[id].used)
        return jsglq_throw(ctx, "wsSend: socket %d is not open", id);
    Ws *w = &g_ws[id];
    if (w->state != WS_OPEN)
        return jsglq_throw(ctx, "wsSend: socket is not open (state %d)", w->state);

    /* Binary when handed an ArrayBuffer or a view over one, text otherwise. */
    size_t off = 0, blen = 0, bpe = 0;
    JSValue abuf = JS_GetTypedArrayBuffer(ctx, argv[1], &off, &blen, &bpe);
    if (!JS_IsException(abuf)) {
        size_t total = 0;
        uint8_t *base = JS_GetArrayBuffer(ctx, &total, abuf);
        JS_FreeValue(ctx, abuf);
        if (!base) return jsglq_throw(ctx, "wsSend: cannot read buffer");
        int ok = ws_send_frame(w, 0x2, base + off, blen);
        return ok ? JS_TRUE : JS_FALSE;
    }
    JS_FreeValue(ctx, JS_GetException(ctx));   /* returned value; must be freed */

    size_t len = 0;
    const char *str = JS_ToCStringLen(ctx, &len, argv[1]);
    if (!str) return JS_EXCEPTION;
    int ok = ws_send_frame(w, 0x1, (const unsigned char *)str, len);
    JS_FreeCString(ctx, str);
    return ok ? JS_TRUE : JS_FALSE;
}

static JSValue js_ws_close(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return jsglq_throw(ctx, "wsClose(id)");
    int32_t id = 0; JS_ToInt32(ctx, &id, argv[0]);
    if (id < 0 || id >= MAX_SOCKETS || !g_ws[id].used) return JS_UNDEFINED;

    Ws *w = &g_ws[id];
    if (w->state == WS_OPEN) {
        w->state = WS_CLOSING;
        ws_send_frame(w, 0x8, NULL, 0);
    }
    w->stop = 1;
    /* Closing the socket unblocks the thread's recv, which is otherwise parked
       until the peer says something. */
    if (w->sock) SDLNet_TCP_Close(w->sock);
    if (w->thread) { SDL_WaitThread(w->thread, NULL); w->thread = NULL; }
    queue_clear(w);
    if (w->lock) { SDL_DestroyMutex(w->lock); w->lock = NULL; }
    w->sock = NULL;
    w->used = 0;
    w->state = WS_CLOSED;
    return JS_UNDEFINED;
}

void jsglq_websocket_shutdown(void)
{
    for (int i = 0; i < MAX_SOCKETS; i++) {
        Ws *w = &g_ws[i];
        if (!w->used) continue;
        w->stop = 1;
        if (w->sock) SDLNet_TCP_Close(w->sock);
        if (w->thread) { SDL_WaitThread(w->thread, NULL); w->thread = NULL; }
        queue_clear(w);
        if (w->lock) { SDL_DestroyMutex(w->lock); w->lock = NULL; }
        w->used = 0;
    }
    if (g_net_ready) { SDLNet_Quit(); g_net_ready = 0; }
}

int jsglq_bind_websocket(JsglqEngine *e)
{
    JSContext *ctx = jsglq_engine_ctx(e);
    JSValue global = JS_GetGlobalObject(ctx);

    JSValue w = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, w, "connect", JS_NewCFunction(ctx, js_ws_connect, "connect", 3));
    JS_SetPropertyStr(ctx, w, "state",   JS_NewCFunction(ctx, js_ws_state, "state", 1));
    JS_SetPropertyStr(ctx, w, "error",   JS_NewCFunction(ctx, js_ws_error, "error", 1));
    JS_SetPropertyStr(ctx, w, "recv",    JS_NewCFunction(ctx, js_ws_recv, "recv", 1));
    JS_SetPropertyStr(ctx, w, "send",    JS_NewCFunction(ctx, js_ws_send, "send", 2));
    JS_SetPropertyStr(ctx, w, "close",   JS_NewCFunction(ctx, js_ws_close, "close", 1));
    JS_SetPropertyStr(ctx, global, "__jsglq_ws", w);

    JS_FreeValue(ctx, global);
    return 0;
}

#else  /* !JSGLQ_HAVE_WEBSOCKET */

int jsglq_bind_websocket(JsglqEngine *e)
{
    /* Built without SDL_net. The JS shim sees no __jsglq_ws and leaves
       WebSocket undefined, so a game can feature-detect it — which is the
       honest answer, and better than a constructor that never connects. */
    (void)e;
    return 0;
}

void jsglq_websocket_shutdown(void) {}

#endif
