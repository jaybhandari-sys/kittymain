/* Consumer (phone-side), real P2P SRT.
 *
 * Phase 2 of the SRT pipeline.  Runs INSIDE the Android app as a subprocess
 * (packaged as libnative_consumer_srt.so so the Android package manager
 * extracts it to nativeLibraryDir, which is mounted exec).  Same trick the
 * existing libjuice consumer_api binary uses.
 *
 * Single-stream design: one process = one camera.  Spawn a fresh process
 * per camera the user is viewing.  Cmdline args (no config file — easier
 * for the Kotlin spawner):
 *
 *   consumer_srt_p2p \
 *       --signaling=HOST:PORT  \
 *       --stun=HOST:PORT       \
 *       --service-id=ID        \
 *       --listen=127.0.0.1:PORT
 *
 * Flow:
 *   1. UDP socket + STUN bind to coturn → SRFLX
 *   2. TCP signaling → send SRT_REQUEST + our SRFLX
 *   3. Receive SRT_PROVIDER (camera's SRFLX) + TURN creds for fallback
 *   4. Close UDP socket; immediately srt_bind to same local port with
 *      RENDEZVOUS=1 + srt_connect to camera's SRFLX
 *   5. Once SRT is up, serve received bytes as HTTP-FLV on the listen
 *      address so Jessibuca / WebView can pull from localhost.
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
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "srt/srt.h"
#include "stun_lite.h"

#define SRT_PAYLOAD_SIZE        1316
#define DEFAULT_LATENCY_MS      300
#define CONNECT_TIMEOUT_MS      6000
#define PEER_IDLE_TIMEOUT_MS    30000   /* Was SRT default 5 s; mobile-data NAT churn / brief
                                         * radio handoff easily stalls peer ACKs > 5 s and was
                                         * killing streams every ~13 s.  30 s gives ARQ recovery
                                         * time without prematurely declaring the camera dead. */
#define RING_SIZE               (4 * 1024 * 1024)
#define HEADER_CACHE_SIZE       2048
#define MAX_HTTP_CLIENTS        4

/* ------------------------------------------------------------------------ */

static struct {
    char signaling_host[128]; int signaling_port;
    char stun_host[128];      int stun_port;
    char service_id[256];
    char listen_ip[64];       int listen_port;
    int  latency_ms;
    int  verbose;
    char http_path[128];
} g_cfg;

static volatile int g_shutdown = 0;

/* ------------------------------------------------------------------------ */

static void log_msg(const char *level, const char *fmt, ...) {
    char ts[32]; struct timespec now; clock_gettime(CLOCK_REALTIME, &now);
    struct tm tm; localtime_r(&now.tv_sec, &tm);
    strftime(ts, sizeof(ts), "%H:%M:%S", &tm);
    fprintf(stdout, "[%s.%03ld] [%s] ", ts, now.tv_nsec / 1000000, level);
    va_list ap; va_start(ap, fmt); vfprintf(stdout, fmt, ap); va_end(ap);
    fputc('\n', stdout); fflush(stdout);
}
#define LOGI(...) log_msg("INFO ",  __VA_ARGS__)
#define LOGW(...) log_msg("WARN ",  __VA_ARGS__)
#define LOGE(...) log_msg("ERROR",  __VA_ARGS__)

/* ------------------------------------------------------------------------ */
/*  Length-prefixed signaling protocol                                       */
/* ------------------------------------------------------------------------ */

static int sig_send(int fd, const char *msg) {
    size_t len = strlen(msg);
    uint32_t nlen = htonl((uint32_t)len);
    if (send(fd, &nlen, 4, 0) != 4) return -1;
    if (send(fd, msg, len, 0) != (ssize_t)len) return -1;
    return 0;
}

static int sig_recv(int fd, char *out, size_t cap) {
    uint32_t nlen;
    if (recv(fd, &nlen, 4, MSG_WAITALL) != 4) return -1;
    uint32_t len = ntohl(nlen);
    if (len >= cap) return -1;
    if (recv(fd, out, len, MSG_WAITALL) != (ssize_t)len) return -1;
    out[len] = 0;
    return (int)len;
}

static int kv_get(const char *msg, const char *key, char *out, size_t cap) {
    size_t klen = strlen(key);
    const char *p = msg;
    while (*p) {
        const char *eol = strchr(p, '\n'); if (!eol) eol = p + strlen(p);
        if ((size_t)(eol - p) > klen + 1 && memcmp(p, key, klen) == 0 && p[klen] == '=') {
            size_t vlen = (size_t)(eol - p) - klen - 1;
            if (vlen >= cap) vlen = cap - 1;
            memcpy(out, p + klen + 1, vlen); out[vlen] = 0;
            return (int)vlen;
        }
        if (!*eol) break;
        p = eol + 1;
    }
    out[0] = 0;
    return -1;
}

/* ------------------------------------------------------------------------ */
/*  Stream state (ring buffer + HTTP fan-out — same shape as consumer_srt.c) */
/* ------------------------------------------------------------------------ */

typedef struct {
    int      fd;
    uint64_t cursor;
    int      header_sent;
    int      flv_hdr_sent;
    size_t   flv_hdr_off;
} http_client_t;

static struct {
    pthread_mutex_t mutex;
    uint8_t *buf; size_t cap;
    uint64_t total_bytes;
    http_client_t clients[MAX_HTTP_CLIENTS];
    int n_clients;
    uint8_t  flv_header[HEADER_CACHE_SIZE];
    size_t   flv_header_len;
    int      eof;        /* SRT closed -- close all clients */
} g_stream;

static void stream_init(void) {
    pthread_mutex_init(&g_stream.mutex, NULL);
    g_stream.buf = malloc(RING_SIZE);
    g_stream.cap = RING_SIZE;
    g_stream.total_bytes = 0;
    g_stream.n_clients = 0;
    g_stream.flv_header_len = 0;
    g_stream.eof = 0;
    for (int i = 0; i < MAX_HTTP_CLIENTS; i++) g_stream.clients[i].fd = -1;
}

/* ------------------------------------------------------------------------ */
/*  HTTP server (same logic as consumer_srt.c, condensed)                    */
/* ------------------------------------------------------------------------ */

static const char *HTTP_FLV_HDR =
    "HTTP/1.1 200 OK\r\nContent-Type: video/x-flv\r\nConnection: close\r\n"
    "Cache-Control: no-cache\r\nAccess-Control-Allow-Origin: *\r\n\r\n";

static int write_all(int fd, const void *p, size_t n) {
    const uint8_t *b = (const uint8_t*)p;
    while (n > 0) {
        ssize_t k = send(fd, b, n, MSG_NOSIGNAL);
        if (k > 0) { b += k; n -= (size_t)k; continue; }
        if (k < 0 && errno == EINTR) continue;
        return -1;
    }
    return 0;
}

static void ring_write(const uint8_t *data, size_t n) {
    size_t off = g_stream.total_bytes % g_stream.cap;
    size_t first = (off + n <= g_stream.cap) ? n : g_stream.cap - off;
    memcpy(g_stream.buf + off, data, first);
    if (first < n) memcpy(g_stream.buf, data + first, n - first);
    g_stream.total_bytes += n;
}

static int drain_client_locked(http_client_t *c) {
    if (!c->header_sent) {
        if (write_all(c->fd, HTTP_FLV_HDR, strlen(HTTP_FLV_HDR)) != 0) return -1;
        c->header_sent = 1;
    }
    if (!c->flv_hdr_sent && g_stream.flv_header_len > 0) {
        while (c->flv_hdr_off < g_stream.flv_header_len) {
            ssize_t k = send(c->fd, g_stream.flv_header + c->flv_hdr_off,
                             g_stream.flv_header_len - c->flv_hdr_off, MSG_NOSIGNAL);
            if (k > 0) { c->flv_hdr_off += (size_t)k; continue; }
            if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
            if (k < 0 && errno == EINTR) continue;
            return -1;
        }
        c->flv_hdr_sent = 1;
        /* Race fix: at connect time, cursor was set to flv_header_len's value
         * AT THAT MOMENT — but the cache keeps growing on subsequent SRT
         * recvs (up to HEADER_CACHE_SIZE).  After the cache loop above sends
         * the now-grown cache, the ring loop below would re-send the bytes
         * in [cursor .. flv_hdr_off), producing a DUPLICATE FLV-header
         * fragment in the byte stream and permanently desyncing the FLV
         * demuxer.  Advance cursor past the cached prefix so the ring loop
         * never serves bytes that came from cache. */
        if (c->cursor < c->flv_hdr_off) c->cursor = c->flv_hdr_off;
    }
    while (c->cursor < g_stream.total_bytes) {
        uint64_t pending = g_stream.total_bytes - c->cursor;
        size_t off = c->cursor % g_stream.cap;
        size_t take = (off + pending <= g_stream.cap) ? (size_t)pending : (g_stream.cap - off);
        if (take > (1 << 20)) take = (1 << 20);
        ssize_t k = send(c->fd, g_stream.buf + off, take, MSG_NOSIGNAL);
        if (k > 0) { c->cursor += (uint64_t)k; continue; }
        if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (k < 0 && errno == EINTR) continue;
        return -1;
    }
    return 0;
}

static void prune_locked(void) {
    int j = 0;
    for (int i = 0; i < g_stream.n_clients; i++) {
        if (g_stream.clients[i].fd >= 0) {
            if (i != j) g_stream.clients[j] = g_stream.clients[i];
            j++;
        }
    }
    for (int k = j; k < g_stream.n_clients; k++) g_stream.clients[k].fd = -1;
    g_stream.n_clients = j;
}

typedef struct { int fd; struct sockaddr_in peer; } http_conn_t;

static int read_request_line(int fd, char *out, size_t cap) {
    size_t off = 0;
    while (off + 1 < cap) {
        char c; ssize_t k = recv(fd, &c, 1, 0);
        if (k <= 0) return -1;
        out[off++] = c;
        if (off >= 4 && out[off-4] == '\r' && out[off-3] == '\n'
                     && out[off-2] == '\r' && out[off-1] == '\n') {
            out[off] = 0; return (int)off;
        }
    }
    return -1;
}

static void *http_handshake_thread(void *arg) {
    http_conn_t *a = (http_conn_t*)arg;
    int cli = a->fd; free(a);

    struct timeval to = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(cli, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));

    char req[2048];
    if (read_request_line(cli, req, sizeof(req)) <= 0) { close(cli); return NULL; }
    if (strncmp(req, "GET ", 4) != 0) { close(cli); return NULL; }
    /* Path check is intentionally lax — Jessibuca will pull whatever URL
     * we hand it.  We just want to make sure the client is HTTP-shaped. */

    struct timeval no = { 0 };
    setsockopt(cli, SOL_SOCKET, SO_RCVTIMEO, &no, sizeof(no));
    int flags = fcntl(cli, F_GETFL, 0);
    fcntl(cli, F_SETFL, flags | O_NONBLOCK);
    int sndbuf = 1 * 1024 * 1024;
    setsockopt(cli, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    pthread_mutex_lock(&g_stream.mutex);
    if (g_stream.n_clients >= MAX_HTTP_CLIENTS) {
        pthread_mutex_unlock(&g_stream.mutex);
        close(cli); return NULL;
    }
    http_client_t *slot = &g_stream.clients[g_stream.n_clients++];
    slot->fd = cli;
    /* The drain logic first replays the cached FLV file-header prefix
     * (first flv_header_len bytes of the stream), THEN drains the ring
     * from cursor to total_bytes.  If we set cursor to total_bytes we'd
     * skip the entire interval [flv_header_len, total_bytes) — the AVC
     * config tag and first keyframe live in there, so the player would
     * never get a syncable stream.  Set cursor to flv_header_len so the
     * client receives:
     *   1. HTTP/1.1 200 OK headers
     *   2. cached FLV file-header bytes [0, flv_header_len)
     *   3. ring bytes [flv_header_len, total_bytes) — contains AVC config + keyframes
     *   4. live bytes as they arrive
     * For very long streams where total_bytes > ring.cap, bytes that have
     * already rolled out of the ring are lost; in practice the 4 MiB ring
     * keeps ~30+ s of stream which is plenty for a fresh connect. */
    slot->cursor = g_stream.flv_header_len;
    slot->header_sent = 0;
    slot->flv_hdr_sent = 0;
    slot->flv_hdr_off = 0;
    pthread_mutex_unlock(&g_stream.mutex);
    LOGI("http: client connected fd=%d (n=%d)", cli, g_stream.n_clients);
    return NULL;
}

static void *http_listen_thread(void *arg) {
    int listen_fd = *(int*)arg; free(arg);
    while (!g_shutdown) {
        struct sockaddr_in peer; socklen_t plen = sizeof(peer);
        int cli = accept(listen_fd, (struct sockaddr*)&peer, &plen);
        if (cli < 0) { if (errno == EINTR) continue; if (g_shutdown) break; continue; }
        http_conn_t *a = malloc(sizeof(*a));
        a->fd = cli; a->peer = peer;
        pthread_t th;
        pthread_create(&th, NULL, http_handshake_thread, a);
        pthread_detach(th);
    }
    return NULL;
}

/* ------------------------------------------------------------------------ */
/*  SRT rendezvous + recv loop                                              */
/* ------------------------------------------------------------------------ */

static int set_srt_options(SRTSOCKET s) {
    int yes = 1;
    int payload = SRT_PAYLOAD_SIZE;
    int conn_to = CONNECT_TIMEOUT_MS;
    int idle_to = PEER_IDLE_TIMEOUT_MS;
    /* SRTT_FILE = reliable byte stream (no message boundaries, no latency-
     * budget drops).  We MUST use this for FLV-over-SRT — LIVE mode drops
     * messages whose ARQ retransmit exceeds RCVLATENCY, which would punch
     * a hole in the byte stream mid-FLV-tag and permanently desync the
     * demuxer.  FILE mode retransmits indefinitely, preserving every byte. */
    int transtype = SRTT_FILE;
    int messageapi = 0;
    if (srt_setsockopt(s, 0, SRTO_TRANSTYPE,     &transtype, sizeof(transtype)) != 0) return -1;
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
    if (s == SRT_INVALID_SOCK) { LOGE("srt_create_socket: %s", srt_getlasterror_str()); return s; }
    if (set_srt_options(s) != 0) { LOGE("srt_setsockopt"); srt_close(s); return SRT_INVALID_SOCK; }
    struct sockaddr_in la = {0};
    la.sin_family = AF_INET; la.sin_addr.s_addr = htonl(INADDR_ANY);
    la.sin_port = htons(local_port);
    if (srt_bind(s, (struct sockaddr*)&la, sizeof(la)) == SRT_ERROR) {
        LOGE("srt_bind :%d: %s", local_port, srt_getlasterror_str());
        srt_close(s); return SRT_INVALID_SOCK;
    }
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM };
    char ps[8]; snprintf(ps, sizeof(ps), "%d", peer_port);
    struct addrinfo *ai = NULL;
    if (getaddrinfo(peer_ip, ps, &hints, &ai) != 0) { srt_close(s); return SRT_INVALID_SOCK; }
    LOGI("srt: rendezvous connect %s:%d (local %d)", peer_ip, peer_port, local_port);
    int rc = srt_connect(s, ai->ai_addr, (int)ai->ai_addrlen);
    freeaddrinfo(ai);
    if (rc == SRT_ERROR) {
        LOGE("srt_connect: %s", srt_getlasterror_str());
        srt_close(s); return SRT_INVALID_SOCK;
    }
    LOGI("srt: rendezvous handshake complete");
    return s;
}

static void *srt_recv_thread(void *arg) {
    SRTSOCKET s = *(SRTSOCKET*)arg; free(arg);
    uint8_t buf[2048];
    uint64_t bytes_in = 0;
    time_t last_log = time(NULL);
    while (!g_shutdown) {
        /* SRTT_FILE byte-stream API — srt_recv (NOT srt_recvmsg).  This
         * returns whatever bytes are currently available in the receive
         * buffer (1..sizeof(buf)), with strict in-order, no-loss delivery. */
        int n = srt_recv(s, (char*)buf, sizeof(buf));
        if (n == SRT_ERROR) { LOGI("srt: recv ended (%s)", srt_getlasterror_str()); break; }
        if (n == 0) { LOGI("srt: peer closed"); break; }
        bytes_in += (uint64_t)n;

        pthread_mutex_lock(&g_stream.mutex);
        if (g_stream.flv_header_len < HEADER_CACHE_SIZE) {
            size_t room = HEADER_CACHE_SIZE - g_stream.flv_header_len;
            size_t take = (size_t)n < room ? (size_t)n : room;
            memcpy(g_stream.flv_header + g_stream.flv_header_len, buf, take);
            g_stream.flv_header_len += take;
        }
        ring_write(buf, (size_t)n);
        for (int i = 0; i < g_stream.n_clients; i++) {
            http_client_t *c = &g_stream.clients[i];
            if (c->fd < 0) continue;
            if (drain_client_locked(c) != 0) {
                close(c->fd); c->fd = -1;
            }
        }
        prune_locked();
        pthread_mutex_unlock(&g_stream.mutex);

        time_t now = time(NULL);
        if (now - last_log >= 5) {
            LOGI("srt: bytes_in=%llu n_clients=%d",
                 (unsigned long long)bytes_in, g_stream.n_clients);
            last_log = now;
        }
    }
    pthread_mutex_lock(&g_stream.mutex);
    g_stream.eof = 1;
    for (int i = 0; i < g_stream.n_clients; i++) {
        if (g_stream.clients[i].fd >= 0) {
            close(g_stream.clients[i].fd);
            g_stream.clients[i].fd = -1;
        }
    }
    g_stream.n_clients = 0;
    pthread_mutex_unlock(&g_stream.mutex);
    g_shutdown = 1;
    return NULL;
}

/* ------------------------------------------------------------------------ */
/*  Bootstrap                                                                */
/* ------------------------------------------------------------------------ */

static int allocate_udp_socket(int wanted_port) {
    int s = socket(AF_INET, SOCK_DGRAM, 0); if (s < 0) return -1;
    int yes = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in la = {0};
    la.sin_family = AF_INET; la.sin_addr.s_addr = htonl(INADDR_ANY);
    la.sin_port = htons(wanted_port);
    if (bind(s, (struct sockaddr*)&la, sizeof(la)) < 0) { close(s); return -1; }
    return s;
}

static int get_local_port(int s) {
    struct sockaddr_in la; socklen_t lalen = sizeof(la);
    if (getsockname(s, (struct sockaddr*)&la, &lalen) != 0) return -1;
    return ntohs(la.sin_port);
}

static int signaling_connect(const char *host, int port) {
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    char ps[8]; snprintf(ps, sizeof(ps), "%d", port);
    struct addrinfo *ai = NULL;
    if (getaddrinfo(host, ps, &hints, &ai) != 0) return -1;
    int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) { freeaddrinfo(ai); return -1; }
    if (connect(fd, ai->ai_addr, ai->ai_addrlen) < 0) {
        LOGE("signaling: connect %s:%d failed: %s", host, port, strerror(errno));
        close(fd); freeaddrinfo(ai); return -1;
    }
    freeaddrinfo(ai);
    return fd;
}

static int parse_args(int argc, char **argv) {
    g_cfg.signaling_port = 8888;
    g_cfg.stun_port      = 3478;
    g_cfg.latency_ms     = DEFAULT_LATENCY_MS;
    snprintf(g_cfg.listen_ip, sizeof(g_cfg.listen_ip), "127.0.0.1");
    snprintf(g_cfg.http_path, sizeof(g_cfg.http_path), "/flv/live.flv");

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = strchr(a, '='); if (!v) continue;
        v++;
        if      (!strncmp(a, "--signaling=", 12)) {
            char host[128], *colon;
            snprintf(host, sizeof(host), "%s", v);
            colon = strrchr(host, ':');
            if (colon) { *colon = 0; g_cfg.signaling_port = atoi(colon + 1); }
            snprintf(g_cfg.signaling_host, sizeof(g_cfg.signaling_host), "%s", host);
        }
        else if (!strncmp(a, "--stun=", 7)) {
            char host[128], *colon;
            snprintf(host, sizeof(host), "%s", v);
            colon = strrchr(host, ':');
            if (colon) { *colon = 0; g_cfg.stun_port = atoi(colon + 1); }
            snprintf(g_cfg.stun_host, sizeof(g_cfg.stun_host), "%s", host);
        }
        else if (!strncmp(a, "--service-id=", 13)) {
            snprintf(g_cfg.service_id, sizeof(g_cfg.service_id), "%s", v);
        }
        else if (!strncmp(a, "--listen=", 9)) {
            char host[128], *colon;
            snprintf(host, sizeof(host), "%s", v);
            colon = strrchr(host, ':');
            if (colon) { *colon = 0; g_cfg.listen_port = atoi(colon + 1); }
            snprintf(g_cfg.listen_ip, sizeof(g_cfg.listen_ip), "%s", host);
        }
        else if (!strncmp(a, "--latency-ms=", 13)) g_cfg.latency_ms = atoi(v);
        else if (!strncmp(a, "--verbose=", 10))    g_cfg.verbose    = atoi(v);
        else if (!strncmp(a, "--http-path=", 12))  snprintf(g_cfg.http_path, sizeof(g_cfg.http_path), "%s", v);
    }
    if (!g_cfg.signaling_host[0] || !g_cfg.stun_host[0] || !g_cfg.service_id[0] || g_cfg.listen_port <= 0) {
        fprintf(stderr,
            "usage: %s --signaling=HOST:PORT --stun=HOST:PORT --service-id=ID --listen=IP:PORT\n",
            argv[0]);
        return -1;
    }
    return 0;
}

static int make_tcp_listener(const char *ip, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0); if (fd < 0) return -1;
    int yes = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = inet_addr(ip);
    a.sin_port = htons(port);
    if (bind(fd, (struct sockaddr*)&a, sizeof(a)) < 0) { close(fd); return -1; }
    if (listen(fd, 4) < 0) { close(fd); return -1; }
    return fd;
}

static void on_signal(int s) { (void)s; g_shutdown = 1; }

int main(int argc, char **argv) {
    if (parse_args(argc, argv) != 0) return 1;

    /* Critical: when the Android Java process (our parent) dies — whether
     * the user force-quit, Android low-memory-killed it, or it crashed —
     * make sure THIS subprocess dies too.  Without PR_SET_PDEATHSIG the
     * subprocess gets reparented to init (PPID=1) and keeps running with
     * the SRT connection to the camera intact.  When the user reopens the
     * app, ConsumerSrtP2P sees no live subprocess in its in-memory map
     * and spawns a NEW one, which races the orphan for the camera.  The
     * camera ends up streaming to the orphan; the new subprocess receives
     * mid-stream bytes (no FLV file header) and Jessibuca fires
     * p2pFetchError.  PR_SET_PDEATHSIG SIGKILL fixes this at the kernel
     * level — orphans terminate immediately and never compete.
     *
     * Linux-specific; Android is Linux so it's fine.  prctl returns 0 on
     * success, -1 on failure (we silently ignore failure on systems that
     * don't support it). */
    prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0);
    /* Note: we used to early-exit if getppid()==1 (orphan race-guard for the
     * case where Android's JVM died between fork and exec, leaving us
     * reparented to init).  That guard is wrong for daemonized launches —
     * `nohup ... &` and `setsid` both reparent to init by design.  The
     * PR_SET_PDEATHSIG above is the real protection: any FUTURE parent
     * death sends us SIGKILL.  An already-dead parent at startup is
     * indistinguishable from intentional daemonization, so we just keep
     * running. */

    LOGI("consumer_srt_p2p: signaling=%s:%d stun=%s:%d service=%s listen=%s:%d",
         g_cfg.signaling_host, g_cfg.signaling_port,
         g_cfg.stun_host, g_cfg.stun_port,
         g_cfg.service_id, g_cfg.listen_ip, g_cfg.listen_port);

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);
    if (srt_startup() != 0) { LOGE("srt_startup failed"); return 1; }
    stream_init();

    /* 1. UDP socket + STUN binding to discover SRFLX */
    int udp = allocate_udp_socket(0);
    if (udp < 0) { LOGE("udp bind failed"); return 1; }
    int local_port = get_local_port(udp);
    LOGI("stun: bound local UDP port %d", local_port);

    stun_binding_t srflx;
    if (stun_get_srflx(udp, g_cfg.stun_host, g_cfg.stun_port, &srflx, 2) != 0) {
        LOGE("stun: discovery failed"); close(udp); return 1;
    }
    LOGI("stun: SRFLX = %s:%d", srflx.srflx_ip, srflx.srflx_port);

    /* Also discover phone's LAN IPv4 so the server can give it to the
     * camera; both sides use LAN candidates when their SRFLX matches
     * (same-NAT case — most consumer routers don't hairpin). */
    char my_lan_ip[64] = {0};
    if (stun_get_lan_ip(my_lan_ip, sizeof(my_lan_ip)) == 0) {
        LOGI("stun: LAN candidate = %s:%d", my_lan_ip, local_port);
    }

    /* 2. TCP signaling: SRT_REQUEST */
    int sig = signaling_connect(g_cfg.signaling_host, g_cfg.signaling_port);
    if (sig < 0) { close(udp); return 1; }
    char msg[2048];
    int mn = snprintf(msg, sizeof(msg),
        "type=SRT_REQUEST\nservice_id=%s\nsrflx_ip=%s\nsrflx_port=%d\n%s%s%s",
        g_cfg.service_id, srflx.srflx_ip, srflx.srflx_port,
        my_lan_ip[0] ? "lan_ip=" : "", my_lan_ip, my_lan_ip[0] ? "\n" : "");
    (void)mn;
    if (sig_send(sig, msg) < 0) { LOGE("signaling: send failed"); close(sig); close(udp); return 1; }

    /* 3. Receive SRT_PROVIDER */
    char inbuf[8192];
    if (sig_recv(sig, inbuf, sizeof(inbuf)) <= 0) { LOGE("signaling: no reply"); close(sig); close(udp); return 1; }
    char t[64]; kv_get(inbuf, "type", t, sizeof(t));
    if (strcmp(t, "SRT_PROVIDER") != 0) {
        char emsg[256]; kv_get(inbuf, "message", emsg, sizeof(emsg));
        LOGE("signaling: %s — %s", t, emsg);
        close(sig); close(udp); return 1;
    }
    char peer_srflx_ip[64]; char peer_srflx_port_str[16]; char peer_lan_ip[64];
    kv_get(inbuf, "srflx_ip",   peer_srflx_ip, sizeof(peer_srflx_ip));
    kv_get(inbuf, "srflx_port", peer_srflx_port_str, sizeof(peer_srflx_port_str));
    kv_get(inbuf, "lan_ip",     peer_lan_ip, sizeof(peer_lan_ip));
    int peer_port = atoi(peer_srflx_port_str);
    if (!peer_srflx_ip[0] || peer_port <= 0) {
        LOGE("SRT_PROVIDER missing fields"); close(sig); close(udp); return 1;
    }

    /* Pick the right peer address: when our SRFLX equals the camera's SRFLX
     * we're on the same NAT, and most NATs do not hairpin (a packet to your
     * own public IP gets dropped instead of forwarded back inside).  Use
     * the LAN IP in that case.  Cross-NAT keeps SRFLX. */
    char peer_ip[64];
    if (strcmp(srflx.srflx_ip, peer_srflx_ip) == 0 && peer_lan_ip[0]) {
        snprintf(peer_ip, sizeof(peer_ip), "%s", peer_lan_ip);
        LOGI("signaling: SRT_PROVIDER same-NAT detected (SRFLX %s:%d shared); "
             "switching to peer LAN %s:%d",
             peer_srflx_ip, peer_port, peer_ip, peer_port);
    } else {
        snprintf(peer_ip, sizeof(peer_ip), "%s", peer_srflx_ip);
        LOGI("signaling: SRT_PROVIDER %s:%d (cross-NAT)", peer_ip, peer_port);
    }
    close(sig);

    /* 4. Hand UDP port off to SRT */
    close(udp);
    SRTSOCKET s = srt_rendezvous_connect(local_port, peer_ip, peer_port);
    if (s == SRT_INVALID_SOCK) { LOGE("srt: rendezvous failed"); return 1; }

    /* 5. Start HTTP-FLV listener + SRT recv */
    int http_listen = make_tcp_listener(g_cfg.listen_ip, g_cfg.listen_port);
    if (http_listen < 0) { LOGE("http listen :%d failed: %s", g_cfg.listen_port, strerror(errno)); srt_close(s); return 1; }
    LOGI("http: listening on %s:%d%s", g_cfg.listen_ip, g_cfg.listen_port, g_cfg.http_path);

    pthread_t srt_th, http_th;
    SRTSOCKET *ps = malloc(sizeof(SRTSOCKET)); *ps = s;
    int       *ph = malloc(sizeof(int));      *ph = http_listen;
    pthread_create(&srt_th,  NULL, srt_recv_thread, ps);
    pthread_create(&http_th, NULL, http_listen_thread, ph);

    pthread_join(srt_th, NULL);
    /* SRT closed -- shut everything down */
    g_shutdown = 1;
    close(http_listen);
    pthread_join(http_th, NULL);
    srt_close(s);
    srt_cleanup();
    LOGI("consumer_srt_p2p: exit");
    return 0;
}
