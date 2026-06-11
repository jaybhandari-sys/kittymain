/*
 * ap2p — srt_thread.c
 *
 * Phase A port of src/provider-srt/provider_srt.c into the v2.0 single-binary
 * scaffold.  The original file's main() + getconfig() are gone; everything
 * else (NAT discovery via the edge server, control-plane REGISTER + wait-for-
 * peer loop, libsrt rendezvous, HTTP source pump) lives below as
 * srt_thread_main().
 *
 * State integration:
 *   - Reads its 17 brand-neutral config values from the shared ap2p_state_t
 *     populated by the MQTT thread's case-81 handler (see config.c).
 *   - Blocks on ap2p_wait_state_ready() until the first case-81 payload
 *     lands.  Without this, an empty ctrl_host would cause a connect storm.
 *   - On each main-loop iteration calls ap2p_consume_srt_reload() — if a
 *     fresh case-81 arrived during the previous session, tears down and
 *     reruns with the new values.
 *
 * Brand-leak guard:  none of the conf-key string literals from the original
 * provider_srt.c (SIGNALING_HOST, STUN_HOST, TURN_HOST, SERVICE_ID, etc.)
 * appear in this file — config is consumed purely as struct fields.  Log
 * labels use the brand-neutral protocol terms ctrl: / edge: / relay: /
 * src: / srt: to keep `strings` output clean.
 */

#define _POSIX_C_SOURCE 200809L
#include "state.h"
#include "boot_timing.h"
/* stream_timing.h intentionally NOT included — see v2.0.6-rc2 revert note
 * below.  Inline MQTT publishes in the pump hot path were causing HEVC
 * keyframe drops; the module exists but is dormant until v2.0.6.2 lands
 * a non-blocking publish channel (set a flag, let MQTT thread ship the
 * JSON from its own idle loop). */

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
#include "edge_lite.h"   /* renamed from stun_lite.h to avoid brand-leak in path */

/* ------------------------------------------------------------------------ */
/*  Tunables                                                                */
/* ------------------------------------------------------------------------ */

#define LOCAL_BUFFER_SIZE       (256 * 1024)
#define SRT_PAYLOAD_SIZE        1316
#define DEFAULT_LATENCY_MS      300
#define RECONNECT_MIN_MS        1000
#define RECONNECT_MAX_MS        5000
#define LONG_SESSION_S          30
#define CONNECT_TIMEOUT_MS      6000
#define PEER_IDLE_TIMEOUT_MS    8000

/* ------------------------------------------------------------------------ */
/*  Module-local snapshot of the runtime config — refreshed at the top of   */
/*  each control session so a mid-session case-81 reload picks up fresh     */
/*  values automatically without ripping data out from underneath libsrt    */
/*  callbacks that were already passed pointers.                            */
/* ------------------------------------------------------------------------ */

typedef struct {
    char     ctrl_host[128];
    int      ctrl_port;
    char     ctrl_scheme[8];
    char     ctrl_token[128];
    char     edge_host[128];
    int      edge_port;
    char     relay_host[128];
    int      relay_port;
    char     relay_user[128];
    char     relay_pass[128];
    char     src_host[128];
    int      src_port;
    char     src_path[512];
    char     src_auth[256];
    char     node_id[256];
    int      latency_ms;
    int      verbose;
} srt_cfg_t;

static srt_cfg_t g_cfg;
static ap2p_state_t *g_state = NULL;

static void snapshot_config(ap2p_state_t *state, srt_cfg_t *out) {
    pthread_mutex_lock(&state->lock);
    snprintf(out->ctrl_host,   sizeof(out->ctrl_host),   "%s", state->ctrl_host);
    out->ctrl_port = state->ctrl_port;
    snprintf(out->ctrl_scheme, sizeof(out->ctrl_scheme), "%s",
             state->ctrl_scheme[0] ? state->ctrl_scheme : "plain");
    snprintf(out->ctrl_token,  sizeof(out->ctrl_token),  "%s", state->ctrl_token);
    snprintf(out->edge_host,   sizeof(out->edge_host),   "%s", state->edge_host);
    out->edge_port = state->edge_port > 0 ? state->edge_port : 3478;
    snprintf(out->relay_host,  sizeof(out->relay_host),  "%s", state->relay_host);
    out->relay_port = state->relay_port > 0 ? state->relay_port : 3478;
    snprintf(out->relay_user,  sizeof(out->relay_user),  "%s", state->relay_user);
    snprintf(out->relay_pass,  sizeof(out->relay_pass),  "%s", state->relay_pass);
    snprintf(out->src_host,    sizeof(out->src_host),
             "%s", state->src_host[0] ? state->src_host : "127.0.0.1");
    out->src_port = state->src_port > 0 ? state->src_port : 80;
    snprintf(out->src_path,    sizeof(out->src_path),    "%s", state->src_path);
    snprintf(out->src_auth,    sizeof(out->src_auth),    "%s", state->src_auth);
    snprintf(out->node_id,     sizeof(out->node_id),     "%s", state->node_id);
    out->latency_ms = state->latency_ms > 0 ? state->latency_ms : DEFAULT_LATENCY_MS;
    out->verbose    = state->verbose;
    pthread_mutex_unlock(&state->lock);
}

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

static inline int shutdown_requested(void) {
    return g_state && g_state->shutdown_flag;
}

/* ------------------------------------------------------------------------ */
/*  Length-prefixed control-plane protocol (plain TCP).  TLS path from the  */
/*  original provider_srt.c is intentionally deferred to Phase B in the     */
/*  v2.0 single-binary layout — cert paths need to land in the broker push  */
/*  payload first, and the brand-leak audit hasn't yet vetted the OpenSSL   */
/*  banner-string footprint of the libcrypto.so.3 symbol set.               */
/* ------------------------------------------------------------------------ */

typedef struct sig_io {
    int   fd;
} ctrl_io_t;

static ssize_t ctrl_read_all(ctrl_io_t *sig, void *buf, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t k = recv(sig->fd, (char*)buf + off, n - off, MSG_WAITALL);
        if (k <= 0) return -1;
        off += (size_t)k;
    }
    return (ssize_t)off;
}

static ssize_t ctrl_write_all(ctrl_io_t *sig, const void *buf, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t k = send(sig->fd, (const char*)buf + off, n - off, 0);
        if (k <= 0) return -1;
        off += (size_t)k;
    }
    return (ssize_t)off;
}

static int ctrl_send(ctrl_io_t *sig, const char *msg) {
    size_t len = strlen(msg);
    uint32_t nlen = htonl((uint32_t)len);
    if (ctrl_write_all(sig, &nlen, 4) != 4) return -1;
    if (ctrl_write_all(sig, msg, len) != (ssize_t)len) return -1;
    return 0;
}

static int ctrl_recv(ctrl_io_t *sig, char *out, size_t cap) {
    uint32_t nlen;
    if (ctrl_read_all(sig, &nlen, 4) != 4) return -1;
    uint32_t len = ntohl(nlen);
    if (len >= cap) return -1;
    if (ctrl_read_all(sig, out, len) != (ssize_t)len) return -1;
    out[len] = 0;
    return (int)len;
}

static int ctrl_recv_timed(ctrl_io_t *sig, char *out, size_t cap, int timeout_ms) {
    struct pollfd pfd = { .fd = sig->fd, .events = POLLIN };
    int pr = poll(&pfd, 1, timeout_ms);
    if (pr < 0) { if (errno == EINTR) return 0; return -1; }
    if (pr == 0) return 0;
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return -1;
    return ctrl_recv(sig, out, cap);
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
/*  TCP control-plane connect with keepalive                                */
/* ------------------------------------------------------------------------ */

static int ctrl_tcp_connect(const char *host, int port) {
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    char portstr[8]; snprintf(portstr, sizeof(portstr), "%d", port);
    struct addrinfo *ai = NULL;
    if (getaddrinfo(host, portstr, &hints, &ai) != 0) return -1;
    int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) { freeaddrinfo(ai); return -1; }
    if (connect(fd, ai->ai_addr, ai->ai_addrlen) < 0) {
        LOGE("ctrl: connect %s:%d failed: %s", host, port, strerror(errno));
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

static ctrl_io_t *ctrl_open(const srt_cfg_t *cfg) {
    ctrl_io_t *sig = (ctrl_io_t*)calloc(1, sizeof(*sig));
    if (!sig) return NULL;
    sig->fd = ctrl_tcp_connect(cfg->ctrl_host, cfg->ctrl_port);
    if (sig->fd < 0) { free(sig); return NULL; }
    LOGI("ctrl: connected %s:%d (plain)", cfg->ctrl_host, cfg->ctrl_port);
    return sig;
}

static void ctrl_close(ctrl_io_t *sig) {
    if (!sig) return;
    if (sig->fd >= 0) close(sig->fd);
    free(sig);
}

/* ------------------------------------------------------------------------ */
/*  Local HTTP source fetch (camera firmware → bytes)                       */
/* ------------------------------------------------------------------------ */

static int connect_src(const char *host, int port) {
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    char portstr[8]; snprintf(portstr, sizeof(portstr), "%d", port);
    struct addrinfo *ai = NULL;
    if (getaddrinfo(host, portstr, &hints, &ai) != 0) return -1;
    int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) { freeaddrinfo(ai); return -1; }
    if (connect(fd, ai->ai_addr, ai->ai_addrlen) < 0) {
        LOGE("src: connect %s:%d failed: %s", host, port, strerror(errno));
        close(fd); freeaddrinfo(ai); return -1;
    }
    freeaddrinfo(ai);
    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    int rcvbuf = 4 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    return fd;
}

static int send_http_get(int src_fd, const srt_cfg_t *cfg) {
    char req[1024];
    int n = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\nHost: %s:%d\r\nUser-Agent: ap2p/2.0\r\n"
        "Accept: */*\r\nConnection: keep-alive\r\n%s%s%s\r\n",
        cfg->src_path, cfg->src_host, cfg->src_port,
        cfg->src_auth[0] ? "Authorization: " : "",
        cfg->src_auth, cfg->src_auth[0] ? "\r\n" : "");
    if (n <= 0 || n >= (int)sizeof(req)) return -1;
    if (send(src_fd, req, (size_t)n, 0) != n) return -1;
    return 0;
}

static int read_http_response_headers(int src_fd, uint8_t *buf, size_t cap,
                                      size_t *body_off, size_t *body_len) {
    size_t total = 0;
    int prev_was_le = 0, saw_double = 0;
    while (total < cap) {
        ssize_t k = recv(src_fd, buf + total, cap - total, 0);
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
        LOGE("src: status=%d head: %.*s", status, (int)print, buf);
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
    int payload    = SRT_PAYLOAD_SIZE;
    int conn_to    = CONNECT_TIMEOUT_MS;
    int idle_to    = PEER_IDLE_TIMEOUT_MS;
    int transtype  = SRTT_FILE;
    int messageapi = 0;
    if (srt_setsockopt(s, 0, SRTO_TRANSTYPE,     &transtype, sizeof(transtype))   != 0) return -1;
    if (srt_setsockopt(s, 0, SRTO_MESSAGEAPI,    &messageapi, sizeof(messageapi)) != 0) return -1;
    if (srt_setsockopt(s, 0, SRTO_RENDEZVOUS,    &yes,     sizeof(yes))     != 0) return -1;
    if (srt_setsockopt(s, 0, SRTO_REUSEADDR,     &yes,     sizeof(yes))     != 0) return -1;
    if (srt_setsockopt(s, 0, SRTO_PAYLOADSIZE,   &payload, sizeof(payload)) != 0) return -1;
    if (srt_setsockopt(s, 0, SRTO_CONNTIMEO,     &conn_to, sizeof(conn_to)) != 0) return -1;
    if (srt_setsockopt(s, 0, SRTO_PEERIDLETIMEO, &idle_to, sizeof(idle_to)) != 0) return -1;
    if (g_cfg.node_id[0]) {
        if (srt_setsockopt(s, 0, SRTO_STREAMID, g_cfg.node_id,
                           (int)strlen(g_cfg.node_id)) != 0) return -1;
    }
    return 0;
}

/* v2.0.7-rc5 — adopts an EXISTING UDP socket via srt_bind_acquire().
 *
 * Previously this function called srt_create_socket + srt_bind, which
 * fails ("unable to create/configure SRT socket") when the kernel UDP
 * port is already held by the STUN-refresh discovery socket (rc3+).
 * The right libsrt-native answer is srt_bind_acquire(s, fd): we already
 * own a UDP fd that is bound to local_port AND has a refreshed NAT
 * mapping → libsrt takes ownership of THAT fd and uses it for the SRT
 * pump.  After srt_close, libsrt closes the underlying fd; the caller
 * must NOT close it separately.
 *
 * Lifecycle contract (enforced by the caller in run_ctrl_session):
 *   - On entry: `existing_udp_fd` is a bound, STUN-refreshed UDP socket
 *     on `local_port`.  Caller must NOT close it (libsrt owns it now).
 *   - On success: returns an SRTSOCKET that wraps the same fd.  After
 *     srt_close(returned), the fd is closed.  Caller should re-allocate
 *     a fresh discovery socket for the next viewer.
 *   - On failure: this function closes existing_udp_fd before returning
 *     (defensive — we don't want the caller stuck owning a half-handed-
 *     off fd).  Returns SRT_INVALID_SOCK.
 */
static SRTSOCKET srt_rendezvous_connect(int existing_udp_fd, int local_port,
                                        const char *peer_ip, int peer_port) {
    SRTSOCKET s = srt_create_socket();
    if (s == SRT_INVALID_SOCK) {
        LOGE("srt_create_socket: %s", srt_getlasterror_str());
        close(existing_udp_fd);
        return SRT_INVALID_SOCK;
    }
    if (set_srt_options_caller(s) != 0) {
        LOGE("srt_setsockopt: %s", srt_getlasterror_str());
        srt_close(s);
        close(existing_udp_fd);
        return SRT_INVALID_SOCK;
    }
    /* Hand the existing kernel UDP fd to libsrt — fd ownership transfers.
     * libsrt's CUDT::bind detects the fd is already bound and skips its
     * own bind step, reusing the kernel port + NAT mapping we maintained
     * during STUN refresh. */
    if (srt_bind_acquire(s, existing_udp_fd) == SRT_ERROR) {
        LOGE("srt_bind_acquire(fd=%d, :%d): %s",
             existing_udp_fd, local_port, srt_getlasterror_str());
        srt_close(s);
        close(existing_udp_fd);
        return SRT_INVALID_SOCK;
    }
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM };
    char portstr[8]; snprintf(portstr, sizeof(portstr), "%d", peer_port);
    struct addrinfo *ai = NULL;
    if (getaddrinfo(peer_ip, portstr, &hints, &ai) != 0) {
        LOGE("srt: peer getaddrinfo failed");
        srt_close(s); return SRT_INVALID_SOCK;
    }
    LOGI("srt: rendezvous connect %s:%d (local %d, adopted fd)",
         peer_ip, peer_port, local_port);
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
    while (off < n && !shutdown_requested()) {
        int chunk = (int)(n - off);
        if (chunk > SRT_PAYLOAD_SIZE) chunk = SRT_PAYLOAD_SIZE;
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
/*  Pump-worker plumbing — runs the SRT rendezvous + HTTP-source drain.     */
/*  The control thread posts pump requests; the pump worker consumes them.  */
/* ------------------------------------------------------------------------ */

typedef struct {
    char  peer_ip[64];
    int   peer_port;
    int   local_port;
    char  my_srflx_ip[64];
    /* v2.0.7-rc5 — handed-off UDP fd that libsrt will adopt via
     * srt_bind_acquire().  Set by run_ctrl_session at SRT_PEER handoff;
     * consumed by the pump thread.  -1 = no fd handoff (legacy path /
     * error guard — pump must NOT proceed if this is -1). */
    int   udp_fd;
} pump_request_t;

static pthread_mutex_t  g_pump_mtx     = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   g_pump_cv      = PTHREAD_COND_INITIALIZER;
static pump_request_t   g_pending_pump;
static int              g_pending_valid = 0;
static atomic_int       g_pump_cancel  = 0;
static atomic_int       g_pump_running = 0;
static SRTSOCKET        g_active_srt   = SRT_INVALID_SOCK;
static pthread_t        g_pump_tid;
static int              g_pump_started = 0;

static int pump_src_to_srt(int src_fd, SRTSOCKET srt_sock) {
    uint8_t *buf = malloc(LOCAL_BUFFER_SIZE);
    if (!buf) return -1;
    uint64_t bytes_in = 0, bytes_out = 0;
    time_t   last_log = time(NULL);

    if (g_cfg.src_path[0]) {
        if (send_http_get(src_fd, &g_cfg) != 0) { free(buf); return -1; }
        size_t body_off = 0, body_len = 0;
        if (read_http_response_headers(src_fd, buf, LOCAL_BUFFER_SIZE,
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

    int src_flags = fcntl(src_fd, F_GETFL, 0);
    fcntl(src_fd, F_SETFL, src_flags | O_NONBLOCK);

    while (!shutdown_requested() && !atomic_load(&g_pump_cancel)) {
        struct pollfd pfd = { .fd = src_fd, .events = POLLIN };
        int pr = poll(&pfd, 1, 200);
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (pr == 0) continue;
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) break;
        ssize_t n = recv(src_fd, buf, LOCAL_BUFFER_SIZE, 0);
        if (n == 0) { LOGI("src: peer closed"); break; }
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            LOGE("src recv: %s", strerror(errno)); break;
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
        LOGI("pump: cancelled by ctrl thread (new peer arrived)");
    }
    return 0;
}

static void *pump_thread_main(void *arg) {
    (void)arg;
    while (!shutdown_requested()) {
        pthread_mutex_lock(&g_pump_mtx);
        while (!g_pending_valid && !shutdown_requested()) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 1;
            pthread_cond_timedwait(&g_pump_cv, &g_pump_mtx, &ts);
        }
        if (shutdown_requested()) { pthread_mutex_unlock(&g_pump_mtx); break; }
        pump_request_t req = g_pending_pump;
        g_pending_valid = 0;
        atomic_store(&g_pump_cancel, 0);
        atomic_store(&g_pump_running, 1);
        pthread_mutex_unlock(&g_pump_mtx);

        /* v2.0.7-rc5 — require the udp_fd handoff from run_ctrl_session.
         * If it's -1 (somehow we got here without a handoff), refuse the
         * rendezvous rather than create a fresh socket that would fight
         * for the same port. */
        if (req.udp_fd < 0) {
            LOGE("pump: SRT_PEER request without udp_fd handoff — refusing rendezvous");
            atomic_store(&g_pump_running, 0);
            continue;
        }
        SRTSOCKET s = srt_rendezvous_connect(req.udp_fd, req.local_port,
                                              req.peer_ip, req.peer_port);
        if (s != SRT_INVALID_SOCK) {
            pthread_mutex_lock(&g_pump_mtx);
            g_active_srt = s;
            pthread_mutex_unlock(&g_pump_mtx);

            int src_fd = connect_src(g_cfg.src_host, g_cfg.src_port);
            if (src_fd >= 0) {
                pump_src_to_srt(src_fd, s);
                close(src_fd);
            } else {
                LOGE("pump: connect_src failed; aborting session");
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

static void post_pump_request(const pump_request_t *req) {
    pthread_mutex_lock(&g_pump_mtx);
    atomic_store(&g_pump_cancel, 1);
    if (g_active_srt != SRT_INVALID_SOCK) {
        srt_close(g_active_srt);
        g_active_srt = SRT_INVALID_SOCK;
    }
    g_pending_pump  = *req;
    g_pending_valid = 1;
    pthread_cond_signal(&g_pump_cv);
    pthread_mutex_unlock(&g_pump_mtx);
}

/* ------------------------------------------------------------------------ */
/*  Edge (NAT-discovery) socket helpers — moral equivalent of the old STUN  */
/*  bind path; renamed to keep the brand-leak audit happy.                  */
/* ------------------------------------------------------------------------ */

static int allocate_edge_socket(int wanted_port) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    /* v2.0.7-rc5 — SO_REUSEPORT is REQUIRED for the rc3 keep-discovery-
     * socket-open-for-NAT-refresh design to coexist with the SRT pump
     * binding the SAME local port at peer-arrival time.
     *
     * SO_REUSEADDR alone only helps with TIME_WAIT cleanup; two LIVE
     * sockets sharing a UDP port on Linux REQUIRES SO_REUSEPORT on both.
     * Without it, srt_rendezvous_connect → srt_bind(:local_port) fails
     * with "unable to create/configure SRT socket" the moment a viewer
     * arrives, and the camera silently rejects the rendezvous.
     *
     * libsrt sets SO_REUSEADDR + SO_REUSEPORT via SRTO_REUSEADDR (see
     * set_srt_options_caller), so as long as WE also set REUSEPORT on
     * the discovery socket, the two coexist.  Effect: discovery socket
     * keeps refreshing the NAT mapping at port P, SRT socket binds at
     * port P, both deliver to the kernel which steers packets by
     * 4-tuple — exactly what rendezvous expects. */
#ifdef SO_REUSEPORT
    setsockopt(s, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
#endif
    struct sockaddr_in la = {0};
    la.sin_family = AF_INET; la.sin_addr.s_addr = htonl(INADDR_ANY);
    la.sin_port = htons(wanted_port);
    if (bind(s, (struct sockaddr*)&la, sizeof(la)) == 0) return s;
    int err = errno;
    LOGW("edge bind :%d failed (%s) — falling back to ephemeral", wanted_port, strerror(err));
    close(s);
    s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#ifdef SO_REUSEPORT
    setsockopt(s, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
#endif
    la.sin_port = htons(0);
    if (bind(s, (struct sockaddr*)&la, sizeof(la)) < 0) {
        LOGE("edge bind ephemeral failed: %s", strerror(errno));
        close(s); return -1;
    }
    return s;
}

static int get_local_port(int sockfd) {
    struct sockaddr_in la; socklen_t lalen = sizeof(la);
    if (getsockname(sockfd, (struct sockaddr*)&la, &lalen) != 0) return -1;
    return ntohs(la.sin_port);
}

/* ------------------------------------------------------------------------ */
/*  Control session — one full registration + wait-for-peer loop.            */
/*  Returns when the control TCP dies, when a reload was requested, or when */
/*  shutdown was tripped.  Caller decides whether to retry.                  */
/* ------------------------------------------------------------------------ */

static int run_ctrl_session(void) {
    int udp = allocate_edge_socket(0);
    if (udp < 0) return -1;
    int local_port = get_local_port(udp);

    ap2p_edge_binding_t srflx;
    if (ap2p_edge_get_srflx(udp, g_cfg.edge_host, g_cfg.edge_port, &srflx, 2) != 0) {
        LOGE("edge: discovery failed"); close(udp); return -1;
    }
    char lan_ip[64] = {0};
    ap2p_edge_get_lan_ip(lan_ip, sizeof(lan_ip));
    bt_mark(BT_EDGE_SRFLX);
    LOGI("edge: SRFLX=%s:%d  LAN=%s:%d  (this is our reachable address)",
         srflx.srflx_ip, srflx.srflx_port, lan_ip, local_port);
    /* v2.0.7-rc3 — DO NOT close(udp) here.  The original code closed the
     * discovery socket before waiting for a peer, which leaves the
     * camera's NAT mapping unrefreshed.  Most consumer / carrier NATs
     * expire idle UDP flows in 30-120 s, well before any viewer arrives;
     * by the time srt_rendezvous_connect() re-binds to local_port later
     * the public port has shifted and the SRFLX we registered with
     * signaling is stale → viewer's srt_connect() times out.
     *
     * Keep the socket open until the end of run_ctrl_session().  The
     * periodic STUN refresh in the loop below sends UDP packets through
     * it every ~25 s, which keeps the NAT mapping fresh.  The SRT pump
     * (srt_rendezvous_connect) still creates its own libsrt socket on
     * the same local_port — but because the NAT mapping is being
     * actively refreshed (and SO_REUSEADDR is set), the public port
     * stays stable across the close-then-srt_bind window.
     *
     * Trade-off: one persistent UDP socket per ctrl session (cheap).
     * On exit (loop break) we close it before returning. */

    ctrl_io_t *sig = ctrl_open(&g_cfg);
    if (!sig) { close(udp); return -1; }

    char regmsg[1024];
    snprintf(regmsg, sizeof(regmsg),
        "type=SRT_REGISTER\nservice_id=%s\nsrflx_ip=%s\nsrflx_port=%d\n%s%s%s%s%s%s",
        g_cfg.node_id, srflx.srflx_ip, srflx.srflx_port,
        lan_ip[0] ? "lan_ip=" : "", lan_ip, lan_ip[0] ? "\n" : "",
        g_cfg.ctrl_token[0] ? "api_token=" : "",
        g_cfg.ctrl_token,
        g_cfg.ctrl_token[0] ? "\n" : "");
    if (ctrl_send(sig, regmsg) < 0) {
        LOGE("ctrl: send register failed"); ctrl_close(sig); close(udp); return -1;
    }
    char inbuf[8192];
    if (ctrl_recv(sig, inbuf, sizeof(inbuf)) <= 0) {
        LOGE("ctrl: no register-ack"); ctrl_close(sig); close(udp); return -1;
    }
    char resp_type[64]; kv_get(inbuf, "type", resp_type, sizeof(resp_type));
    if (strcmp(resp_type, "SRT_REGISTERED") != 0) {
        LOGE("ctrl: expected SRT_REGISTERED, got '%s'", resp_type);
        ctrl_close(sig); close(udp); return -1;
    }
    bt_mark(BT_CTRL_REGISTERED);
    LOGI("ctrl: registered as %s", g_cfg.node_id);
    /* GO-LIVE reached — one-shot summary publish to torque/tx/<sn>/boot.
     * mqtt_client_handle is read directly; bt_publish_summary is idempotent
     * so transient reconnects won't double-publish. */
    bt_publish_summary(g_state ? g_state->mqtt_client_handle : NULL,
                       g_cfg.node_id);

    const int SIG_PING_INTERVAL_MS = 10000;
    char pingbuf[256];
    snprintf(pingbuf, sizeof(pingbuf),
        "type=PING\nservice_id=%s\n", g_cfg.node_id);

    /* v2.0.7-rc3 — periodic STUN refresh to keep the UDP NAT mapping
     * alive AND notice if the SRFLX shifts (NAT rebind, ISP DHCP).
     *
     * Refresh every 3rd TCP-ping tick (≈ every 30 s) — gives us a fresh
     * STUN binding well before any commodity-NAT 60 s idle timer can
     * fire.  If the new SRFLX differs from what we registered, send a
     * SRT_REGISTER update so signaling has the current address before
     * the next viewer arrives.  Without this, the camera registers ONCE
     * at boot and never refreshes — any NAT rebind after that and every
     * viewer hits a dead port. */
    int refresh_tick = 0;

    while (!shutdown_requested()) {
        if (ap2p_consume_srt_reload(g_state)) {
            LOGI("ctrl: reload requested — closing session, rebuilding");
            ctrl_close(sig);
            close(udp);
            return 0;
        }
        int got = ctrl_recv_timed(sig, inbuf, sizeof(inbuf), SIG_PING_INTERVAL_MS);
        if (got < 0) { LOGW("ctrl: TCP closed by peer"); break; }
        if (got == 0) {
            if (ctrl_send(sig, pingbuf) != 0) {
                LOGW("ctrl: keepalive send failed — TCP must be dead");
                break;
            }
            LOGD("ctrl: keepalive PING sent");

            /* Every 3rd PING (≈ 30 s) → STUN refresh + maybe re-register. */
            if (++refresh_tick >= 3) {
                refresh_tick = 0;
                ap2p_edge_binding_t fresh;
                if (ap2p_edge_get_srflx(udp, g_cfg.edge_host,
                                         g_cfg.edge_port, &fresh, 2) == 0) {
                    /* STUN packet itself keeps the NAT mapping alive
                     * even if SRFLX hasn't changed. */
                    if (strcmp(fresh.srflx_ip,  srflx.srflx_ip) != 0 ||
                        fresh.srflx_port != srflx.srflx_port) {
                        LOGI("edge: SRFLX changed %s:%d → %s:%d — re-registering",
                             srflx.srflx_ip, srflx.srflx_port,
                             fresh.srflx_ip, fresh.srflx_port);
                        srflx = fresh;
                        char reg2[1024];
                        snprintf(reg2, sizeof(reg2),
                            "type=SRT_REGISTER\nservice_id=%s\n"
                            "srflx_ip=%s\nsrflx_port=%d\n%s%s%s%s%s%s",
                            g_cfg.node_id,
                            srflx.srflx_ip, srflx.srflx_port,
                            lan_ip[0] ? "lan_ip=" : "", lan_ip,
                            lan_ip[0] ? "\n" : "",
                            g_cfg.ctrl_token[0] ? "api_token=" : "",
                            g_cfg.ctrl_token,
                            g_cfg.ctrl_token[0] ? "\n" : "");
                        if (ctrl_send(sig, reg2) < 0) {
                            LOGW("ctrl: re-register send failed");
                        }
                    } else {
                        LOGD("edge: STUN refresh — SRFLX stable %s:%d",
                             srflx.srflx_ip, srflx.srflx_port);
                    }
                } else {
                    LOGW("edge: STUN refresh failed — NAT may close binding");
                }
            }
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
            req.udp_fd = -1;   /* explicit "no handoff yet" sentinel */
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

            /* v2.0.7-rc5 — HAND OFF the discovery UDP fd to the pump
             * thread.  libsrt will acquire it via srt_bind_acquire()
             * (see srt_rendezvous_connect).  After this point WE no
             * longer own the fd; libsrt closes it on srt_close().
             *
             * Why this is better than the rc4 attempt (close-then-rebind):
             *   - No port-conflict race (one fd, ownership transferred).
             *   - No brief NAT-mapping gap — the kernel-level UDP socket
             *     is continuously open from STUN-refresh era through SRT
             *     pump era.
             *   - NAT mapping that the STUN-refresh loop kept alive is
             *     preserved exactly because the fd is the same.
             */
            if (udp < 0) {
                LOGE("SRT_PEER: no discovery UDP fd to hand off — skipping");
                continue;
            }
            req.udp_fd = udp;
            udp = -1;       /* ownership transferred; do NOT close here */
            post_pump_request(&req);

            /* Reallocate a fresh discovery socket for the next viewer.
             * Gets an ephemeral port (likely different from local_port);
             * re-bind via STUN; if SRFLX changed, send a fresh
             * SRT_REGISTER so signaling has the new address.  Allows
             * back-to-back viewer arrivals without restarting the
             * ctrl session. */
            udp = allocate_edge_socket(0);
            if (udp >= 0) {
                local_port = get_local_port(udp);
                ap2p_edge_binding_t fresh;
                if (ap2p_edge_get_srflx(udp, g_cfg.edge_host,
                                         g_cfg.edge_port, &fresh, 2) == 0) {
                    if (strcmp(fresh.srflx_ip,  srflx.srflx_ip) != 0 ||
                        fresh.srflx_port != srflx.srflx_port) {
                        srflx = fresh;
                        char reg2[1024];
                        snprintf(reg2, sizeof(reg2),
                            "type=SRT_REGISTER\nservice_id=%s\n"
                            "srflx_ip=%s\nsrflx_port=%d\n%s%s%s%s%s%s",
                            g_cfg.node_id,
                            srflx.srflx_ip, srflx.srflx_port,
                            lan_ip[0] ? "lan_ip=" : "", lan_ip,
                            lan_ip[0] ? "\n" : "",
                            g_cfg.ctrl_token[0] ? "api_token=" : "",
                            g_cfg.ctrl_token,
                            g_cfg.ctrl_token[0] ? "\n" : "");
                        if (ctrl_send(sig, reg2) >= 0) {
                            LOGI("edge: rotated to SRFLX %s:%d after SRT_PEER handoff",
                                 srflx.srflx_ip, srflx.srflx_port);
                        }
                    }
                }
            } else {
                LOGW("edge: discovery socket realloc failed — NAT refresh disabled "
                     "until next ctrl-session rebuild");
            }
        } else if (strcmp(t, "PING") == 0) {
            const char *pong = "type=PONG\n";
            ctrl_send(sig, pong);
        } else {
            LOGD("ctrl: ignoring type=%s", t);
        }
    }
    ctrl_close(sig);
    close(udp);                   /* v2.0.7-rc3 — release the NAT-keepalive UDP socket */
    return 0;
}

/* ------------------------------------------------------------------------ */
/*  srt_thread entry point — invoked from main.c via pthread_create.         */
/* ------------------------------------------------------------------------ */

void *srt_thread_main(void *state_arg) {
    fprintf(stderr, "[ap2p:trace] srt_thread_main: entered\n"); fflush(stderr);
    bt_mark(BT_SRT_STARTED);
    ap2p_state_t *state = (ap2p_state_t *)state_arg;
    if (!state) return NULL;
    g_state = state;

    /* Block until the MQTT thread has applied the first case-81 payload. */
    int wr = ap2p_wait_state_ready(state, -1);
    if (wr != 0) return NULL;  /* shutdown or error */
    snapshot_config(state, &g_cfg);
    LOGI("srt_thread: ready node_id=%s ctrl=%s:%d edge=%s:%d src=%s:%d",
         g_cfg.node_id,
         g_cfg.ctrl_host, g_cfg.ctrl_port,
         g_cfg.edge_host, g_cfg.edge_port,
         g_cfg.src_host, g_cfg.src_port);

    signal(SIGPIPE, SIG_IGN);
    if (srt_startup() != 0) { LOGE("srt_startup failed"); return NULL; }

    if (!g_pump_started) {
        if (pthread_create(&g_pump_tid, NULL, pump_thread_main, NULL) != 0) {
            LOGE("pthread_create pump: %s", strerror(errno));
            srt_cleanup();
            return NULL;
        }
        g_pump_started = 1;
    }

    int backoff = RECONNECT_MIN_MS;
    while (!shutdown_requested()) {
        /* Re-snapshot at the top of each session so a mid-loop case-81
         * reload (or its drained state set via ap2p_consume_srt_reload
         * inside run_ctrl_session) picks up the fresh values. */
        snapshot_config(state, &g_cfg);
        time_t t0 = time(NULL);
        run_ctrl_session();
        if (shutdown_requested()) break;
        time_t lasted = time(NULL) - t0;
        if (lasted >= LONG_SESSION_S) backoff = RECONNECT_MIN_MS;
        LOGI("ctrl: TCP died (lasted %lds); reconnecting in %d ms", (long)lasted, backoff);
        struct timespec ts = { backoff / 1000, (backoff % 1000) * 1000000L };
        nanosleep(&ts, NULL);
        backoff *= 2;
        if (backoff > RECONNECT_MAX_MS) backoff = RECONNECT_MAX_MS;
    }

    pthread_cond_broadcast(&g_pump_cv);
    if (g_pump_started) pthread_join(g_pump_tid, NULL);
    srt_cleanup();
    return NULL;
}
