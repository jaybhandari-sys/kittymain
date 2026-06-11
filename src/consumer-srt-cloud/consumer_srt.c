/* Consumer (cloud-side), SRT transport.
 *
 * Phase 1 of the SRT pipeline.  Replaces the libjuice + reassembly + dispatch
 * stack from consumer_api.c with: (1) an SRT listener that accepts inbound
 * connections from camera-side providers, and (2) a tiny HTTP fan-out that
 * lets phones (or any HTTP-FLV client) join the resulting byte stream.
 *
 * Why this is so much smaller than consumer_api.c:
 *   - SRT delivers an ordered, congestion-controlled byte stream.  No 16-byte
 *     chunk header, no offset bitmap, no MAX_CHUNKS_PER_FRAME.
 *   - SRT has SRTO_PEERIDLETIMEO (default 5 s).  When the camera goes silent
 *     for that long, srt_recv() returns SRT_ECONN — no more "frame_in_progress
 *     stays 0 forever and the local TCP socket never closes" (option-3 freeze).
 *
 * Phase 1 simplifications (each will be lifted in a later pass):
 *   - One SRT stream per process.  STREAM_ID in the SRT handshake is checked
 *     for an exact match against the configured value.
 *   - At most ONE active SRT connection at a time.  A second incoming
 *     connection with the right STREAM_ID replaces the first.
 *   - HTTP-FLV clients are fanned out from a fixed-size ring of recent bytes.
 *     A late-joining client that misses the FLV file header will not see a
 *     valid stream until the camera reconnects (forcing a fresh header).
 *     Multi-client late-join requires header caching, which Phase 2 adds.
 */

#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
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

/* ------------------------------------------------------------------------ */
/*  Configuration                                                           */
/* ------------------------------------------------------------------------ */

#define DEFAULT_LATENCY_MS  300
#define SRT_PAYLOAD_SIZE    1316
#define RING_SIZE           (4 * 1024 * 1024)   /* 4 MiB rolling buffer per stream */
#define MAX_HTTP_CLIENTS    8
#define HTTP_LISTEN_BACKLOG 8
#define CONFIG_LINE_MAX     256
#define HEADER_CACHE_SIZE   2048    /* first N bytes of every SRT connection — FLV file header + initial metadata tag, prepended to every new HTTP client */

typedef struct {
    int  srt_port;            /* SRT listen port (e.g. 9888) */
    int  http_port;           /* HTTP-FLV listen port (e.g. 8080) */
    char stream_id[256];      /* required STREAM_ID match */
    char http_path[128];      /* HTTP path the FLV is served at, e.g. /flv/live_ch0_1.flv */
    char verify_token[256];   /* optional ?verify=... value to require; empty = no check */
    int  latency_ms;
    int  verbose;
    int  use_message_api;
    int  passphrase_set;
    char passphrase[80];
    int  pbkeylen;
} config_t;

static config_t g_cfg;
static volatile int g_shutdown = 0;

/* ------------------------------------------------------------------------ */
/*  Logging                                                                 */
/* ------------------------------------------------------------------------ */

static void log_msg(const char *level, const char *fmt, ...) {
    char ts[32];
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    struct tm tm;
    localtime_r(&now.tv_sec, &tm);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
    fprintf(stdout, "[%s.%03ld] [%s] ", ts, now.tv_nsec / 1000000, level);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
    fflush(stdout);
}

#define LOGI(...) log_msg("INFO ", __VA_ARGS__)
#define LOGW(...) log_msg("WARN ", __VA_ARGS__)
#define LOGE(...) log_msg("ERROR", __VA_ARGS__)
#define LOGD(...) do { if (g_cfg.verbose) log_msg("DEBUG", __VA_ARGS__); } while (0)

/* ------------------------------------------------------------------------ */
/*  Stream state: ring buffer + a list of HTTP client fds to fan out to.    */
/* ------------------------------------------------------------------------ */

typedef struct {
    int      fd;                 /* HTTP client TCP socket */
    uint64_t cursor;             /* total bytes already sent to this client */
    int      header_sent;        /* 1 once HTTP/1.1 200 ... \r\n\r\n has been written */
    int      flv_hdr_sent;       /* 1 once cached FLV header bytes have been replayed */
    size_t   flv_hdr_off;        /* progress through the cached FLV header */
} http_client_t;

typedef struct {
    pthread_mutex_t  mutex;
    pthread_cond_t   data_ready;
    /* Ring buffer of recent bytes from SRT.  total_bytes is the running
     * counter of all bytes ever received; clients track their own cursor and
     * we copy [cursor, total_bytes) into their TCP socket. */
    uint8_t         *buf;
    size_t           cap;        /* RING_SIZE */
    uint64_t         total_bytes;/* monotonic counter of bytes received from SRT */

    /* HTTP fan-out — each slot is an active client. */
    http_client_t    clients[MAX_HTTP_CLIENTS];
    int              n_clients;

    /* The active SRT socket; SRT_INVALID_SOCK when no provider connected. */
    SRTSOCKET        srt_sock;

    /* Cached prefix of the current SRT session, replayed verbatim to each
     * new HTTP client before live data so late joiners see a valid FLV
     * file header + the initial metadata tag.  Reset each time the SRT
     * peer reconnects (new connection => new FLV header). */
    uint8_t          flv_header[HEADER_CACHE_SIZE];
    size_t           flv_header_len;
} stream_t;

static stream_t g_stream;

static void stream_init(stream_t *s) {
    pthread_mutex_init(&s->mutex, NULL);
    pthread_cond_init(&s->data_ready, NULL);
    s->buf = malloc(RING_SIZE);
    s->cap = RING_SIZE;
    s->total_bytes = 0;
    s->n_clients = 0;
    s->srt_sock = SRT_INVALID_SOCK;
    memset(s->clients, 0, sizeof(s->clients));
    for (int i = 0; i < MAX_HTTP_CLIENTS; i++) s->clients[i].fd = -1;
}

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
    cfg->srt_port  = 9888;
    cfg->http_port = 8080;
    cfg->latency_ms = DEFAULT_LATENCY_MS;
    cfg->verbose    = 0;
    cfg->use_message_api = 0;
    cfg->passphrase_set  = 0;
    cfg->pbkeylen        = 16;
    snprintf(cfg->http_path, sizeof(cfg->http_path), "/flv/live_ch0_1.flv");

    FILE *f = fopen(path, "r");
    if (!f) { LOGE("config: cannot open %s: %s", path, strerror(errno)); return -1; }

    char line[CONFIG_LINE_MAX]; int lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        char *p = strip(line);
        if (!*p || *p == '#' || *p == ';' || *p == '[') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = strip(p);
        char *v = strip(eq + 1);

        if      (!strcasecmp(k, "SRT_PORT"))      cfg->srt_port  = atoi(v);
        else if (!strcasecmp(k, "HTTP_PORT"))     cfg->http_port = atoi(v);
        else if (!strcasecmp(k, "STREAM_ID"))     snprintf(cfg->stream_id,    sizeof(cfg->stream_id),    "%s", v);
        else if (!strcasecmp(k, "HTTP_PATH"))     snprintf(cfg->http_path,    sizeof(cfg->http_path),    "%s", v);
        else if (!strcasecmp(k, "VERIFY_TOKEN"))  snprintf(cfg->verify_token, sizeof(cfg->verify_token), "%s", v);
        else if (!strcasecmp(k, "LATENCY_MS"))    cfg->latency_ms = atoi(v);
        else if (!strcasecmp(k, "VERBOSE"))       cfg->verbose    = atoi(v);
        else if (!strcasecmp(k, "MESSAGE_API"))   cfg->use_message_api = atoi(v);
        else if (!strcasecmp(k, "PASSPHRASE"))  { snprintf(cfg->passphrase,    sizeof(cfg->passphrase),   "%s", v); cfg->passphrase_set = 1; }
        else if (!strcasecmp(k, "PBKEYLEN"))      cfg->pbkeylen   = atoi(v);
        else { LOGW("config %s:%d: unknown key '%s'", path, lineno, k); }
    }
    fclose(f);
    if (!cfg->stream_id[0]) { LOGE("config: STREAM_ID is required"); return -1; }
    return 0;
}

/* ------------------------------------------------------------------------ */
/*  HTTP helpers                                                            */
/* ------------------------------------------------------------------------ */

static int write_all(int fd, const void *buf, size_t n) {
    const uint8_t *p = (const uint8_t*)buf;
    size_t left = n;
    while (left > 0) {
        ssize_t k = send(fd, p, left, MSG_NOSIGNAL);
        if (k > 0) { p += k; left -= (size_t)k; continue; }
        if (k < 0 && errno == EINTR) continue;
        return -1;
    }
    return 0;
}

static const char *HTTP_FLV_HDR =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: video/x-flv\r\n"
    "Connection: close\r\n"
    "Cache-Control: no-cache\r\n"
    "Access-Control-Allow-Origin: *\r\n"
    "Transfer-Encoding: identity\r\n"
    "\r\n";

static const char *HTTP_404 =
    "HTTP/1.1 404 Not Found\r\n"
    "Content-Length: 0\r\n"
    "Connection: close\r\n\r\n";
static const char *HTTP_403 =
    "HTTP/1.1 403 Forbidden\r\n"
    "Content-Length: 0\r\n"
    "Connection: close\r\n\r\n";

/* ------------------------------------------------------------------------ */
/*  Producer thread: SRT recv → ring buffer → broadcast to HTTP clients     */
/* ------------------------------------------------------------------------ */

static int set_srt_listener_options(SRTSOCKET s) {
    (void)s;
    /* All-default listener.  See note in provider_srt.c set_srt_options. */
    if (g_cfg.passphrase_set) {
        int pbk = g_cfg.pbkeylen;
        if (srt_setsockopt(s, 0, SRTO_PBKEYLEN,   &pbk, sizeof(pbk)) != 0) return -1;
        if (srt_setsockopt(s, 0, SRTO_PASSPHRASE, g_cfg.passphrase, (int)strlen(g_cfg.passphrase)) != 0) return -1;
    }
    return 0;
}

/* Called when an HTTP client lags behind the ring buffer's write window.
 * total_bytes - cursor > cap means the bytes the client wanted have been
 * overwritten — drop the client.  Phase 2 will instead grow the ring or
 * elastically pause the SRT producer. */
static void prune_lagged_clients_locked(stream_t *s) {
    for (int i = 0; i < s->n_clients; i++) {
        http_client_t *c = &s->clients[i];
        if (c->fd < 0) continue;
        if (s->total_bytes - c->cursor > s->cap) {
            LOGW("http: client fd=%d lagged by %llu bytes; dropping",
                 c->fd, (unsigned long long)(s->total_bytes - c->cursor));
            close(c->fd);
            c->fd = -1;
        }
    }
}

static void compact_clients_locked(stream_t *s) {
    int j = 0;
    for (int i = 0; i < s->n_clients; i++) {
        if (s->clients[i].fd >= 0) {
            if (i != j) s->clients[j] = s->clients[i];
            j++;
        }
    }
    for (int k = j; k < s->n_clients; k++) memset(&s->clients[k], 0, sizeof(s->clients[k]));
    for (int k = j; k < s->n_clients; k++) s->clients[k].fd = -1;
    s->n_clients = j;
}

static void ring_write(stream_t *s, const uint8_t *data, size_t n) {
    size_t off = s->total_bytes % s->cap;
    size_t first = (off + n <= s->cap) ? n : s->cap - off;
    memcpy(s->buf + off, data, first);
    if (first < n) memcpy(s->buf, data + first, n - first);
    s->total_bytes += n;
}

/* Drain bytes [cursor, total_bytes) to the client.  Returns 0 on success,
 * -1 if the client errored (caller should drop it). */
static int drain_client_locked(stream_t *s, http_client_t *c) {
    if (!c->header_sent) {
        if (write_all(c->fd, HTTP_FLV_HDR, strlen(HTTP_FLV_HDR)) != 0) return -1;
        c->header_sent = 1;
    }
    /* Replay cached FLV file-header so a late-joining client sees a valid
     * stream from the first byte.  flv_header_len grows the first
     * HEADER_CACHE_SIZE bytes after the SRT peer connects, then is fixed. */
    if (!c->flv_hdr_sent && s->flv_header_len > 0) {
        while (c->flv_hdr_off < s->flv_header_len) {
            ssize_t k = send(c->fd,
                             s->flv_header + c->flv_hdr_off,
                             s->flv_header_len - c->flv_hdr_off,
                             MSG_NOSIGNAL);
            if (k > 0) { c->flv_hdr_off += (size_t)k; continue; }
            if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
            if (k < 0 && errno == EINTR) continue;
            return -1;
        }
        c->flv_hdr_sent = 1;
    }
    while (c->cursor < s->total_bytes) {
        uint64_t pending = s->total_bytes - c->cursor;
        size_t   off     = c->cursor % s->cap;
        size_t   take    = (off + pending <= s->cap) ? (size_t)pending : (s->cap - off);
        if (take > (1 << 20)) take = (1 << 20);  /* cap one send at 1 MiB */
        ssize_t k = send(c->fd, s->buf + off, take, MSG_NOSIGNAL);
        if (k > 0) { c->cursor += (uint64_t)k; continue; }
        if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break; /* would block, try again later */
        if (k < 0 && errno == EINTR) continue;
        return -1;
    }
    return 0;
}

static void *srt_producer_thread(void *arg) {
    SRTSOCKET listener = *(SRTSOCKET*)arg;
    free(arg);

    while (!g_shutdown) {
        struct sockaddr_storage peer; int addrlen = sizeof(peer);
        SRTSOCKET s = srt_accept(listener, (struct sockaddr*)&peer, &addrlen);
        if (s == SRT_INVALID_SOCK) {
            if (g_shutdown) break;
            LOGE("srt_accept: %s", srt_getlasterror_str());
            sleep(1);
            continue;
        }

        /* Verify STREAMID matches configured camera. */
        char streamid[256] = {0};
        int  sidlen = sizeof(streamid) - 1;
        srt_getsockflag(s, SRTO_STREAMID, streamid, &sidlen);
        if (g_cfg.stream_id[0] && strcmp(streamid, g_cfg.stream_id) != 0) {
            LOGW("srt: rejecting connection with stream_id='%s' (expected '%s')",
                 streamid, g_cfg.stream_id);
            srt_close(s);
            continue;
        }

        char addrstr[64] = {0};
        if (peer.ss_family == AF_INET) {
            struct sockaddr_in *sin = (struct sockaddr_in*)&peer;
            inet_ntop(AF_INET, &sin->sin_addr, addrstr, sizeof(addrstr));
        }
        LOGI("srt: accepted from %s stream_id=%s sock=%d", addrstr, streamid, s);

        /* Replace any prior active connection.  Phase 1 only supports one. */
        pthread_mutex_lock(&g_stream.mutex);
        if (g_stream.srt_sock != SRT_INVALID_SOCK) {
            LOGW("srt: replacing prior provider socket %d", g_stream.srt_sock);
            srt_close(g_stream.srt_sock);
        }
        g_stream.srt_sock = s;
        /* Don't close existing HTTP clients on SRT reconnect — they're
         * still usable and we'll feed them the fresh FLV header from the
         * new session.  Reset their cursors so they replay the new header
         * cache, not the now-discarded old ring. */
        for (int i = 0; i < g_stream.n_clients; i++) {
            if (g_stream.clients[i].fd >= 0) {
                g_stream.clients[i].cursor       = 0;
                g_stream.clients[i].flv_hdr_sent = 0;
                g_stream.clients[i].flv_hdr_off  = 0;
                /* keep header_sent: we already wrote the HTTP/1.1 200 OK */
            }
        }
        g_stream.total_bytes = 0;
        g_stream.flv_header_len = 0;     /* fresh SRT session => fresh FLV header to cache */
        pthread_mutex_unlock(&g_stream.mutex);

        /* SRT recv loop — drains until peer disconnects or idle timeout.
         * Live mode requires srt_recvmsg; each call returns one message
         * (<= PAYLOADSIZE bytes). */
        uint8_t buf[2048];
        uint64_t bytes_in = 0;
        time_t   last_log = time(NULL);
        while (!g_shutdown) {
            int n = srt_recvmsg(s, (char*)buf, sizeof(buf));
            if (n == SRT_ERROR) {
                int rej;
                int rc = srt_getlasterror(&rej);
                LOGI("srt: recv ended (%s, errno=%d)", srt_getlasterror_str(), rc);
                break;
            }
            if (n == 0) { LOGI("srt: peer closed"); break; }
            bytes_in += (uint64_t)n;

            pthread_mutex_lock(&g_stream.mutex);
            /* Capture the first HEADER_CACHE_SIZE bytes of every fresh SRT
             * session so late-joining HTTP clients can be sent the FLV
             * file-header prefix before live data. */
            if (g_stream.flv_header_len < HEADER_CACHE_SIZE) {
                size_t room = HEADER_CACHE_SIZE - g_stream.flv_header_len;
                size_t take = (size_t)n < room ? (size_t)n : room;
                memcpy(g_stream.flv_header + g_stream.flv_header_len, buf, take);
                g_stream.flv_header_len += take;
            }
            ring_write(&g_stream, buf, (size_t)n);
            prune_lagged_clients_locked(&g_stream);
            for (int i = 0; i < g_stream.n_clients; i++) {
                http_client_t *c = &g_stream.clients[i];
                if (c->fd < 0) continue;
                if (drain_client_locked(&g_stream, c) != 0) {
                    LOGI("http: client fd=%d errored, dropping", c->fd);
                    close(c->fd);
                    c->fd = -1;
                }
            }
            compact_clients_locked(&g_stream);
            pthread_mutex_unlock(&g_stream.mutex);

            time_t now = time(NULL);
            if (now - last_log >= 1) {
                LOGI("srt: bytes_in=%llu n_clients=%d total=%llu",
                     (unsigned long long)bytes_in, g_stream.n_clients,
                     (unsigned long long)g_stream.total_bytes);
                last_log = now;
            }
        }

        pthread_mutex_lock(&g_stream.mutex);
        if (g_stream.srt_sock == s) g_stream.srt_sock = SRT_INVALID_SOCK;
        pthread_mutex_unlock(&g_stream.mutex);
        srt_close(s);
        LOGI("srt: connection closed (bytes_in=%llu)", (unsigned long long)bytes_in);
    }
    return NULL;
}

/* ------------------------------------------------------------------------ */
/*  HTTP listener: accept FLV pull clients                                  */
/* ------------------------------------------------------------------------ */

static int read_request_line(int fd, char *out, size_t outlen) {
    size_t off = 0;
    while (off + 1 < outlen) {
        char c;
        ssize_t k = recv(fd, &c, 1, 0);
        if (k <= 0) return -1;
        out[off++] = c;
        if (off >= 4 && out[off-4] == '\r' && out[off-3] == '\n' &&
            out[off-2] == '\r' && out[off-1] == '\n') {
            out[off] = 0;
            return (int)off;
        }
    }
    return -1;
}

static int parse_path_and_verify(const char *req, char *path_out, size_t pathlen,
                                 char *verify_out, size_t verifylen) {
    /* req starts with "GET <path> HTTP/1.1\r\n..." */
    if (strncmp(req, "GET ", 4) != 0) return -1;
    const char *p = req + 4;
    const char *e = strchr(p, ' ');
    if (!e) return -1;
    size_t fullen = (size_t)(e - p);
    if (fullen >= pathlen) return -1;
    char full[512];
    if (fullen >= sizeof(full)) return -1;
    memcpy(full, p, fullen); full[fullen] = 0;

    /* split path?query */
    char *q = strchr(full, '?');
    if (q) {
        *q = 0; q++;
        const char *vk = "verify=";
        const char *vp = strstr(q, vk);
        if (vp) {
            vp += strlen(vk);
            const char *ve = strchr(vp, '&');
            size_t vlen = ve ? (size_t)(ve - vp) : strlen(vp);
            if (vlen >= verifylen) vlen = verifylen - 1;
            memcpy(verify_out, vp, vlen); verify_out[vlen] = 0;
        }
    }
    snprintf(path_out, pathlen, "%s", full);
    return 0;
}

/* Per-client handshake worker: parse the GET, install into the fan-out, or
 * reject.  Runs in its own detached thread so a slow / silent client cannot
 * stall the accept loop.  Previously the parse ran inline and any client
 * that opened a TCP connection without sending data — including port scans
 * from the wider internet — would hang the whole HTTP listener forever. */
typedef struct { int fd; struct sockaddr_in peer; } http_handshake_arg_t;

static void *http_handshake_thread(void *arg) {
    http_handshake_arg_t *a = (http_handshake_arg_t*)arg;
    int cli = a->fd;
    char ip[32]; inet_ntop(AF_INET, &a->peer.sin_addr, ip, sizeof(ip));
    free(a);

    /* Hard cap on how long we'll wait for the client to send GET ... \r\n\r\n.
     * Anything past 5 s is almost certainly a port scanner or stuck NAT. */
    struct timeval to = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(cli, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));

    char req[2048];
    if (read_request_line(cli, req, sizeof(req)) <= 0) {
        LOGW("http: dropping %s — no/incomplete request within 5 s", ip);
        close(cli);
        return NULL;
    }

    char path[256] = {0}, verify[256] = {0};
    if (parse_path_and_verify(req, path, sizeof(path), verify, sizeof(verify)) != 0) {
        write_all(cli, HTTP_404, strlen(HTTP_404));
        close(cli); return NULL;
    }
    if (strcmp(path, g_cfg.http_path) != 0) {
        LOGW("http: 404 from %s path='%s'", ip, path);
        write_all(cli, HTTP_404, strlen(HTTP_404));
        close(cli); return NULL;
    }
    if (g_cfg.verify_token[0] && strcmp(verify, g_cfg.verify_token) != 0) {
        LOGW("http: 403 from %s verify='%s'", ip, verify);
        write_all(cli, HTTP_403, strlen(HTTP_403));
        close(cli); return NULL;
    }

    /* Reset RCVTIMEO and switch to nonblocking — drain_client_locked relies on
     * EAGAIN to avoid stalling the producer thread on a slow phone. */
    struct timeval no = { 0 }; setsockopt(cli, SOL_SOCKET, SO_RCVTIMEO, &no, sizeof(no));
    int flags = fcntl(cli, F_GETFL, 0);
    fcntl(cli, F_SETFL, flags | O_NONBLOCK);
    int sndbuf = 1 * 1024 * 1024;
    setsockopt(cli, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    pthread_mutex_lock(&g_stream.mutex);
    if (g_stream.n_clients >= MAX_HTTP_CLIENTS) {
        pthread_mutex_unlock(&g_stream.mutex);
        LOGW("http: 503 from %s (max clients)", ip);
        const char *r = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        write_all(cli, r, strlen(r));
        close(cli);
        return NULL;
    }
    http_client_t *slot = &g_stream.clients[g_stream.n_clients++];
    slot->fd          = cli;
    slot->cursor      = g_stream.total_bytes;
    slot->header_sent = 0;
    slot->flv_hdr_sent = 0;
    slot->flv_hdr_off  = 0;
    pthread_mutex_unlock(&g_stream.mutex);
    LOGI("http: client connected fd=%d from %s (total clients=%d)", cli, ip, g_stream.n_clients);
    return NULL;
}

static void *http_thread(void *arg) {
    int listen_fd = *(int*)arg;
    free(arg);

    while (!g_shutdown) {
        struct sockaddr_in peer; socklen_t plen = sizeof(peer);
        int cli = accept(listen_fd, (struct sockaddr*)&peer, &plen);
        if (cli < 0) {
            if (errno == EINTR) continue;
            if (g_shutdown) break;
            LOGE("http: accept: %s", strerror(errno));
            continue;
        }

        /* Hand off the per-client handshake to a detached thread so the
         * accept loop stays unblocked for the next client. */
        http_handshake_arg_t *a = malloc(sizeof(*a));
        a->fd = cli; a->peer = peer;
        pthread_t th;
        if (pthread_create(&th, NULL, http_handshake_thread, a) != 0) {
            LOGE("http: pthread_create failed: %s", strerror(errno));
            close(cli);
            free(a);
            continue;
        }
        pthread_detach(th);
    }
    return NULL;
}

/* ------------------------------------------------------------------------ */
/*  Main                                                                    */
/* ------------------------------------------------------------------------ */

static int make_tcp_listener(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = htons(port);
    if (bind(fd, (struct sockaddr*)&a, sizeof(a)) < 0) { close(fd); return -1; }
    if (listen(fd, HTTP_LISTEN_BACKLOG) < 0) { close(fd); return -1; }
    return fd;
}

static SRTSOCKET make_srt_listener(int port) {
    SRTSOCKET s = srt_create_socket();
    if (s == SRT_INVALID_SOCK) { LOGE("srt_create_socket: %s", srt_getlasterror_str()); return SRT_INVALID_SOCK; }
    if (set_srt_listener_options(s) != 0) { srt_close(s); return SRT_INVALID_SOCK; }
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = htons(port);
    if (srt_bind(s, (struct sockaddr*)&a, sizeof(a)) == SRT_ERROR) {
        LOGE("srt_bind :%d: %s", port, srt_getlasterror_str()); srt_close(s); return SRT_INVALID_SOCK;
    }
    if (srt_listen(s, 4) == SRT_ERROR) {
        LOGE("srt_listen :%d: %s", port, srt_getlasterror_str()); srt_close(s); return SRT_INVALID_SOCK;
    }
    return s;
}

static void on_signal(int sig) { (void)sig; g_shutdown = 1; }

int main(int argc, char **argv) {
    const char *cfg_path = "consumer.conf";
    if (argc >= 2) cfg_path = argv[1];
    if (load_config(cfg_path, &g_cfg) != 0) return 1;

    LOGI("consumer_srt: srt=:%d http=:%d stream=%s path=%s latency=%dms",
         g_cfg.srt_port, g_cfg.http_port, g_cfg.stream_id, g_cfg.http_path, g_cfg.latency_ms);

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    if (srt_startup() != 0) { LOGE("srt_startup: %s", srt_getlasterror_str()); return 1; }
    stream_init(&g_stream);

    SRTSOCKET srtl = make_srt_listener(g_cfg.srt_port);
    if (srtl == SRT_INVALID_SOCK) { srt_cleanup(); return 1; }
    int httpl = make_tcp_listener(g_cfg.http_port);
    if (httpl < 0) { LOGE("tcp listen :%d failed: %s", g_cfg.http_port, strerror(errno)); srt_close(srtl); srt_cleanup(); return 1; }

    pthread_t srt_th, http_th;
    SRTSOCKET *psrt = malloc(sizeof(SRTSOCKET)); *psrt = srtl;
    int       *phttp = malloc(sizeof(int));      *phttp = httpl;
    pthread_create(&srt_th,  NULL, srt_producer_thread, psrt);
    pthread_create(&http_th, NULL, http_thread,         phttp);

    while (!g_shutdown) sleep(1);

    LOGI("shutdown signaled");
    srt_close(srtl);
    close(httpl);
    pthread_join(srt_th,  NULL);
    pthread_join(http_th, NULL);
    srt_cleanup();
    LOGI("consumer_srt: shutdown complete");
    return 0;
}
