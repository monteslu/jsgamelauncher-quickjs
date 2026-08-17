/*
 * A stream transport that is either plaintext TCP or TLS.
 *
 * Both WebSocket (ws:// and wss://) and fetch (http:// and https://) need the
 * same thing: connect, write, read, close, where "read" and "write" may or may
 * not be encrypted. Putting that behind one struct means the protocol code above
 * is written once and neither implementation grows an `if (secure)` in the
 * middle of its frame handling.
 *
 * TLS is mbedTLS rather than each platform's native API. SChannel and Secure
 * Transport have nothing in common with each other, Secure Transport is
 * deprecated, and maintaining three implementations of the single most
 * security-sensitive code in the project is how subtle differences appear
 * between platforms. mbedTLS is packaged for all three CI package managers.
 *
 * Certificate verification is ON. A TLS client that does not verify is barely
 * better than plaintext while looking secure to the caller, so a verification
 * failure is a hard error with a readable reason rather than a warning.
 */
#include "host.h"
#include "net_tls.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef JSGLQ_HAVE_TLS
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/error.h>
#endif

struct JsglqStream {
    int secure;

#ifdef JSGLQ_HAVE_TLS
    mbedtls_net_context      net;
    mbedtls_ssl_context      ssl;
    mbedtls_ssl_config       conf;
    mbedtls_x509_crt         cacert;
    mbedtls_entropy_context  entropy;
    mbedtls_ctr_drbg_context drbg;
    int                      tls_ready;
#endif
};

#ifdef JSGLQ_HAVE_TLS
/*
 * Where the system keeps its trust store.
 *
 * Shipping our own CA bundle would mean shipping a list that goes stale and has
 * to be re-cut on every release; the OS already maintains one. macOS keychain
 * certs are not on disk in PEM form, so the Homebrew/OpenSSL bundle is the
 * practical source there.
 */
static const char *CA_PATHS[] = {
    "/etc/ssl/certs/ca-certificates.crt",         /* Debian, Ubuntu, Alpine */
    "/etc/pki/tls/certs/ca-bundle.crt",           /* Fedora, RHEL */
    "/etc/ssl/ca-bundle.pem",                     /* openSUSE */
    "/etc/ssl/cert.pem",                          /* macOS (LibreSSL), BSD */
    "/opt/homebrew/etc/ca-certificates/cert.pem", /* Homebrew, Apple silicon */
    "/usr/local/etc/ca-certificates/cert.pem",    /* Homebrew, Intel */
    NULL
};

static int load_ca_certs(mbedtls_x509_crt *cacert, char *err, size_t errlen)
{
    for (int i = 0; CA_PATHS[i]; i++) {
        if (mbedtls_x509_crt_parse_file(cacert, CA_PATHS[i]) == 0) return 1;
    }
    /* A directory of individual certs, which is how some systems store them. */
    if (mbedtls_x509_crt_parse_path(cacert, "/etc/ssl/certs") == 0) return 1;

    snprintf(err, errlen,
             "no CA certificate bundle found; cannot verify TLS certificates");
    return 0;
}
#endif

JsglqStream *jsglq_stream_connect(const char *host, int port, int secure,
                                  char *err, size_t errlen)
{
    if (err && errlen) err[0] = 0;

    if (secure) {
#ifndef JSGLQ_HAVE_TLS
        snprintf(err, errlen,
                 "TLS is not available in this build (rebuild with mbedTLS)");
        return NULL;
#else
        JsglqStream *s = (JsglqStream *)calloc(1, sizeof(JsglqStream));
        if (!s) { snprintf(err, errlen, "out of memory"); return NULL; }
        s->secure = 1;

        mbedtls_net_init(&s->net);
        mbedtls_ssl_init(&s->ssl);
        mbedtls_ssl_config_init(&s->conf);
        mbedtls_x509_crt_init(&s->cacert);
        mbedtls_entropy_init(&s->entropy);
        mbedtls_ctr_drbg_init(&s->drbg);

        const char *pers = "jsglq";
        if (mbedtls_ctr_drbg_seed(&s->drbg, mbedtls_entropy_func, &s->entropy,
                                  (const unsigned char *)pers, strlen(pers)) != 0) {
            snprintf(err, errlen, "cannot seed random number generator");
            jsglq_stream_close(s);
            return NULL;
        }
        if (!load_ca_certs(&s->cacert, err, errlen)) {
            jsglq_stream_close(s);
            return NULL;
        }

        char portstr[16];
        snprintf(portstr, sizeof(portstr), "%d", port);
        if (mbedtls_net_connect(&s->net, host, portstr, MBEDTLS_NET_PROTO_TCP) != 0) {
            snprintf(err, errlen, "cannot connect to %s:%d", host, port);
            jsglq_stream_close(s);
            return NULL;
        }
        if (mbedtls_ssl_config_defaults(&s->conf, MBEDTLS_SSL_IS_CLIENT,
                                        MBEDTLS_SSL_TRANSPORT_STREAM,
                                        MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
            snprintf(err, errlen, "cannot configure TLS");
            jsglq_stream_close(s);
            return NULL;
        }
        /* REQUIRED, not OPTIONAL: an unverified certificate must fail the
           connection, not merely be noted. */
        mbedtls_ssl_conf_authmode(&s->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&s->conf, &s->cacert, NULL);
        mbedtls_ssl_conf_rng(&s->conf, mbedtls_ctr_drbg_random, &s->drbg);

        if (mbedtls_ssl_setup(&s->ssl, &s->conf) != 0) {
            snprintf(err, errlen, "cannot set up TLS session");
            jsglq_stream_close(s);
            return NULL;
        }
        /* SNI, and the name checked against the certificate. Skipping this is
           how a client ends up accepting any valid certificate for any host. */
        if (mbedtls_ssl_set_hostname(&s->ssl, host) != 0) {
            snprintf(err, errlen, "cannot set TLS hostname");
            jsglq_stream_close(s);
            return NULL;
        }
        mbedtls_ssl_set_bio(&s->ssl, &s->net, mbedtls_net_send, mbedtls_net_recv, NULL);

        int rc;
        while ((rc = mbedtls_ssl_handshake(&s->ssl)) != 0) {
            if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
                char buf[128];
                mbedtls_strerror(rc, buf, sizeof(buf));
                uint32_t flags = mbedtls_ssl_get_verify_result(&s->ssl);
                if (flags) {
                    char vbuf[512];
                    mbedtls_x509_crt_verify_info(vbuf, sizeof(vbuf), "", flags);
                    /* Trim the trailing newline mbedtls includes. */
                    size_t vl = strlen(vbuf);
                    while (vl && (vbuf[vl-1] == '\n' || vbuf[vl-1] == '\r')) vbuf[--vl] = 0;
                    snprintf(err, errlen, "TLS certificate rejected: %s", vbuf);
                } else {
                    snprintf(err, errlen, "TLS handshake failed: %s", buf);
                }
                jsglq_stream_close(s);
                return NULL;
            }
        }
        s->tls_ready = 1;
        return s;
#endif
    }

#ifndef JSGLQ_HAVE_TLS
    snprintf(err, errlen, "networking is not available in this build");
    return NULL;
#else
    /*
     * Plaintext uses mbedTLS's socket layer too, NOT SDL_net.
     *
     * SDL_net's TCP_Recv blocks until it has the full requested length, and its
     * SDLNet_CheckSockets never returned at all when the set was polled from the
     * worker thread — a request simply hung forever with no error. mbedTLS ships
     * mbedtls_net_recv_timeout, which does a PARTIAL read with a real timeout,
     * which is the semantics an HTTP or WebSocket reader actually needs. Using it
     * for both modes also means one socket implementation instead of two.
     */
    JsglqStream *s = (JsglqStream *)calloc(1, sizeof(JsglqStream));
    if (!s) { snprintf(err, errlen, "out of memory"); return NULL; }
    s->secure = 0;
    mbedtls_net_init(&s->net);

    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", port);
    if (mbedtls_net_connect(&s->net, host, portstr, MBEDTLS_NET_PROTO_TCP) != 0) {
        snprintf(err, errlen, "cannot connect to %s:%d", host, port);
        free(s);
        return NULL;
    }
    return s;
#endif
}

int jsglq_stream_send(JsglqStream *s, const void *data, int len)
{
    if (!s || len <= 0) return 0;
#ifdef JSGLQ_HAVE_TLS
    if (s->secure) {
        int sent = 0;
        while (sent < len) {
            int rc = mbedtls_ssl_write(&s->ssl, (const unsigned char *)data + sent,
                                       (size_t)(len - sent));
            if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            if (rc <= 0) return sent > 0 ? sent : -1;
            sent += rc;
        }
        return sent;
    }
#endif
#ifdef JSGLQ_HAVE_TLS
    {
        int sent = 0;
        while (sent < len) {
            int rc = mbedtls_net_send(&s->net, (const unsigned char *)data + sent,
                                      (size_t)(len - sent));
            if (rc == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            if (rc <= 0) return sent > 0 ? sent : -1;
            sent += rc;
        }
        return sent;
    }
#else
    (void)data; return -1;
#endif
}

int jsglq_stream_recv(JsglqStream *s, void *buf, int len)
{
    if (!s || len <= 0) return 0;
#ifdef JSGLQ_HAVE_TLS
    if (s->secure) {
        for (;;) {
            int rc = mbedtls_ssl_read(&s->ssl, (unsigned char *)buf, (size_t)len);
            if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            /* A clean TLS close is end-of-stream, not an error: an HTTP/1.0
               response body can be delimited by exactly this. */
            if (rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
            return rc < 0 ? -1 : rc;
        }
    }
#endif
#ifdef JSGLQ_HAVE_TLS
    {
        /* Partial read with a timeout: returns as soon as ANY data is available
           rather than waiting for the full buffer, which is what an HTTP header
           reader and a WebSocket frame reader both need. */
        int rc = mbedtls_net_recv_timeout(&s->net, (unsigned char *)buf,
                                          (size_t)len, 30000);
        if (rc == MBEDTLS_ERR_SSL_TIMEOUT) return -1;
        if (rc == MBEDTLS_ERR_SSL_WANT_READ) return 0;
        return rc < 0 ? -1 : rc;
    }
#else
    (void)buf; return -1;
#endif
}

void jsglq_stream_close(JsglqStream *s)
{
    if (!s) return;
#ifdef JSGLQ_HAVE_TLS
    if (s->secure) {
        if (s->tls_ready) mbedtls_ssl_close_notify(&s->ssl);
        mbedtls_net_free(&s->net);
        mbedtls_ssl_free(&s->ssl);
        mbedtls_ssl_config_free(&s->conf);
        mbedtls_x509_crt_free(&s->cacert);
        mbedtls_ctr_drbg_free(&s->drbg);
        mbedtls_entropy_free(&s->entropy);
        free(s);
        return;
    }
#endif
#ifdef JSGLQ_HAVE_TLS
    mbedtls_net_free(&s->net);
#endif
    free(s);
}

int jsglq_tls_available(void)
{
#ifdef JSGLQ_HAVE_TLS
    return 1;
#else
    return 0;
#endif
}
