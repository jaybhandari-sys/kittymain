/* Provider (camera-side), real P2P SRT — multi-threaded redesign.
 *
 * The architecture has two independent threads:
 *
 *   1. SIGNALING THREAD (immortal):
 *      Owns the long-lived TCP connection to the cloud signaling server.
 *      Sends SRT_REGISTER on connect.  Reads SRT_PEER messages forever.
 *      When a new SRT_PEER arrives, posts the peer info into a slot and
 *      signals the pump thread to tear down any current session and start
 *      a fresh rendezvous to the new peer.  This thread NEVER blocks on
 *      pump or HTTP-FLV fetch — its only job is staying connected to the
 *      cloud and brokering new SRT requests.
 *
 *   2. PUMP THREAD (transient):
 *      Spawned on demand when signaling thread posts a peer.  Does the
 *      SRT rendezvous + HTTP-FLV fetch + pump loop.  Exits whenever:
 *        - SRT breaks (peer disconnected, network drop)
 *        - signaling thread requests a tear-down (new peer arrived)
 *        - g_shutdown set
 *
 * Critical invariant: the camera's signaling TCP and registration with the
 * cloud must be UNAFFECTED by SRT/pump events.  Phone close/reopen,
 * subprocess crashes, mobile-network blips on the phone — none of these
 * may cause the camera to disappear from the cloud's active-services list.
 */

#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "srt/srt.h"
#include "stun_lite.h"

/* ------------------------------------------------------------------------ */
/*  TLS (OpenSSL) — optional, compiled in by default; the legacy plain-TCP  */
/*  signaling path stays available via config (SIGNALING_SCHEME=plain).     */
/*                                                                          */
/*  We require openssl 3.x which the camera firmware already ships (see     */
/*  /mny/mtd/ipc/ambicam/libcrypto.so.1.1 symlink → /usr/lib/libcrypto.so.3 */
/*  in deploy/ambicam.sh).  No new library to vendor.                       */
/* ------------------------------------------------------------------------ */
#ifndef KITTY_NO_TLS
#define KITTY_TLS 1
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
#endif

/* ------------------------------------------------------------------------ */
/*  Configuration                                                           */
/* ------------------------------------------------------------------------ */

#define LOCAL_BUFFER_SIZE       (256 * 1024)
#define SRT_PAYLOAD_SIZE        1316
#define DEFAULT_LATENCY_MS      300
#define RECONNECT_MIN_MS        1000
#define RECONNECT_MAX_MS        5000
#define LONG_SESSION_S          30
#define CONNECT_TIMEOUT_MS      6000
#define PEER_IDLE_TIMEOUT_MS    8000   /* Was 30s; with the new cancel-on-new-peer
                                         * path the pump aborts immediately when
                                         * a new request arrives, so we no longer
                                         * need a long timeout to mask phone churn.
                                         * 8s still tolerates brief mobile blips. */
#define CONFIG_LINE_MAX         256

typedef struct {
    char     local_host[128];
    int      local_port;
    char     local_http_path[512];
    char     local_http_auth[256];
    char     service_id[256];
    char     signaling_host[128];
    int      signaling_port;
    char     api_token[128];
    char     stun_host[128];
    int      stun_port;
    char     turn_host[128];   /* Phase 3 fallback */
    int      turn_port;
    char     turn_username[128];
    char     turn_password[128];
    int      bind_port;
    int      latency_ms;
    int      verbose;

    /* TLS / mTLS for signaling (v1.13+).  When signaling_scheme=tls, the
     * signaling TCP is wrapped in TLS using openssl, and the device cert
     * is presented as the client cert (mTLS).  Server cert is verified
     * against the CA chain. */
    char     signaling_scheme[8];   /* "plain" (default) or "tls" */
    char     signaling_ca[256];     /* PEM bundle for verifying server cert */
    char     signaling_cert[256];   /* device cert (PEM) */
    char     signaling_key[256];    /* device private key (PEM) */
    int      signaling_verify_host; /* 1 = verify server CN/SAN matches host (default 1) */
} config_t;

static config_t g_cfg;
static volatile int g_shutdown = 0;

/* ------------------------------------------------------------------------ */
/*  Logging                                                                 */
/* ------------------------------------------------------------------------ */

static void log_msg(const char *level, const char *fmt, ...) {
    char ts[32];
    struct timespec now; clock_gettime(CLOCK_REALTIME, &now);
    struct tm tm; localtime_r(&now.tv_sec, &tm);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
    fprintf(stdout, "[%s.%03ld] [%s] ", ts, now.tv_nsec / 1000000, level);
    va_list ap; va_start(ap, fmt); vfprintf(stdout, fmt, ap); va_end(ap);
    fputc('\n', stdout); fflush(stdout);
}
#define LOGI(...) log_msg("INFO ", __VA_ARGS__)
#define LOGW(...) log_msg("WARN ", __VA_ARGS__)
#define LOGE(...) log_msg("ERROR", __VA_ARGS__)
#define LOGD(...) do { if (g_cfg.verbose) log_msg("DEBUG", __VA_ARGS__); } while (0)

/* ------------------------------------------------------------------------ */
/*  Config parser                                                           */
/* ------------------------------------------------------------------------ */

static char *strip(char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}

static int load_config(const char *path, config_t *cfg) {
    snprintf(cfg->local_host, sizeof(cfg->local_host), "127.0.0.1");
    cfg->local_port      = 80;
    cfg->signaling_port  = 8888;
    cfg->stun_port       = 3478;
    cfg->turn_port       = 3478;
    cfg->bind_port       = 0;
    cfg->latency_ms      = DEFAULT_LATENCY_MS;
    cfg->verbose         = 0;
    snprintf(cfg->signaling_scheme, sizeof(cfg->signaling_scheme), "plain");
    cfg->signaling_ca[0]            = 0;
    cfg->signaling_cert[0]          = 0;
    cfg->signaling_key[0]           = 0;
    cfg->signaling_verify_host      = 1;

    FILE *f = fopen(path, "r");
    if (!f) { LOGE("config: cannot open %s: %s", path, strerror(errno)); return -1; }
    char line[CONFIG_LINE_MAX]; int lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        char *p = strip(line);
        if (!*p || *p == '#' || *p == ';' || *p == '[') continue;
        char *eq = strchr(p, '='); if (!eq) continue;
        *eq = 0;
        char *k = strip(p); char *v = strip(eq + 1);
        if      (!strcasecmp(k, "LOCAL_HOST"))      snprintf(cfg->local_host,      sizeof(cfg->local_host),      "%s", v);
        else if (!strcasecmp(k, "LOCAL_PORT"))      cfg->local_port      = atoi(v);
        else if (!strcasecmp(k, "LOCAL_HTTP_PATH")) snprintf(cfg->local_http_path, sizeof(cfg->local_http_path), "%s", v);
        else if (!strcasecmp(k, "LOCAL_HTTP_AUTH")) snprintf(cfg->local_http_auth, sizeof(cfg->local_http_auth), "%s", v);
        else if (!strcasecmp(k, "SERVICE_ID"))      snprintf(cfg->service_id,      sizeof(cfg->service_id),      "%s", v);
        else if (!strcasecmp(k, "SIGNALING_HOST"))  snprintf(cfg->signaling_host,  sizeof(cfg->signaling_host),  "%s", v);
        else if (!strcasecmp(k, "SIGNALING_PORT"))  cfg->signaling_port  = atoi(v);
        else if (!strcasecmp(k, "API_TOKEN"))       snprintf(cfg->api_token,       sizeof(cfg->api_token),       "%s", v);
        else if (!strcasecmp(k, "STUN_HOST"))       snprintf(cfg->stun_host,       sizeof(cfg->stun_host),       "%s", v);
        else if (!strcasecmp(k, "STUN_PORT"))       cfg->stun_port       = atoi(v);
        else if (!strcasecmp(k, "TURN_HOST"))       snprintf(cfg->turn_host,       sizeof(cfg->turn_host),       "%s", v);
        else if (!strcasecmp(k, "TURN_PORT"))       cfg->turn_port       = atoi(v);
        else if (!strcasecmp(k, "TURN_USERNAME"))   snprintf(cfg->turn_username,   sizeof(cfg->turn_username),   "%s", v);
        else if (!strcasecmp(k, "TURN_PASSWORD"))   snprintf(cfg->turn_password,   sizeof(cfg->turn_password),   "%s", v);
        else if (!strcasecmp(k, "BIND_PORT"))       cfg->bind_port       = atoi(v);
        else if (!strcasecmp(k, "LATENCY_MS"))      cfg->latency_ms      = atoi(v);
        else if (!strcasecmp(k, "VERBOSE"))         cfg->verbose         = atoi(v);
        else if (!strcasecmp(k, "SIGNALING_SCHEME")) snprintf(cfg->signaling_scheme, sizeof(cfg->signaling_scheme), "%s", v);
        else if (!strcasecmp(k, "SIGNALING_CA"))     snprintf(cfg->signaling_ca,     sizeof(cfg->signaling_ca),     "%s", v);
        else if (!strcasecmp(k, "SIGNALING_CERT"))   snprintf(cfg->signaling_cert,   sizeof(cfg->signaling_cert),   "%s", v);
        else if (!strcasecmp(k, "SIGNALING_KEY"))    snprintf(cfg->signaling_key,    sizeof(cfg->signaling_key),    "%s", v);
        else if (!strcasecmp(k, "SIGNALING_VERIFY_HOST")) cfg->signaling_verify_host = atoi(v);
        else { LOGW("config %s:%d: unknown key '%s'", path, lineno, k); }
    }
    fclose(f);
    if (!cfg->signaling_host[0] || !cfg->stun_host[0] || !cfg->service_id[0]) {
        LOGE("config: SIGNALING_HOST, STUN_HOST, SERVICE_ID are required");
        return -1;
    }
    /* TLS scheme validation — if requested, all four bits must be present.
     * We don't fall back silently to plain: misconfigured mTLS that *thinks*
     * it's secure but isn't is worse than a hard failure. */
    if (!strcasecmp(cfg->signaling_scheme, "tls")) {
#ifndef KITTY_TLS
        LOGE("config: SIGNALING_SCHEME=tls but binary built without TLS (KITTY_NO_TLS defined)");
        return -1;
#else
        if (!cfg->signaling_ca[0])   { LOGE("config: SIGNALING_SCHEME=tls requires SIGNALING_CA"); return -1; }
        if (!cfg->signaling_cert[0]) { LOGE("config: SIGNALING_SCHEME=tls requires SIGNALING_CERT"); return -1; }
        if (!cfg->signaling_key[0])  { LOGE("config: SIGNALING_SCHEME=tls requires SIGNALING_KEY");  return -1; }
#endif
    } else if (strcasecmp(cfg->signaling_scheme, "plain") != 0) {
        LOGE("config: SIGNALING_SCHEME must be 'plain' or 'tls' (got '%s')", cfg->signaling_scheme);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/*  Length-prefixed signaling protocol — TLS-aware                          */
/* ------------------------------------------------------------------------ */

/* sig_t wraps either a plain TCP fd or an SSL* on top of an fd.  Public
 * surface (sig_send/sig_recv/sig_close/sig_fd) keeps the run_signaling
 * loop oblivious to which transport it's running over. */
typedef struct sig_io {
    int   fd;        /* always valid — the underlying TCP socket */
#ifdef KITTY_TLS
    SSL  *ssl;       /* non-NULL iff cfg->signaling_scheme == "tls" */
    SSL_CTX *ctx;    /* owned by this sig_t; freed in sig_close */
#endif
} sig_t;

/* Read exactly `n` bytes from the signaling transport (handles both the
 * plain MSG_WAITALL semantics and SSL's may-return-less-bytes semantics). */
static ssize_t sig_read_all(sig_t *sig, void *buf, size_t n) {
    size_t off = 0;
#ifdef KITTY_TLS
    if (sig->ssl) {
        while (off < n) {
            int r = SSL_read(sig->ssl, (char*)buf + off, (int)(n - off));
            if (r <= 0) {
                int err = SSL_get_error(sig->ssl, r);
                if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) continue;
                return -1;
            }
            off += (size_t)r;
        }
        return (ssize_t)off;
    }
#endif
    /* plain path — preserve MSG_WAITALL behaviour */
    while (off < n) {
        ssize_t k = recv(sig->fd, (char*)buf + off, n - off, MSG_WAITALL);
        if (k <= 0) return -1;
        off += (size_t)k;
    }
    return (ssize_t)off;
}

/* Write exactly `n` bytes — handles SSL partial writes. */
static ssize_t sig_write_all(sig_t *sig, const void *buf, size_t n) {
    size_t off = 0;
#ifdef KITTY_TLS
    if (sig->ssl) {
        while (off < n) {
            int r = SSL_write(sig->ssl, (const char*)buf + off, (int)(n - off));
            if (r <= 0) {
                int err = SSL_get_error(sig->ssl, r);
                if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) continue;
                return -1;
            }
            off += (size_t)r;
        }
        return (ssize_t)off;
    }
#endif
    while (off < n) {
        ssize_t k = send(sig->fd, (const char*)buf + off, n - off, 0);
        if (k <= 0) return -1;
        off += (size_t)k;
    }
    return (ssize_t)off;
}

static int sig_send(sig_t *sig, const char *msg) {
    size_t len = strlen(msg);
    uint32_t nlen = htonl((uint32_t)len);
    if (sig_write_all(sig, &nlen, 4) != 4) return -1;
    if (sig_write_all(sig, msg, len) != (ssize_t)len) return -1;
    return 0;
}

static int sig_recv(sig_t *sig, char *out, size_t cap) {
    uint32_t nlen;
    if (sig_read_all(sig, &nlen, 4) != 4) return -1;
    uint32_t len = ntohl(nlen);
    if (len >= cap) return -1;
    if (sig_read_all(sig, out, len) != (ssize_t)len) return -1;
    out[len] = 0;
    return (int)len;
}

/* Same as sig_recv but returns 0 if no data arrives within timeout_ms
 * (without consuming any bytes), -1 on TCP error, length on success.
 * Used by run_signaling so it can send periodic application-level PINGs
 * during idle periods.  Works for TLS too: SSL_pending() catches buffered
 * bytes that arrived in the same TCP segment as a prior message; poll()
 * catches new fresh data. */
static int sig_recv_timed(sig_t *sig, char *out, size_t cap, int timeout_ms) {
#ifdef KITTY_TLS
    if (sig->ssl && SSL_pending(sig->ssl) > 0) {
        return sig_recv(sig, out, cap);
    }
#endif
    struct pollfd pfd = { .fd = sig->fd, .events = POLLIN };
    int pr = poll(&pfd, 1, timeout_ms);
    if (pr < 0) { if (errno == EINTR) return 0; return -1; }
    if (pr == 0) return 0;  /* timeout — caller should send keepalive */
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return -1;
    return sig_recv(sig, out, cap);
}

static int kv_get(const char *msg, const char *key, char *out, size_t cap) {
    if (!out || cap == 0) return -1;
    out[0] = 0;
    size_t klen = strlen(key);
    const char *p = msg;
    while (*p) {
        const char *eol = strchr(p, '\n'); if (!eol) eol = p + strlen(p);
        if ((size_t)(eol - p) > klen + 1 && memcmp(p, key, klen) == 0 && p[klen] == '=') {
            size_t vlen = (size_t)(eol - p) - klen - 1;
            if (vlen >= cap) vlen = cap - 1;
            memcpy(out, p + klen + 1, vlen); out[vlen] = 0;
            return 0;
        }
        if (!*eol) break;
        p = eol + 1;
    }
    return -1;
}

/* ------------------------------------------------------------------------ */
/*  TCP signaling connect with TCP keepalive                                */
/* ------------------------------------------------------------------------ */

/* Plain-TCP-only piece: getaddrinfo + socket + connect + keepalive.
 * Used as the foundation for both plain and TLS paths. */
static int signaling_tcp_connect(const char *host, int port) {
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    char portstr[8]; snprintf(portstr, sizeof(portstr), "%d", port);
    struct addrinfo *ai = NULL;
    if (getaddrinfo(host, portstr, &hints, &ai) != 0) return -1;
    int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) { freeaddrinfo(ai); return -1; }
    if (connect(fd, ai->ai_addr, ai->ai_addrlen) < 0) {
        LOGE("signaling: connect %s:%d failed: %s", host, port, strerror(errno));
        close(fd); freeaddrinfo(ai); return -1;
    }
    freeaddrinfo(ai);
    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    setsockopt(fd, SOL_SOCKET,  SO_KEEPALIVE, &yes, sizeof(yes));
#ifdef TCP_KEEPIDLE
    int ka_idle = 30;  setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE,  &ka_idle, sizeof(ka_idle));
#endif
#ifdef TCP_KEEPINTVL
    int ka_intvl = 10; setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &ka_intvl, sizeof(ka_intvl));
#endif
#ifdef TCP_KEEPCNT
    int ka_cnt = 3;    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT,   &ka_cnt, sizeof(ka_cnt));
#endif
    return fd;
}

#ifdef KITTY_TLS
/* OpenSSL one-time init.  Modern openssl 3.x doesn't need explicit
 * library_init, but keeping the call is harmless and defends against
 * older runtime stubs. */
static void tls_one_time_init(void) {
    static int done = 0;
    if (done) return;
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();
    done = 1;
}

static void tls_log_errors(const char *label) {
    unsigned long e;
    while ((e = ERR_get_error()) != 0) {
        char buf[256];
        ERR_error_string_n(e, buf, sizeof(buf));
        LOGE("tls %s: %s", label, buf);
    }
}
#endif

/* sig_open: create a sig_t handle for signaling I/O.  Returns NULL on
 * failure (any failure log line already emitted). */
static sig_t *sig_open(const config_t *cfg) {
    sig_t *sig = (sig_t*)calloc(1, sizeof(*sig));
    if (!sig) return NULL;
    sig->fd = signaling_tcp_connect(cfg->signaling_host, cfg->signaling_port);
    if (sig->fd < 0) { free(sig); return NULL; }

    if (strcasecmp(cfg->signaling_scheme, "plain") == 0) {
        LOGI("signaling: connected %s:%d (plain)", cfg->signaling_host, cfg->signaling_port);
        return sig;
    }

#ifdef KITTY_TLS
    tls_one_time_init();
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) { tls_log_errors("SSL_CTX_new"); goto fail; }
    /* TLS 1.2 min — matches the nginx server config (ssl_protocols TLSv1.2 TLSv1.3). */
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
    SSL_CTX_set_verify_depth(ctx, 3);
    if (SSL_CTX_load_verify_locations(ctx, cfg->signaling_ca, NULL) != 1) {
        LOGE("tls: load CA from %s failed", cfg->signaling_ca);
        tls_log_errors("load_verify_locations"); goto fail;
    }
    if (SSL_CTX_use_certificate_chain_file(ctx, cfg->signaling_cert) != 1) {
        LOGE("tls: load client cert from %s failed", cfg->signaling_cert);
        tls_log_errors("use_certificate_chain_file"); goto fail;
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, cfg->signaling_key, SSL_FILETYPE_PEM) != 1) {
        LOGE("tls: load private key from %s failed", cfg->signaling_key);
        tls_log_errors("use_PrivateKey_file"); goto fail;
    }
    if (SSL_CTX_check_private_key(ctx) != 1) {
        LOGE("tls: private key does not match certificate");
        tls_log_errors("check_private_key"); goto fail;
    }

    SSL *ssl = SSL_new(ctx);
    if (!ssl) { tls_log_errors("SSL_new"); goto fail; }
    SSL_set_fd(ssl, sig->fd);
    /* SNI — required by nginx ssl_preread + by virtual hosts. */
    SSL_set_tlsext_host_name(ssl, cfg->signaling_host);
    /* Hostname verification — RFC 6125, matches CN or any SAN. */
    if (cfg->signaling_verify_host) {
        X509_VERIFY_PARAM *vp = SSL_get0_param(ssl);
        X509_VERIFY_PARAM_set_hostflags(vp, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
        if (X509_VERIFY_PARAM_set1_host(vp, cfg->signaling_host, 0) != 1) {
            LOGE("tls: set hostname verification target failed");
            SSL_free(ssl); goto fail;
        }
    }
    int hs = SSL_connect(ssl);
    if (hs != 1) {
        int err = SSL_get_error(ssl, hs);
        LOGE("tls: handshake failed (SSL_get_error=%d)", err);
        tls_log_errors("SSL_connect");
        SSL_free(ssl); goto fail;
    }
    long vr = SSL_get_verify_result(ssl);
    if (vr != X509_V_OK) {
        LOGE("tls: server cert verify failed: %ld (%s)", vr,
             X509_verify_cert_error_string(vr));
        SSL_free(ssl); goto fail;
    }
    sig->ssl = ssl;
    sig->ctx = ctx;
    LOGI("signaling: connected %s:%d (tls 1.2+; mTLS active; server-verify ok)",
         cfg->signaling_host, cfg->signaling_port);
    return sig;

fail:
    if (ctx) SSL_CTX_free(ctx);
    close(sig->fd); free(sig); return NULL;
#else
    LOGE("signaling: SCHEME=tls but binary built without TLS"); /* unreachable; checked in load_config */
    close(sig->fd); free(sig); return NULL;
#endif
}

static void sig_close(sig_t *sig) {
    if (!sig) return;
#ifdef KITTY_TLS
    if (sig->ssl) { SSL_shutdown(sig->ssl); SSL_free(sig->ssl); }
    if (sig->ctx) SSL_CTX_free(sig->ctx);
#endif
    if (sig->fd >= 0) close(sig->fd);
    free(sig);
}

/* ------------------------------------------------------------------------ */
/*  Local TCP fetch (HTTP-FLV from camera firmware)                         */
/* ------------------------------------------------------------------------ */

static int connect_local(const char *host, int port) {
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    char portstr[8]; snprintf(portstr, sizeof(portstr), "%d", port);
    struct addrinfo *ai = NULL;
    if (getaddrinfo(host, portstr, &hints, &ai) != 0) return -1;
    int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) { freeaddrinfo(ai); return -1; }
    if (connect(fd, ai->ai_addr, ai->ai_addrlen) < 0) {
        LOGE("local: connect %s:%d failed: %s", host, port, strerror(errno));
        close(fd); freeaddrinfo(ai); return -1;
    }
    freeaddrinfo(ai);
    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    int rcvbuf = 4 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    return fd;
}

static int send_http_get(int local_fd, const config_t *cfg) {
    char req[1024];
    int n = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\nHost: %s:%d\r\nUser-Agent: arcis-srt-provider/3.0\r\n"
        "Accept: */*\r\nConnection: keep-alive\r\n%s%s%s\r\n",
        cfg->local_http_path, cfg->local_host, cfg->local_port,
        cfg->local_http_auth[0] ? "Authorization: " : "",
        cfg->local_http_auth, cfg->local_http_auth[0] ? "\r\n" : "");
    if (n <= 0 || n >= (int)sizeof(req)) return -1;
    if (send(local_fd, req, (size_t)n, 0) != n) return -1;
    return 0;
}

static int read_http_response_headers(int local_fd, uint8_t *buf, size_t cap,
                                      size_t *body_off, size_t *body_len) {
    size_t total = 0;
    int prev_was_le = 0, saw_double = 0;
    while (total < cap) {
        ssize_t k = recv(local_fd, buf + total, cap - total, 0);
        if (k <= 0) return -1;
        for (size_t i = total; i < total + (size_t)k; i++) {
            uint8_t c = buf[i];
            if (c == '\n') {
                if (prev_was_le) { saw_double = 1; *body_off = i + 1; break; }
                prev_was_le = 1;
            } else if (c != '\r') {
                prev_was_le = 0;
            }
        }
        total += (size_t)k;
        if (saw_double) break;
    }
    if (!saw_double) return -1;
    if (total < 12 || memcmp(buf, "HTTP/1.", 7) != 0) return -1;
    int status = atoi((const char*)buf + 9);
    if (status != 200) {
        size_t print = total < 256 ? total : 256;
        LOGE("http: status=%d head: %.*s", status, (int)print, buf);
        return -1;
    }
    *body_len = total - *body_off;
    return 0;
}

/* ------------------------------------------------------------------------ */
/*  SRT options + rendezvous helpers                                        */
/* ------------------------------------------------------------------------ */

static int set_srt_options_caller(SRTSOCKET s) {
    int yes = 1;
    int payload  = SRT_PAYLOAD_SIZE;
    int conn_to  = CONNECT_TIMEOUT_MS;
    int idle_to  = PEER_IDLE_TIMEOUT_MS;
    /* SRTT_FILE = reliable byte-stream (TCP-like semantics over UDP).  The
     * earlier LIVE mode dropped any message whose ARQ retransmit exceeded
     * RCVLATENCY=300ms — observed in the wild as RCV-DROPPED entries in the
     * receiver log, each one a hole in the byte stream that would land
     * mid-FLV-tag and permanently desync the demuxer.  FILE mode retransmits
     * indefinitely, guaranteeing every byte the FLV demuxer sees is the
     * exact byte the camera emitted, in order. */
    int transtype  = SRTT_FILE;
    int messageapi = 0;
    if (srt_setsockopt(s, 0, SRTO_TRANSTYPE,     &transtype, sizeof(transtype))   != 0) return -1;
    if (srt_setsockopt(s, 0, SRTO_MESSAGEAPI,    &messageapi, sizeof(messageapi)) != 0) return -1;
    if (srt_setsockopt(s, 0, SRTO_RENDEZVOUS,    &yes,     sizeof(yes))     != 0) return -1;
    if (srt_setsockopt(s, 0, SRTO_REUSEADDR,     &yes,     sizeof(yes))     != 0) return -1;
    if (srt_setsockopt(s, 0, SRTO_PAYLOADSIZE,   &payload, sizeof(payload)) != 0) return -1;
    if (srt_setsockopt(s, 0, SRTO_CONNTIMEO,     &conn_to, sizeof(conn_to)) != 0) return -1;
    if (srt_setsockopt(s, 0, SRTO_PEERIDLETIMEO, &idle_to, sizeof(idle_to)) != 0) return -1;
    if (g_cfg.service_id[0]) {
        if (srt_setsockopt(s, 0, SRTO_STREAMID, g_cfg.service_id,
                           (int)strlen(g_cfg.service_id)) != 0) return -1;
    }
    return 0;
}

static SRTSOCKET srt_rendezvous_connect(int local_port,
                                        const char *peer_ip, int peer_port) {
    SRTSOCKET s = srt_create_socket();
    if (s == SRT_INVALID_SOCK) {
        LOGE("srt_create_socket: %s", srt_getlasterror_str());
        return SRT_INVALID_SOCK;
    }
    if (set_srt_options_caller(s) != 0) {
        LOGE("srt_setsockopt: %s", srt_getlasterror_str());
        srt_close(s); return SRT_INVALID_SOCK;
    }
    struct sockaddr_in la = {0};
    la.sin_family = AF_INET; la.sin_addr.s_addr = htonl(INADDR_ANY);
    la.sin_port = htons(local_port);
    if (srt_bind(s, (struct sockaddr*)&la, sizeof(la)) == SRT_ERROR) {
        LOGE("srt_bind :%d: %s", local_port, srt_getlasterror_str());
        srt_close(s); return SRT_INVALID_SOCK;
    }
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM };
    char portstr[8]; snprintf(portstr, sizeof(portstr), "%d", peer_port);
    struct addrinfo *ai = NULL;
    if (getaddrinfo(peer_ip, portstr, &hints, &ai) != 0) {
        LOGE("srt: peer getaddrinfo failed");
        srt_close(s); return SRT_INVALID_SOCK;
    }
    LOGI("srt: rendezvous connect %s:%d (local %d)", peer_ip, peer_port, local_port);
    int rc = srt_connect(s, ai->ai_addr, (int)ai->ai_addrlen);
    freeaddrinfo(ai);
    if (rc == SRT_ERROR) {
        LOGE("srt_connect rendezvous failed: %s", srt_getlasterror_str());
        srt_close(s); return SRT_INVALID_SOCK;
    }
    LOGI("srt: rendezvous handshake complete");
    return s;
}

static int srt_pump_bytes(SRTSOCKET s, const uint8_t *buf, size_t n,
                          uint64_t *out_total) {
    size_t off = 0;
    while (off < n && !g_shutdown) {
        int chunk = (int)(n - off);
        if (chunk > SRT_PAYLOAD_SIZE) chunk = SRT_PAYLOAD_SIZE;
        /* SRTT_FILE byte-stream API: srt_send (NOT srt_sendmsg).  Returns
         * the number of bytes successfully queued; we may need to loop. */
        int rc = srt_send(s, (const char*)(buf + off), chunk);
        if (rc == SRT_ERROR) {
            LOGE("srt_send: %s", srt_getlasterror_str());
            return -1;
        }
        off += rc; *out_total += (uint64_t)rc;
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/*  Pump-thread state.  The signaling thread fills next_peer and posts the
 *  cancel-pump signal whenever a fresh SRT_PEER arrives.  The pump thread
 *  watches g_pump_cancel and aborts its current SRT session when set.       */
/* ------------------------------------------------------------------------ */

typedef struct {
    char        peer_ip[64];
    int         peer_port;
    int         local_port;     /* port the camera should bind SRT to */
    /* Snapshot of camera's STUN-discovered SRFLX/LAN at the moment the
     * peer message was received.  Owned by signaling thread, read by
     * pump thread.  Static, no further mutation. */
    char        my_srflx_ip[64];
} pump_request_t;

static pthread_mutex_t   g_pump_mtx     = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t    g_pump_cv      = PTHREAD_COND_INITIALIZER;
static pump_request_t    g_pending_pump;          /* protected by g_pump_mtx */
static int               g_pending_valid = 0;     /* protected by g_pump_mtx */
static atomic_int        g_pump_cancel  = 0;      /* read by pump thread without lock */
static atomic_int        g_pump_running = 0;      /* set by pump thread, read by sig */
static SRTSOCKET         g_active_srt   = -1;  /* SRT_INVALID_SOCK — GCC5/C99: static const isn't a constant expr */

/* Pump thread reads from local HTTP-FLV TCP and sends to SRT until any
 * of: (a) SRT breaks, (b) local closed, (c) cancel set, (d) shutdown. */
static int pump_local_to_srt(int local_fd, SRTSOCKET srt_sock) {
    uint8_t *buf = malloc(LOCAL_BUFFER_SIZE);
    if (!buf) return -1;
    uint64_t bytes_in = 0, bytes_out = 0;
    time_t   last_log = time(NULL);

    if (g_cfg.local_http_path[0]) {
        if (send_http_get(local_fd, &g_cfg) != 0) { free(buf); return -1; }
        size_t body_off = 0, body_len = 0;
        if (read_http_response_headers(local_fd, buf, LOCAL_BUFFER_SIZE,
                                       &body_off, &body_len) != 0) {
            free(buf); return -1;
        }
        if (body_len > 0) {
            if (srt_pump_bytes(srt_sock, buf + body_off, body_len, &bytes_out) != 0) {
                free(buf); return -1;
            }
            bytes_in += body_len;
        }
    }

    int local_flags = fcntl(local_fd, F_GETFL, 0);
    fcntl(local_fd, F_SETFL, local_flags | O_NONBLOCK);

    while (!g_shutdown && !atomic_load(&g_pump_cancel)) {
        struct pollfd pfd = { .fd = local_fd, .events = POLLIN };
        int pr = poll(&pfd, 1, 200);   /* 200 ms tick — checks cancel flag often */
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (pr == 0) continue;
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) break;
        ssize_t n = recv(local_fd, buf, LOCAL_BUFFER_SIZE, 0);
        if (n == 0) { LOGI("local: peer closed"); break; }
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            LOGE("local recv: %s", strerror(errno)); break;
        }
        bytes_in += (uint64_t)n;
        if (srt_pump_bytes(srt_sock, buf, (size_t)n, &bytes_out) != 0) break;

        time_t now = time(NULL);
        if (now - last_log >= 5) {
            LOGI("pump: in=%llu out=%llu",
                 (unsigned long long)bytes_in, (unsigned long long)bytes_out);
            last_log = now;
        }
    }
    free(buf);
    if (atomic_load(&g_pump_cancel)) {
        LOGI("pump: cancelled by signaling thread (new peer arrived)");
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/*  Pump thread main                                                         */
/* ------------------------------------------------------------------------ */

static void *pump_thread_main(void *arg) {
    (void)arg;
    while (!g_shutdown) {
        /* Wait for signaling thread to post a pump request. */
        pthread_mutex_lock(&g_pump_mtx);
        while (!g_pending_valid && !g_shutdown) {
            pthread_cond_wait(&g_pump_cv, &g_pump_mtx);
        }
        if (g_shutdown) { pthread_mutex_unlock(&g_pump_mtx); break; }
        pump_request_t req = g_pending_pump;
        g_pending_valid = 0;
        atomic_store(&g_pump_cancel, 0);
        atomic_store(&g_pump_running, 1);
        pthread_mutex_unlock(&g_pump_mtx);

        /* Run the SRT rendezvous + pump for THIS peer.  Once the pump exits
         * (SRT broke, local closed, or cancel from signaling), we loop back
         * up and either block on a fresh pump request or jump straight into
         * the next pending one if a new SRT_PEER arrived during the pump. */
        SRTSOCKET s = srt_rendezvous_connect(req.local_port, req.peer_ip, req.peer_port);
        if (s != SRT_INVALID_SOCK) {
            pthread_mutex_lock(&g_pump_mtx);
            g_active_srt = s;
            pthread_mutex_unlock(&g_pump_mtx);

            int local_fd = connect_local(g_cfg.local_host, g_cfg.local_port);
            if (local_fd >= 0) {
                pump_local_to_srt(local_fd, s);
                close(local_fd);
            } else {
                LOGE("pump: connect_local failed; aborting session");
            }

            pthread_mutex_lock(&g_pump_mtx);
            g_active_srt = SRT_INVALID_SOCK;
            pthread_mutex_unlock(&g_pump_mtx);
            srt_close(s);
        }

        atomic_store(&g_pump_running, 0);
    }
    return NULL;
}

/* Signaling thread calls this when a fresh SRT_PEER arrives.  It tears down
 * any in-flight pump (causing a cancel) and posts the new request. */
static void post_pump_request(const pump_request_t *req) {
    pthread_mutex_lock(&g_pump_mtx);
    /* Cancel any current pump. */
    atomic_store(&g_pump_cancel, 1);
    if (g_active_srt != SRT_INVALID_SOCK) {
        /* Closing the SRT socket from another thread unblocks any in-progress
         * srt_sendmsg with SRT_ERROR.  This is the ONLY safe way to interrupt
         * a blocked SRT call without waiting for PEERIDLETIMEO. */
        srt_close(g_active_srt);
        g_active_srt = SRT_INVALID_SOCK;
    }
    g_pending_pump  = *req;
    g_pending_valid = 1;
    pthread_cond_signal(&g_pump_cv);
    pthread_mutex_unlock(&g_pump_mtx);
}

/* ------------------------------------------------------------------------ */
/*  Signaling thread main (the IMMORTAL one)                                */
/* ------------------------------------------------------------------------ */

static int allocate_stun_socket(int wanted_port) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in la = {0};
    la.sin_family = AF_INET; la.sin_addr.s_addr = htonl(INADDR_ANY);
    la.sin_port = htons(wanted_port);
    if (bind(s, (struct sockaddr*)&la, sizeof(la)) == 0) return s;
    int err = errno;
    LOGW("stun bind :%d failed (%s) — falling back to ephemeral", wanted_port, strerror(err));
    close(s);
    s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    la.sin_port = htons(0);
    if (bind(s, (struct sockaddr*)&la, sizeof(la)) < 0) {
        LOGE("stun bind ephemeral failed: %s", strerror(errno));
        close(s); return -1;
    }
    return s;
}

static int get_local_port(int sockfd) {
    struct sockaddr_in la; socklen_t lalen = sizeof(la);
    if (getsockname(sockfd, (struct sockaddr*)&la, &lalen) != 0) return -1;
    return ntohs(la.sin_port);
}

/* One full attempt at maintaining the signaling TCP forever.  Returns when
 * the TCP dies (we'll be called again with backoff). */
static int run_signaling(void) {
    /* 1. Discover our public address and LAN address. */
    int udp = allocate_stun_socket(g_cfg.bind_port);
    if (udp < 0) return -1;
    int local_port = get_local_port(udp);

    stun_binding_t srflx;
    if (stun_get_srflx(udp, g_cfg.stun_host, g_cfg.stun_port, &srflx, 2) != 0) {
        LOGE("stun: discovery failed"); close(udp); return -1;
    }
    char lan_ip[64] = {0};
    stun_get_lan_ip(lan_ip, sizeof(lan_ip));
    LOGI("stun: SRFLX=%s:%d  LAN=%s:%d  (this is our reachable address)",
         srflx.srflx_ip, srflx.srflx_port, lan_ip, local_port);
    /* We close the STUN socket immediately because our pump thread will need
     * to bind SRT to local_port for rendezvous.  Outbound NAT mapping is
     * preserved for ~30 s on most NATs, which is enough for the next SRT
     * rendezvous to refresh it. */
    close(udp);

    /* 2. Connect to signaling and register. */
    sig_t *sig = sig_open(&g_cfg);
    if (!sig) return -1;

    char regmsg[1024];
    int rln = snprintf(regmsg, sizeof(regmsg),
        "type=SRT_REGISTER\nservice_id=%s\nsrflx_ip=%s\nsrflx_port=%d\n%s%s%s%s%s%s",
        g_cfg.service_id, srflx.srflx_ip, srflx.srflx_port,
        lan_ip[0] ? "lan_ip=" : "", lan_ip, lan_ip[0] ? "\n" : "",
        g_cfg.api_token[0] ? "api_token=" : "",
        g_cfg.api_token,
        g_cfg.api_token[0] ? "\n" : "");
    (void)rln;
    if (sig_send(sig, regmsg) < 0) {
        LOGE("signaling: send SRT_REGISTER failed"); sig_close(sig); return -1;
    }
    char inbuf[8192];
    if (sig_recv(sig, inbuf, sizeof(inbuf)) <= 0) {
        LOGE("signaling: no SRT_REGISTERED"); sig_close(sig); return -1;
    }
    char resp_type[64]; kv_get(inbuf, "type", resp_type, sizeof(resp_type));
    if (strcmp(resp_type, "SRT_REGISTERED") != 0) {
        LOGE("signaling: expected SRT_REGISTERED, got '%s'", resp_type);
        sig_close(sig); return -1;
    }
    LOGI("signaling: registered as %s", g_cfg.service_id);

    /* 3. Forever-loop reading messages and dispatching pump requests.
     *
     * Camera-initiated application-level keepalive (every 10s when the link
     * is idle).  Critical for two reasons observed in the field on the
     * Augentix camera over Reliance/Jio cellular:
     *   (a) Carrier CGNAT silently times out idle TCP mappings around the
     *       1-3 minute mark.  After SRT_REQUEST handshakes complete, the
     *       signaling TCP is silent for the entire pump session — and once
     *       NAT removes the mapping, the next server probe is dropped, server
     *       sees TCP dead via SO_KEEPALIVE (15s+5s*3=30s detection), closes,
     *       and the camera disappears from the registry until reconnect.
     *   (b) Application-level traffic refreshes the NAT mapping AND lets the
     *       camera detect a dead TCP within ~10s (its own send returns -1)
     *       instead of waiting for the server's next keepalive failure.
     * The PING is `type=PING\nservice_id=<id>\n` — server doesn't need a
     * dedicated handler (unknown msg_types are ignored upstream); the byte
     * exchange itself is sufficient to refresh the mapping. */
    const int SIG_PING_INTERVAL_MS = 10000;
    char pingbuf[256];
    int pinglen = snprintf(pingbuf, sizeof(pingbuf),
        "type=PING\nservice_id=%s\n", g_cfg.service_id);
    (void)pinglen;
    while (!g_shutdown) {
        int got = sig_recv_timed(sig, inbuf, sizeof(inbuf), SIG_PING_INTERVAL_MS);
        if (got < 0) { LOGW("signaling: TCP closed by peer"); break; }
        if (got == 0) {
            /* No data for SIG_PING_INTERVAL_MS — send keepalive. */
            if (sig_send(sig, pingbuf) != 0) {
                LOGW("signaling: keepalive send failed — TCP must be dead");
                break;
            }
            LOGD("signaling: keepalive PING sent");
            continue;
        }
        char t[64]; kv_get(inbuf, "type", t, sizeof(t));
        if (!t[0]) continue;
        if (strcmp(t, "SRT_PEER") == 0) {
            char peer_srflx_ip[64], peer_srflx_port_str[16], peer_lan_ip[64];
            kv_get(inbuf, "srflx_ip",   peer_srflx_ip,        sizeof(peer_srflx_ip));
            kv_get(inbuf, "srflx_port", peer_srflx_port_str,  sizeof(peer_srflx_port_str));
            kv_get(inbuf, "lan_ip",     peer_lan_ip,          sizeof(peer_lan_ip));
            int peer_port = atoi(peer_srflx_port_str);
            if (!peer_srflx_ip[0] || peer_port <= 0) continue;

            pump_request_t req;
            memset(&req, 0, sizeof(req));
            /* Same-NAT detection: if peer's SRFLX matches ours, prefer LAN. */
            if (peer_lan_ip[0] && strcmp(srflx.srflx_ip, peer_srflx_ip) == 0) {
                snprintf(req.peer_ip, sizeof(req.peer_ip), "%s", peer_lan_ip);
                LOGI("SRT_PEER: same-NAT detected; LAN target=%s:%d", req.peer_ip, peer_port);
            } else {
                snprintf(req.peer_ip, sizeof(req.peer_ip), "%s", peer_srflx_ip);
                LOGI("SRT_PEER: cross-NAT target=%s:%d", req.peer_ip, peer_port);
            }
            req.peer_port  = peer_port;
            req.local_port = local_port;
            snprintf(req.my_srflx_ip, sizeof(req.my_srflx_ip), "%s", srflx.srflx_ip);
            post_pump_request(&req);
        } else if (strcmp(t, "PING") == 0) {
            /* Server keep-alive — respond with PONG so it doesn't time us out. */
            const char *pong = "type=PONG\n";
            sig_send(sig, pong);
        } else {
            LOGD("signaling: ignoring type=%s", t);
        }
    }
    sig_close(sig);
    return 0;
}

/* ------------------------------------------------------------------------ */
/*  Top-level supervisor                                                    */
/* ------------------------------------------------------------------------ */

static void on_signal(int sig) { (void)sig; g_shutdown = 1; pthread_cond_broadcast(&g_pump_cv); }

int main(int argc, char **argv) {
    const char *cfg_path = (argc >= 2) ? argv[1] : "provider.conf";
    if (load_config(cfg_path, &g_cfg) != 0) return 1;

    LOGI("provider_srt v3 (multi-thread): service_id=%s signaling=%s:%d stun=%s:%d local=%s:%d",
         g_cfg.service_id,
         g_cfg.signaling_host, g_cfg.signaling_port,
         g_cfg.stun_host, g_cfg.stun_port,
         g_cfg.local_host, g_cfg.local_port);

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);
    if (srt_startup() != 0) { LOGE("srt_startup failed"); return 1; }

    /* Spawn the pump thread once.  It blocks on the condvar until the
     * signaling thread posts a peer, then runs one rendezvous+pump cycle,
     * then loops back to wait. */
    pthread_t pump_tid;
    if (pthread_create(&pump_tid, NULL, pump_thread_main, NULL) != 0) {
        LOGE("pthread_create pump: %s", strerror(errno));
        return 1;
    }

    /* Signaling supervisor — keeps the camera registered with the cloud
     * forever.  Phone-side events (close/reopen, subprocess crash, network
     * change) NEVER reach this loop.  Only TCP failures cause a backoff +
     * reconnect. */
    int backoff = RECONNECT_MIN_MS;
    while (!g_shutdown) {
        time_t t0 = time(NULL);
        run_signaling();
        if (g_shutdown) break;
        time_t lasted = time(NULL) - t0;
        if (lasted >= LONG_SESSION_S) backoff = RECONNECT_MIN_MS;
        LOGI("signaling: TCP died (lasted %lds); reconnecting in %d ms", (long)lasted, backoff);
        struct timespec ts = { backoff / 1000, (backoff % 1000) * 1000000L };
        nanosleep(&ts, NULL);
        backoff *= 2;
        if (backoff > RECONNECT_MAX_MS) backoff = RECONNECT_MAX_MS;
    }

    pthread_cond_broadcast(&g_pump_cv);
    pthread_join(pump_tid, NULL);
    srt_cleanup();
    return 0;
}
