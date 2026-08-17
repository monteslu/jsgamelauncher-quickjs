/*
 * One stream abstraction over plaintext TCP and TLS, so the WebSocket and HTTP
 * code above it is written once. See net_tls.c for why TLS is mbedTLS rather
 * than each platform's native API.
 */
#ifndef JSGLQ_NET_TLS_H
#define JSGLQ_NET_TLS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JsglqStream JsglqStream;

/* Connect, optionally wrapping the connection in TLS. Returns NULL on failure
   and fills `err` with a reason meant to be shown to a developer — "TLS
   certificate rejected: ..." rather than a numeric code. */
JsglqStream *jsglq_stream_connect(const char *host, int port, int secure,
                                  char *err, size_t errlen);

/* Set the read timeout in milliseconds. The default is long (a stalled server
   should eventually fail), but a caller that must stay responsive to a stop
   flag — a WebSocket reader waiting on an idle connection — needs a short one
   so its loop can re-check. */
void jsglq_stream_set_timeout(JsglqStream *s, int ms);

/* Returns bytes transferred, or -1. send() writes all of `len` or fails. */
int  jsglq_stream_send(JsglqStream *s, const void *data, int len);
/* Returns bytes read, 0 at clean end of stream, -1 on error. */
int  jsglq_stream_recv(JsglqStream *s, void *buf, int len);
void jsglq_stream_close(JsglqStream *s);

/* Whether this build can do TLS at all, so callers can refuse https:// and
   wss:// by name instead of failing obscurely at connect time. */
int  jsglq_tls_available(void);

#ifdef __cplusplus
}
#endif

#endif /* JSGLQ_NET_TLS_H */
