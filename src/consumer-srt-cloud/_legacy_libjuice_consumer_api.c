// Multi-Camera VMS Consumer with Persistent Signaling
// Modified from original P2P Consumer to support multiple simultaneous camera connections
// Each camera gets its own ICE agent, connections, and local listen port
//
// ============================================================================
// STUN / TURN / ICE Protocol Reference
// ============================================================================
// STUN (RFC 5389 — Session Traversal Utilities for NAT):
//   - Uses Binding Request / Binding Response exchange (informally "ping-pong")
//   - Helps discover external (public) IP and port via Server Reflexive (SRFLX)
//   - Clients periodically send STUN Binding Requests to keep NAT mappings alive
//     (the server only responds — it never initiates keepalives)
//   - Default port: 3478
//
// TURN (RFC 5766 — Traversal Using Relays around NAT):
//   - A separate protocol that *extends* STUN (not "STUN over TCP/TLS")
//   - Used as a fallback when direct peer-to-peer connection fails
//   - Allocates a relay address on the TURN server to forward media
//   - CreatePermission expires ~300s; must be refreshed proactively
//   - Default port: 3478 (shared with STUN)
//
// ICE (RFC 8445 — Interactive Connectivity Establishment):
//   - Orchestrates STUN/TURN to find the best connectivity path
//   - Candidate priority: HOST > SRFLX > PRFLX > RELAY
//   - libjuice handles ICE candidate gathering and connectivity checks
//
// Redirection / Retry Safety:
//   - Clients should avoid repeatedly contacting the same server to prevent
//     loops or unnecessary retries (REDIRECTION_COOLDOWN enforces this)
// ============================================================================

#include "juice/juice.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <time.h>
#include <netdb.h>
#include <signal.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/resource.h>

FILE* g_log_file = NULL;
#define printf(...) do { \
    fprintf(stdout, __VA_ARGS__); fflush(stdout); \
    if (g_log_file) { fprintf(g_log_file, __VA_ARGS__); fflush(g_log_file); } \
} while(0)

#define BUFFER_SIZE 4194304    /* 4 MB — covers H.265/HEVC 4K keyframes and HTTP snapshot frames */
#define MAX_SDP_LEN 4096
#define MAX_UDP_PAYLOAD 1100   /* reduced from 1200 to account for TURN relay overhead */
#define MAX_CONNECTIONS 64
#define MAX_CAMERAS 32
#define REASSEMBLY_TIMEOUT 30
#define KEEPALIVE_INTERVAL 5
/* PONG_TIMEOUT: with libjuice consent-freshness disabled, libjuice no longer
 * tears the path down on missing STUN responses, so this app-level PING/PONG
 * watchdog is the primary liveness check.  KEEPALIVE_INTERVAL=5 s means a
 * 60 s window represents 12 lost PONGs in a row — enough to ride out routine
 * mobile/CGNAT loss bursts without spurious reconnects.  Two strikes are
 * required, so worst-case detection of a TRULY dead path is 120 s (and
 * any single PONG inside that window resets the counter).  */
#define PONG_TIMEOUT 60
#define PONG_TIMEOUT_STRIKES 2     /* consecutive PONG_TIMEOUT windows before reconnect */
#define REDIRECTION_COOLDOWN 60   /* Avoid retrying same TURN/STUN server within this window (seconds) */
#define HEADER_SIZE 16         /* conn_id(4)+seq(4)+total_size(4)+offset(4) */
#define CONFIG_LINE_MAX 256
#define MAX_CHUNK_PAYLOAD (MAX_UDP_PAYLOAD - HEADER_SIZE)                /* 1184 */
#define MAX_CHUNKS_PER_FRAME ((BUFFER_SIZE / MAX_CHUNK_PAYLOAD) + 2)    /* ~444 */
/* Dispatch queue sizing — DOUBLED from 32→64 / 32MB→64MB so longer transient
 * client stalls (heavy keyframe + concurrent multi-pane decode + WebView GC)
 * are absorbed without forcing a TCP teardown.  Each forced teardown is a
 * visible 0.5–2 s stutter as the FLV parser re-syncs.
 *
 *   - Slot count (64) is a safety cap.  At 30 fps, 64 frames = ~2 s backlog.
 *     Further buffering hurts latency without helping a CCTV live stream
 *     that only cares about the newest frame.
 *   - Byte cap (64 MB) is the real limit.  Each slot can hold up to 4 MB
 *     (BUFFER_SIZE), so 64 slots × 4 MB = 256 MB worst-case footprint, but
 *     in practice the byte cap fires first at 16 keyframe-sized frames.
 *     We drop the oldest frame whenever EITHER the slot or the byte cap
 *     is exceeded.  Worth monitoring memory on low-end Android hardware
 *     when running 4-pane multi mode (4 × 64 MB = 256 MB potential). */
#define DISPATCH_QUEUE_SIZE         64
#define DISPATCH_QUEUE_MAX_BYTES    (64U * 1024U * 1024U)

// Configuration structure
typedef struct {
    int p2p_port_base;              // Base port for P2P (e.g., 9200)
    int local_listen_port_base;     // Base port for local listen (e.g., 8000)
    char signaling_server[256];
    int signaling_port;
    // TURN server configuration
    char turn_server_host[256];
    uint16_t turn_server_port;
    char turn_server_username[256];
    char turn_server_password[256];
    int turn_server_enabled;
    // STUN-upgrade probe interval (seconds). 0 = disabled.
    // When connected via TURN relay, the consumer periodically attempts a fresh
    // ICE negotiation to check if a direct/STUN path has become available.
    // A successful STUN Binding Request/Response (RFC 5389) discovers the public
    // IP, enabling direct P2P without the relay overhead.
    // Set low (e.g. 120) when consumer has a direct internet path.
    // Set 0 when consumer is in Docker/NAT that always needs TURN.
    int stun_upgrade_interval;
    // TURN proactive refresh interval (seconds). 0 = disabled.
    // TURN (RFC 5766) CreatePermission expires at ~300s (coturn default).
    // Refreshing at 240s avoids the "Lost connectivity" dropout caused by
    // permission expiry. TURN extends STUN to relay traffic — it is a separate
    // protocol, not merely "STUN over TCP/TLS".
    int turn_refresh_interval;
    // REST API authentication token. If non-empty, all API requests (except /health)
    // must include the header: X-API-Token: <value>
    // Set in consumer.conf: API_TOKEN=your-secret-token
    char api_token[128];
    // CORS allowed origin for REST API. Empty = no CORS header (most secure).
    // Set in consumer.conf: CORS_ORIGIN=https://your-dashboard.example.com
    char cors_origin[256];
} config_t;

// Pending send item (produced by ICE callback, consumed by dispatch thread)
typedef struct {
    int      fd;       /* dup'd local socket fd */
    uint8_t *data;     /* heap-allocated frame  */
    size_t   size;
} pending_send_t;

// Lock-protected bounded queue used to decouple ICE callback from send()
typedef struct {
    pending_send_t   items[DISPATCH_QUEUE_SIZE];
    int              head, tail;
    size_t           bytes_queued;      /* running total of item.size in queue */
    uint64_t         drop_count;        /* frames dropped since last log flush */
    uint64_t         drop_bytes;        /* bytes dropped since last log flush  */
    time_t           last_drop_log;     /* rate-limit drop warnings to 1 Hz    */
    pthread_mutex_t  mutex;
    pthread_cond_t   cond;
} dispatch_queue_t;

// Connection structure (per camera connection)
typedef struct {
    uint32_t conn_id;
    int local_socket;
    pthread_t forward_thread;
    int active;
    uint8_t  *reassembly_buffer;   /* heap-allocated (BUFFER_SIZE bytes) */
    uint8_t  *spare_buffer;        /* pre-allocated standby; ICE callback swaps */
                                   /* it in when a frame completes so the 4 MB */
                                   /* memcpy never runs inside cam->mutex.      */
    uint8_t  *chunk_received;      /* heap-allocated (MAX_CHUNKS_PER_FRAME bytes) */
    uint32_t  expected_size;       /* total frame size announced by sender */
    uint32_t  bytes_received;      /* bytes accumulated so far (out-of-order safe) */
    uint32_t  last_seq_num;
    uint32_t  current_frame_start_seq;   /* seq_num of the offset==0 chunk for the frame currently reassembling */
    uint32_t  current_frame_num_chunks;  /* ceil(expected_size / MAX_CHUNK_PAYLOAD); used to reject stale-frame chunks */
    int       frame_in_progress;         /* 1 while reassembling, 0 between frames */
    time_t    last_activity;
} connection_t;

// Camera session structure (one per camera)
typedef struct {
    char camera_id[128];            // e.g., "cam001"
    juice_agent_t *agent;           // ICE agent for this camera
    int connected;                  // P2P connection status
    int local_listen_port;          // Port for this camera's stream
    int listen_socket;              // Socket for this camera
    char service_protocol[16];      // "rtsp" or "rtsps"
    char service_codec[16];         // "h264" or "h265"
    pthread_t accept_thread;        // Accept thread for this camera
    pthread_t cleanup_thread;       // Cleanup thread
    pthread_t keepalive_thread;     // Keepalive thread
    pthread_t dispatch_thread;      // Sends completed frames without blocking ICE
    volatile int dispatch_running;  // Set to 1 by dispatch thread at entry, 0 at exit (watchdog)
    pthread_mutex_t dispatch_respawn_lock; // Guards respawn race from concurrent enqueue callers
    dispatch_queue_t dispatch_queue;// Frame queue between ICE callback and dispatch
    pthread_mutex_t mutex;          // Mutex for this camera
    connection_t connections[MAX_CONNECTIONS];  // Connections for this camera
    uint32_t next_conn_id;
    uint32_t global_seq_num;
    time_t connection_start_time;
    int keepalive_enabled;
    int active;                     // Is this session active?
    int should_stop;                // Signal to stop threads
    // Auto-reconnect state
    int needs_reconnect;            // 1 = ICE failed, schedule reconnect
    int reconnect_attempts;         // Attempt counter (for backoff)
    time_t last_reconnect_time;     // Timestamp of last reconnect attempt
    // STUN/TURN mode tracking (for STUN-upgrade probe)
    int using_turn;                 // 1 = currently connected via TURN relay, 0 = direct/STUN
    time_t turn_since;              // When TURN mode was activated (0 if not on TURN)
    time_t last_stun_check;         // Last time a STUN-upgrade probe was attempted
    // ICE gathering completion (set by on_gathering_done_camera, polled before sending SDP)
    volatile int gathering_done;    // 1 once all local candidates have been gathered
    int turn_refresh_needed;        // 1 = proactive TURN refresh (skip STUN-first, go direct to TURN)
    time_t last_pong_at;            // Last PONG heartbeat from provider
    time_t last_redirection_time;   // Last time we failed over to TURN
    int    pong_strikes;            // Consecutive PONG_TIMEOUT windows; reset on PONG
} camera_session_t;

// Accessor macro — reads from the global config at runtime so the value
// from consumer.conf (STUN_UPGRADE_INTERVAL=N) takes effect.
#define STUN_UPGRADE_INTERVAL (config.stun_upgrade_interval)

// Multi-camera consumer state
typedef struct {
    camera_session_t cameras[MAX_CAMERAS];
    int num_active_cameras;
    pthread_mutex_t global_mutex;
    pthread_mutex_t signaling_mutex; // Protects send/recv on signaling socket
    int signaling_socket;           // Persistent connection to signaling server
    volatile sig_atomic_t running;
} multi_camera_consumer_t;

// Global state
static multi_camera_consumer_t vms_state;
static config_t config;

// TURN server (shared, static so pointers remain valid for libjuice)
static juice_turn_server_t g_turn_server;

// Forward declarations for functions used by HTTP API
static camera_session_t* find_camera_session(const char* camera_id);
static camera_session_t* allocate_camera_session(const char* camera_id);
static void free_camera_session(camera_session_t* cam);
static int connect_to_camera(const char* camera_id);
static int init_signaling_connection();
static int connect_to_camera_attempt(const char* camera_id, int use_turn, int timeout_sec);

// Include HTTP API server
#include "http_api.h"

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

static char* trim_whitespace(char* str) {
    char* end;
    while (*str == ' ' || *str == '\t') str++;
    if (*str == 0) return str;
    end = str + strlen(str) - 1;
    while (end > str && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) end--;
    *(end + 1) = '\0';
    return str;
}

static int load_config(const char* config_file, config_t* cfg) {
    FILE* fp = fopen(config_file, "r");
    if (!fp) {
        printf("[X] Failed to open config file: %s\n", config_file);
        return 0;
    }
    
    // Set default values
    cfg->p2p_port_base = 9200;
    cfg->local_listen_port_base = 8000;
    strcpy(cfg->signaling_server, "127.0.0.1");
    cfg->signaling_port = 8888;
    cfg->turn_server_enabled = 0;
    cfg->turn_server_port = 3478;
    cfg->turn_server_host[0] = '\0';
    cfg->turn_server_username[0] = '\0';
    cfg->turn_server_password[0] = '\0';
    cfg->stun_upgrade_interval = 0;    // Default: disabled — forced ICE reconnects drop live streams
    cfg->turn_refresh_interval = 0;    // Default: disabled — libjuice refreshes TURN allocations internally
    cfg->api_token[0]  = '\0';         // Default: no auth required (set API_TOKEN in conf)
    cfg->cors_origin[0] = '\0';        // Default: no CORS header (set CORS_ORIGIN in conf)
    
    char line[CONFIG_LINE_MAX];
    int line_num = 0;
    
    while (fgets(line, sizeof(line), fp)) {
        line_num++;
        char* trimmed = trim_whitespace(line);
        if (strlen(trimmed) == 0 || trimmed[0] == '#') continue;
        
        char* equals = strchr(trimmed, '=');
        if (!equals) {
            printf("[!] Warning: Invalid line %d in config file\n", line_num);
            continue;
        }
        
        *equals = '\0';
        char* key = trim_whitespace(trimmed);
        char* value = trim_whitespace(equals + 1);
        
        if (strcmp(key, "P2P_PORT_BASE") == 0) {
            cfg->p2p_port_base = atoi(value);
            printf("[✓] Config: P2P_PORT_BASE = %d\n", cfg->p2p_port_base);
        } else if (strcmp(key, "LOCAL_LISTEN_PORT_BASE") == 0) {
            cfg->local_listen_port_base = atoi(value);
            printf("[✓] Config: LOCAL_LISTEN_PORT_BASE = %d\n", cfg->local_listen_port_base);
        } else if (strcmp(key, "SIGNALING_SERVER") == 0) {
            strncpy(cfg->signaling_server, value, sizeof(cfg->signaling_server) - 1);
            printf("[✓] Config: SIGNALING_SERVER = %s\n", cfg->signaling_server);
        } else if (strcmp(key, "SIGNALING_PORT") == 0) {
            cfg->signaling_port = atoi(value);
            printf("[✓] Config: SIGNALING_PORT = %d\n", cfg->signaling_port);
        } else if (strcmp(key, "TURN_SERVER_HOST") == 0) {
            strncpy(cfg->turn_server_host, value, sizeof(cfg->turn_server_host) - 1);
            cfg->turn_server_enabled = 1;
            printf("[✓] Config: TURN_SERVER_HOST = %s\n", cfg->turn_server_host);
        } else if (strcmp(key, "TURN_SERVER_PORT") == 0) {
            cfg->turn_server_port = (uint16_t)atoi(value);
            printf("[✓] Config: TURN_SERVER_PORT = %d\n", cfg->turn_server_port);
        } else if (strcmp(key, "TURN_SERVER_USERNAME") == 0) {
            strncpy(cfg->turn_server_username, value, sizeof(cfg->turn_server_username) - 1);
            printf("[✓] Config: TURN_SERVER_USERNAME = %s\n", cfg->turn_server_username);
        } else if (strcmp(key, "TURN_SERVER_PASSWORD") == 0) {
            strncpy(cfg->turn_server_password, value, sizeof(cfg->turn_server_password) - 1);
            printf("[✓] Config: TURN_SERVER_PASSWORD = (hidden)\n");
        } else if (strcmp(key, "STUN_UPGRADE_INTERVAL") == 0) {
            cfg->stun_upgrade_interval = atoi(value);
            if (cfg->stun_upgrade_interval == 0)
                printf("[✓] Config: STUN_UPGRADE_INTERVAL = 0 (STUN probe disabled)\n");
            else
                printf("[✓] Config: STUN_UPGRADE_INTERVAL = %ds\n", cfg->stun_upgrade_interval);
        } else if (strcmp(key, "TURN_REFRESH_INTERVAL") == 0) {
            cfg->turn_refresh_interval = atoi(value);
            if (cfg->turn_refresh_interval == 0)
                printf("[✓] Config: TURN_REFRESH_INTERVAL = 0 (TURN auto-refresh disabled)\n");
            else
                printf("[✓] Config: TURN_REFRESH_INTERVAL = %ds\n", cfg->turn_refresh_interval);
        } else if (strcmp(key, "API_TOKEN") == 0) {
            strncpy(cfg->api_token, value, sizeof(cfg->api_token) - 1);
            printf("[✓] Config: API_TOKEN = (set, %zu chars)\n", strlen(value));
        } else if (strcmp(key, "CORS_ORIGIN") == 0) {
            strncpy(cfg->cors_origin, value, sizeof(cfg->cors_origin) - 1);
            printf("[✓] Config: CORS_ORIGIN = %s\n", cfg->cors_origin);
        }
    }

    fclose(fp);
    return 1;
}

static int send_all(int socket_fd, const void* buf, size_t len) {
    const char* ptr = (const char*)buf;
    while (len > 0) {
        ssize_t n = send(socket_fd, ptr, len, MSG_NOSIGNAL);
        if (n <= 0) return 0;
        ptr += n;
        len -= (size_t)n;
    }
    return 1;
}

static int send_message(int socket_fd, const char* msg) {
    uint32_t msg_len_net = htonl((uint32_t)strlen(msg));
    if (!send_all(socket_fd, &msg_len_net, sizeof(msg_len_net))) return 0;
    if (!send_all(socket_fd, msg, strlen(msg))) return 0;
    return 1;
}

static int recv_message(int socket_fd, char* buffer, size_t buffer_size) {
    uint32_t msg_len;
    ssize_t received = recv(socket_fd, &msg_len, sizeof(msg_len), MSG_WAITALL);
    if (received != sizeof(msg_len)) return 0;
    msg_len = ntohl(msg_len);
    if (msg_len >= buffer_size) {
        // Drain the unread bytes so the stream stays in sync for future messages.
        char drain[4096];
        uint32_t remaining = msg_len;
        while (remaining > 0) {
            uint32_t chunk = remaining < (uint32_t)sizeof(drain) ? remaining : (uint32_t)sizeof(drain);
            ssize_t n = recv(socket_fd, drain, chunk, MSG_WAITALL);
            if (n <= 0) break;
            remaining -= (uint32_t)n;
        }
        return 0;
    }
    received = recv(socket_fd, buffer, msg_len, MSG_WAITALL);
    if (received != (ssize_t)msg_len) return 0;
    buffer[msg_len] = '\0';
    return 1;
}

static void set_nonblocking(int sock) {
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
}

// Some remote SDP candidate addresses (loopback, unspecified, multicast,
// link-local, mDNS hostnames) can trigger TURN CreatePermission 403 in coturn.
// Filter those lines before handing SDP to libjuice.
static int should_drop_candidate_line(const char* line) {
    if (strncmp(line, "a=candidate:", 12) != 0) {
        return 0;
    }

    char copy[512];
    strncpy(copy, line, sizeof(copy) - 1);
    copy[sizeof(copy) - 1] = '\0';

    char* saveptr = NULL;
    char* tok = strtok_r(copy, " ", &saveptr);
    int idx = 0;
    char ip[128] = {0};
    while (tok) {
        if (idx == 4) {
            strncpy(ip, tok, sizeof(ip) - 1);
            break;
        }
        tok = strtok_r(NULL, " ", &saveptr);
        idx++;
    }
    if (ip[0] == '\0') return 0;

    size_t ip_len = strlen(ip);
    if (ip_len >= 6 && strcmp(ip + ip_len - 6, ".local") == 0) {
        return 1;
    }

    struct in_addr addr;
    if (inet_pton(AF_INET, ip, &addr) == 1) {
        uint32_t v = ntohl(addr.s_addr);
        uint8_t a = (uint8_t)((v >> 24) & 0xFF);
        uint8_t b = (uint8_t)((v >> 16) & 0xFF);
        if (a == 0 || a == 127 || a >= 224 || a == 255 || (a == 169 && b == 254)) {
            return 1;
        }
    }

    struct in6_addr addr6;
    if (inet_pton(AF_INET6, ip, &addr6) == 1) {
        // Drop IPv6 unspecified/loopback/link-local/multicast/ULA
        if (IN6_IS_ADDR_UNSPECIFIED(&addr6) ||
            IN6_IS_ADDR_LOOPBACK(&addr6) ||
            IN6_IS_ADDR_LINKLOCAL(&addr6) ||
            IN6_IS_ADDR_MULTICAST(&addr6)) {
            return 1;
        }
        if ((addr6.s6_addr[0] & 0xFE) == 0xFC) { // fc00::/7
            return 1;
        }
    }
    return 0;
}

static int sanitize_remote_sdp(const char* input, char* output, size_t output_size) {
    size_t out_len = 0;
    int dropped = 0;
    const char* p = input;

    while (*p) {
        const char* eol = strpbrk(p, "\r\n");
        size_t line_len = eol ? (size_t)(eol - p) : strlen(p);
        int drop = 0;

        if (line_len < 512) {
            char line[512];
            memcpy(line, p, line_len);
            line[line_len] = '\0';
            drop = should_drop_candidate_line(line);
        }

        if (!drop) {
            if (out_len + line_len + 2 >= output_size) return -1;
            memcpy(output + out_len, p, line_len);
            out_len += line_len;
        } else {
            dropped++;
        }

        if (!eol) break;
        if (*eol == '\r' && *(eol + 1) == '\n') {
            if (!drop) {
                output[out_len++] = '\r';
                output[out_len++] = '\n';
            }
            p = eol + 2;
        } else {
            if (!drop) output[out_len++] = *eol;
            p = eol + 1;
        }
    }

    if (out_len >= output_size) return -1;
    output[out_len] = '\0';
    return dropped;
}

// ============================================================================
// CAMERA SESSION MANAGEMENT
// ============================================================================

static camera_session_t* find_camera_session(const char* camera_id) {
    pthread_mutex_lock(&vms_state.global_mutex);
    for (int i = 0; i < MAX_CAMERAS; i++) {
        if (vms_state.cameras[i].active && 
            strcmp(vms_state.cameras[i].camera_id, camera_id) == 0) {
            pthread_mutex_unlock(&vms_state.global_mutex);
            return &vms_state.cameras[i];
        }
    }
    pthread_mutex_unlock(&vms_state.global_mutex);
    return NULL;
}

static camera_session_t* allocate_camera_session(const char* camera_id) {
    pthread_mutex_lock(&vms_state.global_mutex);
    for (int i = 0; i < MAX_CAMERAS; i++) {
        if (!vms_state.cameras[i].active) {
            camera_session_t* cam = &vms_state.cameras[i];
            memset(cam, 0, sizeof(camera_session_t));
            snprintf(cam->camera_id, sizeof(cam->camera_id), "%s", camera_id);
            cam->active = 1;
            cam->keepalive_enabled = 1;
            cam->next_conn_id = 1;
            cam->global_seq_num = 0;
            cam->listen_socket = -1;
            cam->should_stop = 0;
            strcpy(cam->service_protocol, "rtsp");
            strcpy(cam->service_codec, "h264");
            pthread_mutex_init(&cam->mutex, NULL);
            pthread_mutex_init(&cam->dispatch_respawn_lock, NULL);
            pthread_mutex_init(&cam->dispatch_queue.mutex, NULL);
            pthread_cond_init(&cam->dispatch_queue.cond, NULL);
            cam->dispatch_queue.head = 0;
            cam->dispatch_queue.tail = 0;
            cam->dispatch_queue.bytes_queued  = 0;
            cam->dispatch_queue.drop_count    = 0;
            cam->dispatch_queue.drop_bytes    = 0;
            cam->dispatch_queue.last_drop_log = 0;
            cam->dispatch_running             = 0;
            vms_state.num_active_cameras++;
            cam->last_pong_at = time(NULL);
            cam->last_redirection_time = 0;
            pthread_mutex_unlock(&vms_state.global_mutex);
            printf("[✓] Allocated session for camera: %s\n", camera_id);
            return cam;
        }
    }
    pthread_mutex_unlock(&vms_state.global_mutex);
    return NULL;
}

// ============================================================================
// CONNECTION MANAGEMENT (per camera)
// ============================================================================

static connection_t* find_connection_by_id(camera_session_t* cam, uint32_t conn_id) {
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (cam->connections[i].active && cam->connections[i].conn_id == conn_id) {
            return &cam->connections[i];
        }
    }
    return NULL;
}

static connection_t* create_connection(camera_session_t* cam, int local_socket) {
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (!cam->connections[i].active) {
            connection_t *c = &cam->connections[i];
            memset(c, 0, sizeof(connection_t));
            c->reassembly_buffer = malloc(BUFFER_SIZE);
            c->spare_buffer      = malloc(BUFFER_SIZE);
            c->chunk_received    = calloc(MAX_CHUNKS_PER_FRAME, 1);
            if (!c->reassembly_buffer || !c->spare_buffer || !c->chunk_received) {
                free(c->reassembly_buffer);
                free(c->spare_buffer);
                free(c->chunk_received);
                c->reassembly_buffer = NULL;
                c->spare_buffer      = NULL;
                c->chunk_received    = NULL;
                return NULL;
            }
            c->conn_id       = __sync_fetch_and_add(&cam->next_conn_id, 1);
            c->local_socket  = local_socket;
            c->active        = 1;
            c->last_activity = time(NULL);
            return c;
        }
    }
    return NULL;
}

static void close_connection(connection_t* conn) {
    if (conn->local_socket >= 0) {
        close(conn->local_socket);
        conn->local_socket = -1;
    }
    free(conn->reassembly_buffer);
    free(conn->spare_buffer);
    free(conn->chunk_received);
    conn->reassembly_buffer = NULL;
    conn->spare_buffer      = NULL;
    conn->chunk_received    = NULL;
    conn->active = 0;
}

static void cleanup_stale_connections(camera_session_t* cam) {
    time_t now = time(NULL);
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        connection_t *c = &cam->connections[i];
        if (!c->active) continue;

        /* Stuck mid-reassembly: one or more chunks lost and the frame will
           never complete.  Previously this called close_connection(), which
           killed the live TCP session with the local player (Jessibuca on
           Android) and forced a reconnect — causing a visible several-second
           stall while the player re-established the socket AND the provider
           caught up with the new conn_id.  Instead, just reset the reassembly
           state in place so the NEXT offset==0 chunk starts a fresh frame.
           The TCP session stays alive and the user sees at most a few lost
           frames, not a reconnect gap. */
        if (c->frame_in_progress &&
            now - c->last_activity > REASSEMBLY_TIMEOUT) {
            printf("[!] Camera %s: Resetting stuck reassembly conn %u "
                   "(%u/%u bytes, idle %lds) — keeping TCP session\n",
                   cam->camera_id, c->conn_id,
                   c->bytes_received, c->expected_size,
                   (long)(now - c->last_activity));
            c->bytes_received          = 0;
            c->expected_size           = 0;
            c->frame_in_progress       = 0;
            c->current_frame_num_chunks = 0;
            c->last_activity           = now;
        }
    }
}

// ============================================================================
// DATA FORWARDING (per camera)
// ============================================================================

static void* forward_local_to_p2p(void* arg) {
    connection_t* conn = (connection_t*)arg;
    camera_session_t* cam = NULL;
    
    // Find which camera this connection belongs to
    pthread_mutex_lock(&vms_state.global_mutex);
    for (int i = 0; i < MAX_CAMERAS; i++) {
        if (vms_state.cameras[i].active) {
            for (int j = 0; j < MAX_CONNECTIONS; j++) {
                if (&vms_state.cameras[i].connections[j] == conn) {
                    cam = &vms_state.cameras[i];
                    break;
                }
            }
            if (cam) break;
        }
    }
    pthread_mutex_unlock(&vms_state.global_mutex);
    
    if (!cam) return NULL;

    uint8_t *buffer = malloc(BUFFER_SIZE);
    uint8_t  packet[MAX_UDP_PAYLOAD];
    if (!buffer) return NULL;

    while (cam->connected && conn->active && conn->local_socket >= 0 && !cam->should_stop) {
        ssize_t bytes_read = recv(conn->local_socket, buffer, BUFFER_SIZE, 0);
        
        if (bytes_read > 0) {
            printf("[->] Camera %s: Read %zd bytes from LOCAL -> P2P\n", cam->camera_id, bytes_read);
            size_t offset = 0;
            while (offset < (size_t)bytes_read) {
                size_t chunk_size = bytes_read - offset;
                if (chunk_size > MAX_UDP_PAYLOAD - HEADER_SIZE)
                    chunk_size = MAX_UDP_PAYLOAD - HEADER_SIZE;

                uint32_t seq         = __sync_fetch_and_add(&cam->global_seq_num, 1);
                uint32_t conn_id_net = htonl(conn->conn_id);
                uint32_t seq_net     = htonl(seq);
                uint32_t total_net   = htonl((uint32_t)bytes_read);
                uint32_t offset_net  = htonl((uint32_t)offset);

                memcpy(packet,      &conn_id_net, 4);
                memcpy(packet + 4,  &seq_net,     4);
                memcpy(packet + 8,  &total_net,   4);
                memcpy(packet + 12, &offset_net,  4);
                memcpy(packet + HEADER_SIZE, buffer + offset, chunk_size);

                /* Lock only for the juice_send call — ICE callbacks must not be blocked
                   across the entire frame; usleep() runs outside the lock */
                int ret = -1;
                pthread_mutex_lock(&cam->mutex);
                if (cam->agent && cam->connected)
                    ret = juice_send(cam->agent, (const char*)packet, chunk_size + HEADER_SIZE);
                pthread_mutex_unlock(&cam->mutex);

                if (ret < 0) break;
                offset += chunk_size;

                if (offset < (size_t)bytes_read)
                    usleep(150);
            }
        } else if (bytes_read == 0) {
            break;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            break;
        } else {
            usleep(10000);
        }
    }
    
    free(buffer);
    pthread_mutex_lock(&cam->mutex);
    close_connection(conn);
    pthread_mutex_unlock(&cam->mutex);

    return NULL;
}

// ============================================================================
// DISPATCH THREAD — sends completed frames outside the ICE callback thread
// ============================================================================

/* Forward decl so dispatch_enqueue can respawn a dead dispatch thread. */
static void* dispatch_thread_func(void* arg);

static void dispatch_enqueue(camera_session_t *cam, int fd, uint8_t *data, size_t size) {
    dispatch_queue_t *q = &cam->dispatch_queue;

    /* WATCHDOG: if the dispatch thread has exited (e.g. brief cam->active==0 race
       during a reconnect), respawn it before enqueueing.  Without this, a dead
       dispatch thread would silently accumulate frames until the queue's byte
       cap is hit, at which point every subsequent frame is dropped — manifesting
       to the user as "stream froze and never recovered". */
    if (!cam->dispatch_running && !cam->should_stop) {
        pthread_mutex_lock(&cam->dispatch_respawn_lock);
        if (!cam->dispatch_running && !cam->should_stop) {
            printf("[!] Camera %s: dispatch thread is dead — respawning\n", cam->camera_id);
            cam->dispatch_running = 1;   /* set BEFORE create, cleared by thread on exit */
            if (pthread_create(&cam->dispatch_thread, NULL, dispatch_thread_func, cam) == 0) {
                pthread_detach(cam->dispatch_thread);
            } else {
                cam->dispatch_running = 0;
                printf("[X] Camera %s: dispatch thread respawn FAILED: %s\n",
                       cam->camera_id, strerror(errno));
            }
        }
        pthread_mutex_unlock(&cam->dispatch_respawn_lock);
    }

    /* Collect any items displaced by the drop policy so we can release them
       AFTER unlocking — never run close()/free() inside the queue mutex. */
    int      dropped_fds[DISPATCH_QUEUE_SIZE];
    uint8_t *dropped_datas[DISPATCH_QUEUE_SIZE];
    int      dropped_count = 0;

    pthread_mutex_lock(&q->mutex);

    /* Enforce BOTH the slot cap and the byte cap.  Drop OLDEST until the new
       item fits.  Live-video policy: newest frame is most valuable. */
    int next = (q->tail + 1) % DISPATCH_QUEUE_SIZE;
    while ((next == q->head) ||
           (q->bytes_queued + size > DISPATCH_QUEUE_MAX_BYTES && q->head != q->tail)) {
        pending_send_t old = q->items[q->head];
        q->head = (q->head + 1) % DISPATCH_QUEUE_SIZE;
        q->bytes_queued  = (q->bytes_queued >= old.size) ? q->bytes_queued - old.size : 0;
        dropped_fds[dropped_count]   = old.fd;
        dropped_datas[dropped_count] = old.data;
        dropped_count++;
        q->drop_count++;
        q->drop_bytes += old.size;
        next = (q->tail + 1) % DISPATCH_QUEUE_SIZE;
        if (dropped_count >= DISPATCH_QUEUE_SIZE) break;  /* safety */
    }

    q->items[q->tail].fd   = fd;
    q->items[q->tail].data = data;
    q->items[q->tail].size = size;
    q->tail = next;
    q->bytes_queued += size;

    /* Rate-limited drop warning (1 Hz) so logs don't explode under sustained drop. */
    time_t now = time(NULL);
    uint64_t rep_count = 0, rep_bytes = 0;
    if (q->drop_count > 0 && now - q->last_drop_log >= 1) {
        rep_count = q->drop_count;
        rep_bytes = q->drop_bytes;
        q->drop_count = 0;
        q->drop_bytes = 0;
        q->last_drop_log = now;
    }

    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);

    /* Dropping a frame from the queue means its bytes will NEVER reach the
     * client.  But in our protocol each queued item is a contiguous slice
     * of the camera's FLV byte stream, so any frame we skip creates a
     * gap that the FLV parser on the client cannot recover from — the
     * next frame we send will be interpreted as a continuation of the
     * dropped one's position, desyncing the parser permanently.
     *
     * Therefore: whenever we drop ANY frame, we immediately half-close
     * the underlying TCP socket (via shutdown on one of the dup'd fds —
     * shutdown affects the file description, not the descriptor, so it
     * propagates to every dup pointing at the same socket).  The client
     * sees a clean EOF, tears down its FLV parser, and reconnects with a
     * fresh FLV header — latency ~100 ms on 127.0.0.1.  Far better than
     * continuing to deliver corrupted bytes that the parser can never
     * resync from.  Any dispatch-queue items still queued for the old
     * conn_id will fail to send with EPIPE and be silently discarded. */
    if (dropped_count > 0) {
        for (int i = 0; i < dropped_count; i++) {
            if (dropped_fds[i] >= 0) {
                shutdown(dropped_fds[i], SHUT_WR);
                close(dropped_fds[i]);
            }
            free(dropped_datas[i]);
        }
    }

    if (rep_count > 0) {
        printf("[!] Camera %s: dispatch queue dropped %llu frame(s) (%llu bytes) — forced reconnect\n",
               cam->camera_id, (unsigned long long)rep_count, (unsigned long long)rep_bytes);
    }
}

static void* dispatch_thread_func(void* arg) {
    camera_session_t *cam = (camera_session_t*)arg;
    dispatch_queue_t *q   = &cam->dispatch_queue;

    cam->dispatch_running = 1;

    /* The thread survives transient cam->active flips during reconnect.
       It only exits when should_stop is set (i.e. the camera session is
       being freed).  This prevents the silent-death watchdog pattern
       where a dispatch thread exits during a reconnect and subsequent
       frames pile up in a dead queue. */
    while (!cam->should_stop) {
        pthread_mutex_lock(&q->mutex);
        while (q->head == q->tail && !cam->should_stop)
            pthread_cond_wait(&q->cond, &q->mutex);

        if (q->head == q->tail) {
            pthread_mutex_unlock(&q->mutex);
            break;   /* only reached when should_stop is set and queue is empty */
        }
        pending_send_t item = q->items[q->head];
        q->head = (q->head + 1) % DISPATCH_QUEUE_SIZE;
        q->bytes_queued = (q->bytes_queued >= item.size) ? q->bytes_queued - item.size : 0;
        pthread_mutex_unlock(&q->mutex);

        /* Send the completed frame to the local player.
         *
         * CRITICAL CORRECTNESS CONSTRAINT: FLV-over-TCP is a continuous byte
         * stream — an FLV parser on the client depends on every byte being
         * delivered in order.  A partial send (frame half-delivered, then
         * the next frame appended on the same socket) CORRUPTS the byte
         * stream and the FLV parser fails.  That failure is what the user
         * perceives as "stream stuck / proxy stuck".  The earlier 300 ms
         * stall_budget was aggressive enough that any JS/WASM garbage-
         * collection pause in Jessibuca — routinely 200–500 ms — would
         * trigger a half-sent frame.
         *
         * New policy:
         *   - Track PROGRESS.  Any non-zero send() resets the idle timer.
         *   - Tolerate long transient pauses (up to 10 s of no progress)
         *     — a GC pause / surface swap / brief screen-off is benign.
         *   - If the client makes NO progress for 10 s, assume it is truly
         *     stuck.  Call shutdown(SHUT_WR) to cleanly half-close the
         *     underlying TCP socket (shutdown affects the file description,
         *     so all dup() siblings see it).  The client reads EOF and
         *     reconnects with a fresh FLV header — far better than writing
         *     another frame after a partial one and handing the client a
         *     mangled byte stream it can never recover from. */
        if (item.fd >= 0 && item.data && item.size > 0) {
            size_t sent = 0;
            /* Millisecond-granular progress clock so a sub-second GC pause
             * doesn't masquerade as "no progress" and trigger a tear-down. */
            struct timespec tnow, tprog;
            clock_gettime(CLOCK_MONOTONIC, &tprog);
            /* 60000 ms (was 5000).  Augentix HEVC streams have ~1.4 Mbps
             * with 60 KB+ keyframes that take MediaCodec several hundred ms
             * to decode; meanwhile MSE source-buffer back-pressure can
             * pause Chromium's read of the InputStream for multiple seconds.
             * The previous 5 s tear-down was firing on ROUTINE keyframe
             * processing, not on a genuinely-dead client — surfacing as
             * "fetch response status 502" / IOException: unexpected end of
             * stream in the WebView intercept.  60 s tolerates the worst
             * decoder pauses we observe while still detecting a truly
             * stuck client within a minute.  */
            const long NO_PROGRESS_TIMEOUT_MS = 60000;
            int torn_down = 0;
            while (sent < item.size) {
                ssize_t n = send(item.fd, item.data + sent, item.size - sent, MSG_NOSIGNAL);
                if (n > 0) {
                    sent += (size_t)n;
                    clock_gettime(CLOCK_MONOTONIC, &tprog);
                    continue;
                }
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    clock_gettime(CLOCK_MONOTONIC, &tnow);
                    long elapsed_ms =
                        (tnow.tv_sec  - tprog.tv_sec)  * 1000L +
                        (tnow.tv_nsec - tprog.tv_nsec) / 1000000L;
                    if (elapsed_ms > NO_PROGRESS_TIMEOUT_MS) {
                        printf("[!] Camera %s: client stuck %ldms — shutdown(SHUT_WR) to force reconnect\n",
                               cam->camera_id, elapsed_ms);
                        shutdown(item.fd, SHUT_WR);
                        torn_down = 1;
                        break;
                    }
                    /* Poll with a short tail so the remaining budget is used
                     * efficiently.  Cap at 250 ms so we check the progress
                     * clock often. */
                    long remaining_ms = NO_PROGRESS_TIMEOUT_MS - elapsed_ms;
                    int poll_ms = (remaining_ms > 250) ? 250 : (int)remaining_ms;
                    if (poll_ms < 10) poll_ms = 10;
                    struct pollfd pfd = { .fd = item.fd, .events = POLLOUT, .revents = 0 };
                    int pr = poll(&pfd, 1, poll_ms);
                    if (pr > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
                        torn_down = 1;
                        break;
                    }
                    continue;
                }
                if (n < 0 && errno == EINTR) continue;
                /* Hard error (EPIPE / ECONNRESET / EBADF): socket already
                 * dead — nothing to tear down, just stop sending. */
                torn_down = 1;
                break;
            }
            (void)torn_down;
        }
        close(item.fd);
        free(item.data);
    }

    // Drain remaining items on exit
    pthread_mutex_lock(&q->mutex);
    while (q->head != q->tail) {
        pending_send_t item = q->items[q->head];
        q->head = (q->head + 1) % DISPATCH_QUEUE_SIZE;
        q->bytes_queued = (q->bytes_queued >= item.size) ? q->bytes_queued - item.size : 0;
        close(item.fd);
        free(item.data);
    }
    pthread_mutex_unlock(&q->mutex);

    cam->dispatch_running = 0;
    printf("[i] Camera %s: dispatch thread exit (should_stop=%d, active=%d)\n",
           cam->camera_id, cam->should_stop, cam->active);
    return NULL;
}

// ============================================================================
// ICE CALLBACKS (per camera)
// ============================================================================

static void on_recv_camera(juice_agent_t *agent, const char *data, size_t size, void *user_ptr) {
    (void)agent;
    camera_session_t* cam = (camera_session_t*)user_ptr;
    if (!cam || !cam->active) return;
    if (size < HEADER_SIZE) return;
    
    uint32_t conn_id    = ntohl(*(uint32_t*)data);
    uint32_t seq_num    = ntohl(*(uint32_t*)(data + 4));
    uint32_t total_size = ntohl(*(uint32_t*)(data + 8));
    uint32_t offset     = ntohl(*(uint32_t*)(data + 12));
    size_t chunk_size   = size - HEADER_SIZE;

    // Handle P2P heartbeat (conn_id == 0)
    if (conn_id == 0 && total_size == 0 && offset == 0) {
        if (seq_num == 2) { // PONG
            pthread_mutex_lock(&cam->mutex);
            cam->last_pong_at = time(NULL);
            cam->pong_strikes = 0;
            pthread_mutex_unlock(&cam->mutex);
        }
        return;
    }

    pthread_mutex_lock(&cam->mutex);

    connection_t* conn = find_connection_by_id(cam, conn_id);
    if (!conn) {
        pthread_mutex_unlock(&cam->mutex);
        return;
    }

    conn->last_activity = time(NULL);
    conn->last_seq_num  = seq_num;

    // New frame: reset state when first chunk (offset==0) arrives.
    // We also capture seq_num as the "frame-start seq" so later chunks can be
    // verified to belong to THIS frame — UDP can reorder, and without this
    // guard a late chunk from frame N-1 whose offset/size happens to fit the
    // new frame's layout would be written into the new frame's buffer, silently
    // corrupting it.
    if (offset == 0) {
        if (total_size == 0 || total_size > BUFFER_SIZE) {
            pthread_mutex_unlock(&cam->mutex);
            if (total_size > BUFFER_SIZE)
                printf("[!] Camera %s: Oversized frame dropped (size=%u > buffer=%u) — increase BUFFER_SIZE if legitimate\n",
                       cam->camera_id, total_size, (uint32_t)BUFFER_SIZE);
            return;
        }
        memset(conn->chunk_received, 0, MAX_CHUNKS_PER_FRAME);
        conn->expected_size           = total_size;
        conn->bytes_received          = 0;
        conn->current_frame_start_seq = seq_num;
        conn->current_frame_num_chunks =
            (total_size + MAX_CHUNK_PAYLOAD - 1) / MAX_CHUNK_PAYLOAD;
        conn->frame_in_progress       = 1;
    }

    // Must have seen offset=0 first before accepting later chunks
    if (!conn->frame_in_progress || conn->expected_size == 0 || !conn->reassembly_buffer) {
        pthread_mutex_unlock(&cam->mutex);
        return;
    }

    /* Frame-ID guard: this chunk's seq_num must fall within the window of
       chunks allocated to the currently-reassembling frame.  Uses subtraction
       under uint32_t so it wraps safely at 2^32.  If the chunk came from a
       previous frame that arrived late, seq_offset will be large (near 2^32)
       and we reject it cleanly instead of corrupting the new frame. */
    uint32_t seq_offset = seq_num - conn->current_frame_start_seq;
    if (seq_offset >= conn->current_frame_num_chunks) {
        pthread_mutex_unlock(&cam->mutex);
        return;
    }

    uint32_t chunk_index = offset / MAX_CHUNK_PAYLOAD;
    uint8_t  *frame      = NULL;
    int       fd         = -1;
    size_t    frame_size = 0;
    int       spare_consumed = 0;

    /* Hardened bounds check (MEM-3): reject malformed / crafted chunks before
     * any memcpy.  All arithmetic is done in 64-bit to avoid uint32 wraparound
     * when a peer sends a huge offset value. */
    uint64_t end64 = (uint64_t)offset + (uint64_t)chunk_size;
    if (total_size == conn->expected_size &&
        chunk_index < MAX_CHUNKS_PER_FRAME &&
        offset < total_size &&
        end64 <= (uint64_t)total_size &&
        end64 <= (uint64_t)BUFFER_SIZE &&
        !conn->chunk_received[chunk_index]) {

        memcpy(conn->reassembly_buffer + offset, data + HEADER_SIZE, chunk_size);
        conn->chunk_received[chunk_index] = 1;
        conn->bytes_received += (uint32_t)chunk_size;

        if (conn->bytes_received >= conn->expected_size) {
            /* Zero-copy handoff: swap reassembly_buffer with the pre-allocated
               spare. The completed frame is passed to the dispatch thread
               as-is, avoiding the 4 MB memcpy and 4 MB malloc that would
               otherwise run while holding cam->mutex. Those two operations
               were the dominant cause of frame drops under load — during the
               ~1–2 ms they took, the single libjuice ICE thread could not
               deliver any more chunks for this camera, so the kernel UDP
               queue would overflow on busy keyframes. */
            if (conn->spare_buffer && conn->local_socket >= 0) {
                frame      = conn->reassembly_buffer;
                frame_size = conn->expected_size;
                conn->reassembly_buffer = conn->spare_buffer;
                conn->spare_buffer = NULL;   /* replenished below, outside lock */
                fd = dup(conn->local_socket);
                spare_consumed = 1;
            }
            conn->bytes_received    = 0;
            conn->expected_size     = 0;
            conn->frame_in_progress = 0;   /* ready for next offset==0 chunk */
        }
    }

    pthread_mutex_unlock(&cam->mutex);

    /* Replenish the consumed spare buffer OUTSIDE the lock. Done here (not in
       the dispatch thread) so the next keyframe never stalls on a malloc. */
    if (spare_consumed) {
        uint8_t *new_spare = malloc(BUFFER_SIZE);
        if (new_spare) {
            pthread_mutex_lock(&cam->mutex);
            connection_t *c2 = find_connection_by_id(cam, conn_id);
            if (c2 && c2->active && c2->spare_buffer == NULL) {
                c2->spare_buffer = new_spare;
                new_spare = NULL;
            }
            pthread_mutex_unlock(&cam->mutex);
            free(new_spare);   /* no-op if attached */
        }
    }

    if (frame && fd >= 0 && frame_size > 0) {
        printf("[<-] Camera %s: frame %zu bytes P2P->LOCAL\n", cam->camera_id, frame_size);
        // Hand off to dispatch thread — never block the ICE callback
        dispatch_enqueue(cam, fd, frame, frame_size);
    } else {
        if (fd >= 0) close(fd);
        free(frame);
    }
}

static void on_candidate_camera(juice_agent_t *agent, const char *sdp, void *user_ptr) {
    (void)agent;
    camera_session_t* cam = (camera_session_t*)user_ptr;
    if (!cam) return;

    // Detect candidate type from SDP line
    if (strstr(sdp, " host ")) {
        printf("[ICE] Camera %s: Candidate found -> HOST (direct LAN)\n", cam->camera_id);
    } else if (strstr(sdp, " srflx ")) {
        printf("[ICE] Camera %s: Candidate found -> SRFLX (STUN - public IP discovered)\n", cam->camera_id);
    } else if (strstr(sdp, " relay ")) {
        printf("[ICE] Camera %s: Candidate found -> RELAY (TURN - traffic will be relayed)\n", cam->camera_id);
    } else if (strstr(sdp, " prflx ")) {
        printf("[ICE] Camera %s: Candidate found -> PRFLX (peer reflexive)\n", cam->camera_id);
    } else {
        printf("[ICE] Camera %s: Candidate found -> %s\n", cam->camera_id, sdp);
    }
}

static void on_gathering_done_camera(juice_agent_t *agent, void *user_ptr) {
    (void)agent;
    camera_session_t* cam = (camera_session_t*)user_ptr;
    if (!cam) return;
    printf("[ICE] Camera %s: ===== All candidates gathered =====\n", cam->camera_id);
    // Signal the gathering-wait loop in connect_to_camera_attempt /
    // reconnect_camera_ice_attempt that the local SDP is now complete
    // (contains HOST + SRFLX + RELAY candidates).
    cam->gathering_done = 1;
}

static void on_state_changed_camera(juice_agent_t *agent, juice_state_t state_val, void *user_ptr) {
    camera_session_t* cam = (camera_session_t*)user_ptr;
    if (!cam) return;

    const char *state_names[] = {"DISCONNECTED","GATHERING","CONNECTING","CONNECTED","COMPLETED","FAILED"};
    const char *sn = (state_val >= 0 && state_val <= 5) ? state_names[state_val] : "UNKNOWN";
    printf("[ICE] Camera %s: State -> %s\n", cam->camera_id, sn);

    if (state_val == JUICE_STATE_CONNECTED || state_val == JUICE_STATE_COMPLETED) {
        // Determine connection method from selected candidate pair BEFORE locking
        // (juice_get_selected_candidates may internally lock; don't nest with cam->mutex)
        char local_cand[256] = {0};
        char remote_cand[256] = {0};
        juice_get_selected_candidates(agent, local_cand, sizeof(local_cand), remote_cand, sizeof(remote_cand));

        // Use strstr without requiring a trailing space so "typ prflx" at
        // end-of-string (no raddr/rport) is still detected correctly.
        int has_relay = (strstr(local_cand, " relay") || strstr(remote_cand, " relay")) ? 1 : 0;
        int has_srflx = (strstr(local_cand, " srflx") || strstr(remote_cand, " srflx")) ? 1 : 0;
        int has_prflx = (strstr(local_cand, " prflx") || strstr(remote_cand, " prflx")) ? 1 : 0;
        int has_host  = (strstr(local_cand, " host")  || strstr(remote_cand, " host"))  ? 1 : 0;
        int is_turn   = has_relay;

        const char *method = "UNKNOWN";
        const char *detail = "";
        if (has_relay) {
            method = "TURN RELAY";
            detail = " (traffic relayed through Coturn server)";
        } else if (has_host || has_prflx) {
            // host or peer-reflexive on either side = direct LAN path
            // prflx+host means same-subnet, discovered during connectivity checks
            method = "HOST (Direct LAN)";
            detail = " (direct connection, no STUN/TURN needed — best possible)";
        } else if (has_srflx) {
            method = "STUN (Server Reflexive)";
            detail = " (direct P2P across internet, Coturn only discovered public IPs)";
        }

        pthread_mutex_lock(&cam->mutex);
        if (!cam->connected) {
            cam->connected = 1;
            cam->connection_start_time = time(NULL);
            cam->needs_reconnect = 0;
            cam->reconnect_attempts = 0;  // Reset backoff on successful connection
            cam->last_pong_at = time(NULL);  // Avoid spurious PONG_TIMEOUT before first PONG
            cam->pong_strikes = 0;
        }
        // Track TURN vs STUN mode for STUN-upgrade probe logic
        if (is_turn) {
            if (!cam->using_turn) {
                cam->using_turn = 1;
                cam->turn_since = time(NULL);
                cam->last_stun_check = time(NULL);
                printf("[!] Camera %s: Connected via TURN relay — will probe STUN every %ds\n",
                       cam->camera_id, STUN_UPGRADE_INTERVAL);
            }
        } else {
            if (cam->using_turn) {
                printf("[✓] Camera %s: Upgraded from TURN to direct/STUN path\n", cam->camera_id);
            }
            cam->using_turn = 0;
            cam->turn_since = 0;
        }
        pthread_mutex_unlock(&cam->mutex);

        printf("╔══════════════════════════════════════════════════════════╗\n");
        printf("║  Camera %s: P2P %s                                     \n", cam->camera_id, sn);
        printf("║  Connection Method: %s%s\n", method, detail);
        printf("║  Local  candidate: %s\n", local_cand[0] ? local_cand : "N/A");
        printf("║  Remote candidate: %s\n", remote_cand[0] ? remote_cand : "N/A");
        printf("║  Listen port: %d\n", cam->local_listen_port);
        printf("╚══════════════════════════════════════════════════════════╝\n");
    } else if (state_val == JUICE_STATE_FAILED) {
        printf("[X] Camera %s: P2P connection failed — scheduling reconnect\n", cam->camera_id);
        pthread_mutex_lock(&cam->mutex);
        cam->connected = 0;
        cam->needs_reconnect = 1;
        // Do NOT set last_reconnect_time = 0.  The backoff schedule uses
        // last_reconnect_time (set by reconnect_camera_ice) to space attempts.
        // Zeroing it defeats the backoff entirely, causing rapid-fire REQUESTs
        // when ICE keeps failing quickly — each REQUEST interrupts the previous
        // ICE negotiation at the provider, creating an infinite timeout loop.
        //
        // First reconnect after a long-lived connection still fires immediately
        // because elapsed = time(NULL) - last_reconnect_time is already large.
        cam->using_turn = 0;  // Reset; reconnect will probe STUN first
        pthread_mutex_unlock(&cam->mutex);
    } else if (state_val == JUICE_STATE_DISCONNECTED) {
        pthread_mutex_lock(&cam->mutex);
        cam->connected = 0;
        if (cam->agent != NULL) {
            // Real unexpected disconnect (network drop, remote end left, etc.)
            // cam->agent is still set → this is NOT our own juice_destroy() cleanup.
            printf("[!] Camera %s: P2P disconnected — scheduling reconnect\n", cam->camera_id);
            cam->needs_reconnect = 1;
            // Do NOT set last_reconnect_time = 0 — same reason as FAILED handler.
        } else {
            // cam->agent == NULL: we called juice_destroy() ourselves inside
            // reconnect_camera_ice() — the reconnect is already in progress.
            // Do NOT set needs_reconnect here or the monitor will fire a second
            // reconnect that sends a duplicate REQUEST and breaks the live negotiation.
            printf("[~] Camera %s: ICE cleanup disconnect (own juice_destroy) — ignoring\n", cam->camera_id);
        }
        cam->using_turn = 0;
        pthread_mutex_unlock(&cam->mutex);
    }
}

// ============================================================================
// CAMERA THREADS
// ============================================================================

static void* accept_thread_func(void* arg) {
    camera_session_t* cam = (camera_session_t*)arg;
    
    printf("[✓] Camera %s: Accept thread started\n", cam->camera_id);
    
    while (cam->active && !cam->should_stop) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int client_socket = accept(cam->listen_socket, (struct sockaddr*)&client_addr, &client_len);
        if (client_socket < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(100000);
                continue;
            }
            break;
        }
        
        set_nonblocking(client_socket);
        int flag = 1;
        setsockopt(client_socket, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
        /* Fat kernel send buffer: a single H.265 / 4K keyframe can be up to the
           full BUFFER_SIZE (4 MB). A tiny default SO_SNDBUF forces the dispatch
           thread to split the send across many poll() waits whenever the local
           reader pauses briefly, which in turn leaves less time for the next
           frame. 4 MB matches the reassembly buffer exactly. */
        int sndbuf = 4 * 1024 * 1024;
        setsockopt(client_socket, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
        int rcvbuf = 256 * 1024;
        setsockopt(client_socket, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

        pthread_mutex_lock(&cam->mutex);
        if (cam->connected) {
            connection_t* conn = create_connection(cam, client_socket);
            if (conn) {
                printf("[✓] Camera %s: New connection %u\n", cam->camera_id, conn->conn_id);
                pthread_create(&conn->forward_thread, NULL, forward_local_to_p2p, conn);
                pthread_detach(conn->forward_thread);
            } else {
                printf("[!] Camera %s: Max connections reached\n", cam->camera_id);
                close(client_socket);
            }
        } else {
            close(client_socket);
        }
        pthread_mutex_unlock(&cam->mutex);
    }
    
    printf("[!] Camera %s: Accept thread stopped\n", cam->camera_id);
    return NULL;
}

static void* cleanup_thread_func(void* arg) {
    camera_session_t* cam = (camera_session_t*)arg;
    
    while (cam->active && !cam->should_stop) {
        sleep(10);
        pthread_mutex_lock(&cam->mutex);
        cleanup_stale_connections(cam);
        pthread_mutex_unlock(&cam->mutex);
    }
    
    return NULL;
}

// P2P Keepalive: The CLIENT (consumer) periodically sends Binding Requests
// (application-level PING over the P2P/ICE channel) to the provider.
// The provider only responds with PONG — it never initiates keepalives.
// This keeps NAT mappings alive (RFC 5389 Section 10) and detects dead paths.
static void* keepalive_thread_func(void* arg) {
    camera_session_t* cam = (camera_session_t*)arg;

    printf("[✓] Camera %s: Keepalive thread started (client sends PING every %ds)\n",
           cam->camera_id, KEEPALIVE_INTERVAL);

    /* Counter so we send the per-connection keepalive (every 5×KEEPALIVE_INTERVAL
     * = 25 s) less often than the global PING — once per ~25 s comfortably beats
     * the provider's REASSEMBLY_TIMEOUT of 30 s. */
    int per_conn_kick = 0;

    while (cam->keepalive_enabled && cam->active && !cam->should_stop) {
        sleep(KEEPALIVE_INTERVAL);

        pthread_mutex_lock(&cam->mutex);
        if (cam->agent && cam->connected) {
            // Application-level PING: conn_id=0, seq=1 (PING), total_size=0, offset=0
            // Analogous to STUN Binding Request for NAT mapping refresh
            uint8_t ping[HEADER_SIZE];
            memset(ping, 0, HEADER_SIZE);
            *(uint32_t*)(ping + 4) = htonl(1); // seq_num = 1 for PING

            int ret = juice_send(cam->agent, (const char*)ping, HEADER_SIZE);
            if (ret >= 0) {
                // printf("[♥] Camera %s: PING sent (uptime: %ld sec)\n", cam->camera_id, (long)(time(NULL) - cam->connection_start_time));
            }

            /* PER-CONNECTION keepalive — fires every ~25 s.
             *
             * Without this, the PROVIDER closes any active connection whose
             * `last_activity` hasn't been touched in REASSEMBLY_TIMEOUT (30 s).
             * For HTTP-FLV streams the consumer sends the GET once and then
             * ONLY listens — no consumer→provider data ever flows on that
             * conn_id again, so its `last_activity` stays at the time of the
             * GET and the provider force-closes it at +30 s.  That's the
             * "33 second freeze" the field tested with two cameras of two
             * different vendors (Sigmastar S-series, Augentix).
             *
             * Fix: send a 0-payload chunk on each active conn_id every 25 s.
             * The provider's on_recv path updates `last_activity` at line 826
             * BEFORE the offset==0/total_size==0 silent-drop branch (line 833),
             * so this packet is a no-op forward-wise but a real liveness ping
             * for the provider's main-loop reaper.                            */
            if (++per_conn_kick >= 5) {
                per_conn_kick = 0;
                for (int i = 0; i < MAX_CONNECTIONS; i++) {
                    connection_t *c = &cam->connections[i];
                    if (!c->active || c->conn_id == 0) continue;
                    uint8_t ka[HEADER_SIZE];
                    uint32_t conn_id_net = htonl(c->conn_id);
                    uint32_t seq_net     = htonl(0); // seq irrelevant for size=0
                    uint32_t total_net   = htonl(0); // total_size=0 → provider silently drops payload
                    uint32_t offset_net  = htonl(0);
                    memcpy(ka,      &conn_id_net,  4);
                    memcpy(ka + 4,  &seq_net,      4);
                    memcpy(ka + 8,  &total_net,    4);
                    memcpy(ka + 12, &offset_net,   4);
                    juice_send(cam->agent, (const char*)ka, HEADER_SIZE);
                }
            }
        }
        pthread_mutex_unlock(&cam->mutex);
    }

    printf("[!] Camera %s: Keepalive thread stopped\n", cam->camera_id);
    return NULL;
}

// ============================================================================
// FREE CAMERA SESSION
// ============================================================================

static void free_camera_session(camera_session_t* cam) {
    if (!cam || !cam->active) return;
    
    printf("[!] Freeing session for camera: %s\n", cam->camera_id);
    
    // Signal threads to stop
    cam->should_stop = 1;
    cam->keepalive_enabled = 0;

    // Wake dispatch thread so it can observe should_stop and exit
    pthread_mutex_lock(&cam->dispatch_queue.mutex);
    pthread_cond_signal(&cam->dispatch_queue.cond);
    pthread_mutex_unlock(&cam->dispatch_queue.mutex);

    // Give threads time to exit
    sleep(1);

    // Nullify the agent pointer under cam->mutex FIRST so that detached
    // threads (keepalive, forward) see NULL before we call juice_destroy().
    // This prevents use-after-free when a thread checks cam->agent between
    // our NULL assignment and juice_destroy() completing.
    juice_agent_t *agent_to_destroy = NULL;
    pthread_mutex_lock(&cam->mutex);
    agent_to_destroy = cam->agent;
    cam->agent = NULL;
    cam->connected = 0;
    pthread_mutex_unlock(&cam->mutex);

    if (agent_to_destroy) {
        juice_destroy(agent_to_destroy);
    }

    pthread_mutex_lock(&vms_state.global_mutex);

    // Close all connections and free dynamic buffers
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (cam->connections[i].active) {
            close_connection(&cam->connections[i]);
        }
    }

    // Close listen socket
    if (cam->listen_socket >= 0) {
        close(cam->listen_socket);
        cam->listen_socket = -1;
    }

    // Mark as inactive
    cam->active = 0;
    vms_state.num_active_cameras--;

    pthread_mutex_unlock(&vms_state.global_mutex);
    
    printf("[✓] Camera %s: Session freed\n", cam->camera_id);
}

// ============================================================================
// AUTO-RECONNECT
// ============================================================================

static int reconnect_camera_ice_attempt(camera_session_t* cam, int use_turn) {
    // Create a fresh ICE agent
    juice_config_t juice_config;
    memset(&juice_config, 0, sizeof(juice_config));
    juice_config.cb_state_changed = on_state_changed_camera;
    juice_config.cb_recv = on_recv_camera;
    juice_config.cb_candidate = on_candidate_camera;
    juice_config.cb_gathering_done = on_gathering_done_camera;
    juice_config.user_ptr = cam;
    juice_config.bind_address = "0.0.0.0";

    if (config.turn_server_enabled && config.turn_server_host[0] != '\0') {
        juice_config.stun_server_host = config.turn_server_host;
        juice_config.stun_server_port = config.turn_server_port;
        if (use_turn) {
            memset(&g_turn_server, 0, sizeof(g_turn_server));
            g_turn_server.host = config.turn_server_host;
            g_turn_server.port = config.turn_server_port;
            if (config.turn_server_username[0] != '\0')
                g_turn_server.username = config.turn_server_username;
            if (config.turn_server_password[0] != '\0')
                g_turn_server.password = config.turn_server_password;
            juice_config.turn_servers = &g_turn_server;
            juice_config.turn_servers_count = 1;
            printf("[~] Camera %s: Reconnect attempt mode TURN fallback enabled\n", cam->camera_id);
        } else {
            printf("[~] Camera %s: Reconnect attempt mode STUN-only\n", cam->camera_id);
        }
    } else {
        juice_config.stun_server_host = "74.125.250.129";
        juice_config.stun_server_port = 19302;
        printf("[~] Camera %s: Reconnect using Google STUN only (no coturn configured)\n", cam->camera_id);
    }

    cam->agent = juice_create(&juice_config);
    if (!cam->agent) {
        printf("[X] Camera %s: Failed to create ICE agent for reconnect\n", cam->camera_id);
        return -1;
    }

    // Gather local candidates — wait for on_gathering_done_camera so the SDP
    // sent to the signaling server contains ALL candidates (HOST+SRFLX+RELAY).
    char local_sdp[MAX_SDP_LEN];
    cam->gathering_done = 0;
    juice_get_local_description(cam->agent, local_sdp, MAX_SDP_LEN);
    juice_gather_candidates(cam->agent);
    /* 50 ms × 60 = 3 s.  TURN candidate gathering completes well under 1 s
     * on healthy paths; the previous 15 s cap turned every reconnect into a
     * mandatory long freeze.  ICE will continue gathering after we send the
     * SDP — late candidates are still usable for trickle ICE upgrades.  */
    for (int gi = 0; gi < 60 && !cam->gathering_done; gi++) {
        usleep(50000);
    }
    if (!cam->gathering_done) {
        printf("[!] Camera %s: Reconnect ICE gathering still pending at 3 s — proceeding with current candidates\n",
               cam->camera_id);
    }
    juice_get_local_description(cam->agent, local_sdp, MAX_SDP_LEN);

    // Re-negotiate with the signaling server
    char request_msg[MAX_SDP_LEN + 512];
    snprintf(request_msg, sizeof(request_msg),
             "type=REQUEST\nservice_id=%s\npeer_id=consumer_reconnect_%u\nconsumer_sdp=%s",
             cam->camera_id, (unsigned int)time(NULL) % 1000000, local_sdp);

    char response[MAX_SDP_LEN + 512];
    int got_response = 0;
    for (int attempt = 0; attempt < 2 && !got_response; attempt++) {
        pthread_mutex_lock(&vms_state.signaling_mutex);
        int sent_ok = send_message(vms_state.signaling_socket, request_msg);
        int recv_ok = sent_ok ? recv_message(vms_state.signaling_socket, response, sizeof(response)) : 0;
        pthread_mutex_unlock(&vms_state.signaling_mutex);

        if (sent_ok && recv_ok) {
            got_response = 1;
            break;
        }

        // IMPORTANT: Only retry when send FAILED (REQUEST was not delivered).
        // If send succeeded but recv failed, the server already received our REQUEST
        // and forwarded CONSUMER_CONNECT to the provider.  Retrying would send a
        // second REQUEST → second CONSUMER_CONNECT → provider destroys the first
        // live ICE negotiation → timeout.
        if (!sent_ok && attempt == 0) {
            printf("[!] Camera %s: Send failed; reconnecting signaling socket and retrying...\n",
                   cam->camera_id);
            pthread_mutex_lock(&vms_state.signaling_mutex);
            if (vms_state.signaling_socket >= 0) close(vms_state.signaling_socket);
            vms_state.signaling_socket = -1;
            pthread_mutex_unlock(&vms_state.signaling_mutex);

            if (!init_signaling_connection()) {
                printf("[X] Camera %s: Failed to reconnect signaling socket\n", cam->camera_id);
                break;
            }
        } else {
            // sent_ok=1, recv_ok=0: REQUEST delivered but response lost.
            // Do NOT retry — the REQUEST is already in flight.
            if (attempt == 0) {
                printf("[!] Camera %s: REQUEST sent but response lost — not retrying to avoid duplicate CONSUMER_CONNECT\n",
                       cam->camera_id);
            }
            break;
        }
    }

    // Helper macro: clear cam->agent BEFORE juice_destroy so that the
    // on_state_changed DISCONNECTED callback (fired by juice_destroy) sees
    // cam->agent==NULL and skips re-scheduling a reconnect.
#define DESTROY_AGENT_AND_RETURN_MINUS1(cam_ptr) do { \
    juice_agent_t *_a = (cam_ptr)->agent;              \
    (cam_ptr)->agent = NULL;                           \
    if (_a) juice_destroy(_a);                         \
    return -1;                                         \
} while (0)

    if (!got_response) {
        printf("[X] Camera %s: No response from signaling server during reconnect\n", cam->camera_id);
        DESTROY_AGENT_AND_RETURN_MINUS1(cam);
    }

    if (strstr(response, "type=PROVIDER_INFO") == NULL) {
        printf("[X] Camera %s: Provider not available yet — will retry\n", cam->camera_id);
        DESTROY_AGENT_AND_RETURN_MINUS1(cam);
    }

    char* sdp_start = strstr(response, "provider_sdp=");
    if (!sdp_start) {
        printf("[X] Camera %s: No provider SDP in reconnect response\n", cam->camera_id);
        DESTROY_AGENT_AND_RETURN_MINUS1(cam);
    }
    sdp_start += strlen("provider_sdp=");

    char sanitized_sdp[MAX_SDP_LEN];
    int dropped = sanitize_remote_sdp(sdp_start, sanitized_sdp, sizeof(sanitized_sdp));
    if (dropped < 0) {
        printf("[X] Camera %s: Remote SDP too large after sanitization\n", cam->camera_id);
        DESTROY_AGENT_AND_RETURN_MINUS1(cam);
    }
    if (dropped > 0) {
        printf("[!] Camera %s: Dropped %d unsupported ICE candidate(s) from remote SDP\n",
               cam->camera_id, dropped);
    }

    if (juice_set_remote_description(cam->agent, sanitized_sdp) < 0) {
        printf("[X] Camera %s: Failed to set remote SDP during reconnect\n", cam->camera_id);
        DESTROY_AGENT_AND_RETURN_MINUS1(cam);
    }
    // Remote SDP comes from the signaling server in one shot — no trickle ICE.
    juice_set_remote_gathering_done(cam->agent);

    printf("[✓] Camera %s: ICE reconnect in progress (listen port %d preserved, mode=%s)\n",
           cam->camera_id, cam->local_listen_port, use_turn ? "TURN" : "STUN");
    return 0;
}

// Re-run ICE negotiation for an existing session without touching the listen socket.
// Called from the reconnect monitor thread — the port stays the same so VMS clients
// are unaffected by the dropout.
static int reconnect_camera_ice(camera_session_t* cam) {
    printf("[~] Camera %s: ICE reconnect attempt %d...\n",
           cam->camera_id, cam->reconnect_attempts + 1);

    // Atomically grab and clear the old agent pointer under the mutex so that
    // detached threads (keepalive, forward) immediately see NULL and stop using it.
    // Then destroy the agent OUTSIDE the lock — juice_destroy() waits for libjuice
    // internal threads to stop, and those threads may call our state callbacks which
    // also try to acquire cam->mutex. Destroying inside the lock would deadlock.
    pthread_mutex_lock(&cam->mutex);

    juice_agent_t *old_agent = cam->agent;
    cam->agent = NULL;  // Visible to callbacks before juice_destroy() returns

    // Close all dead application connections
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (cam->connections[i].active) {
            if (cam->connections[i].local_socket >= 0) {
                close(cam->connections[i].local_socket);
                cam->connections[i].local_socket = -1;
            }
            cam->connections[i].active = 0;
        }
    }

    cam->connected = 0;
    cam->needs_reconnect = 0;
    cam->reconnect_attempts++;
    cam->last_reconnect_time = time(NULL);
    /* Do NOT reset next_conn_id / global_seq_num on reconnect.  Resetting to
     * 1 / 0 risks colliding with late chunks from the previous session that
     * still carry the same low conn_id / seq window.  uint32 wrap is safe at
     * any realistic stream rate (years of continuous traffic).  */

    pthread_mutex_unlock(&cam->mutex);

    // Destroy outside the lock — safe now that cam->agent is NULL
    if (old_agent) {
        juice_destroy(old_agent);
    }

    // Single ICE attempt per reconnect.
    //
    // When TURN is configured, include BOTH STUN-reflexive and TURN-relay candidates
    // in the same ICE agent.  ICE selects the best working path automatically
    // (priority: HOST > SRFLX > RELAY), so there is no need for a STUN-first /
    // TURN-fallback two-attempt loop.  The two-attempt pattern sent two REQUESTs to
    // the signaling server, causing two CONSUMER_CONNECTs at the provider — the
    // provider destroyed the first live ICE negotiation to start the second one,
    // which then timed out.
    //
    // Proactive TURN refresh uses the same single-attempt path.  The STUN-upgrade
    // probe (job 2 in the monitor) still works: if a direct path opens up after we
    // have been on TURN, the ICE agent will select SRFLX/HOST over RELAY and the
    // CONNECTED handler clears cam->using_turn accordingly.

    if (cam->turn_refresh_needed) {
        cam->turn_refresh_needed = 0;
        printf("[~] Camera %s: Proactive TURN refresh — reconnecting via TURN directly...\n",
               cam->camera_id);
    }

    int use_turn = (config.turn_server_enabled && config.turn_server_host[0] != '\0') ? 1 : 0;
    if (reconnect_camera_ice_attempt(cam, use_turn) == 0) {
        return 0;
    }

    cam->needs_reconnect = 1;
    return -1;
}

// Monitor thread: wakes every 5 seconds.
//
// Three jobs:
//  1. Reconnect cameras where needs_reconnect==1 (failure recovery).
//     Backoff schedule: 5s → 10s → 20s → 30s (capped).
//
//  2. STUN-upgrade probe: when a camera is currently connected via TURN relay,
//     check every STUN_UPGRADE_INTERVAL seconds whether a direct/STUN path
//     has become available. If so, the reconnect will succeed via STUN and
//     cam->using_turn is cleared. If not, we stay on TURN and probe again later.
//
//  3. Proactive TURN refresh: TURN (RFC 5766 — a separate protocol that extends
//     STUN) CreatePermission expires at ~300s, but the effective window is shorter
//     (~180s observed) because ICE setup time consumes part of the permission
//     lifetime.  At TURN_REFRESH_INTERVAL (default 120s) we force a quick
//     TURN-only reconnect before the permission expires, preventing the
//     "Lost connectivity" dropout observed at ~181s uptime.
static void* reconnect_monitor_func(void* arg) {
    (void)arg;
    printf("[✓] Reconnect monitor started\n");

    while (vms_state.running) {
        sleep(5);

        for (int i = 0; i < MAX_CAMERAS; i++) {
            camera_session_t* cam = &vms_state.cameras[i];

            if (!cam->active) continue;

            // --- STUN-upgrade probe (job 2) -----------------------------------
            // Only when actively connected via TURN and not already reconnecting.
            if (cam->connected && cam->using_turn && !cam->needs_reconnect
                    && STUN_UPGRADE_INTERVAL > 0) {
                time_t now = time(NULL);
                time_t since_check = now - cam->last_stun_check;
                time_t since_redirection = now - cam->last_redirection_time;

                if (since_check >= STUN_UPGRADE_INTERVAL && since_redirection >= REDIRECTION_COOLDOWN) {
                    printf("[~] Camera %s: On TURN for %lds (limit %ds) — probing STUN recovery...\n",
                           cam->camera_id, (long)(now - cam->turn_since), REDIRECTION_COOLDOWN);
                    // Trigger a fresh ICE negotiation.
                    pthread_mutex_lock(&cam->mutex);
                    cam->last_stun_check = now;
                    cam->needs_reconnect  = 1;
                    cam->reconnect_attempts = 0;
                    cam->last_reconnect_time = 0;
                    pthread_mutex_unlock(&cam->mutex);
                }
            }
            
            // --- P2P Ping-Pong Timeout watchdog ---
            // With libjuice consent-freshness disabled, this is the primary
            // detector of a truly dead path.  Applies to BOTH STUN and TURN —
            // a relay can fail just as a direct path can.  Requires
            // PONG_TIMEOUT_STRIKES consecutive timeout windows (cleared by any
            // PONG arrival, see on_recv_camera) so a brief loss burst never
            // forces a reconnect.  Backoff is honoured — last_reconnect_time
            // is NOT zeroed, so repeated false positives on a flaky link
            // don't degrade into a 5 s reconnect loop.
            if (cam->connected && !cam->needs_reconnect) {
                time_t now = time(NULL);
                if (now - cam->last_pong_at > PONG_TIMEOUT &&
                    now - cam->connection_start_time > PONG_TIMEOUT) {
                    pthread_mutex_lock(&cam->mutex);
                    cam->pong_strikes++;
                    int strikes = cam->pong_strikes;
                    pthread_mutex_unlock(&cam->mutex);

                    if (strikes >= PONG_TIMEOUT_STRIKES) {
                        printf("[!] Camera %s: P2P path dead (no PONG for %lds, strikes=%d, mode=%s) — reconnecting\n",
                               cam->camera_id, (long)(now - cam->last_pong_at),
                               strikes, cam->using_turn ? "TURN" : "STUN");
                        pthread_mutex_lock(&cam->mutex);
                        cam->needs_reconnect = 1;
                        cam->pong_strikes = 0;
                        cam->last_redirection_time = now;
                        /* last_reconnect_time intentionally NOT zeroed: keep
                         * exponential backoff active so a flaky link can't
                         * cause continuous reconnect storms.  */
                        pthread_mutex_unlock(&cam->mutex);
                    } else {
                        printf("[~] Camera %s: PONG timeout strike %d/%d (no PONG for %lds) — waiting before reconnect\n",
                               cam->camera_id, strikes, PONG_TIMEOUT_STRIKES,
                               (long)(now - cam->last_pong_at));
                        /* Refresh last_pong_at so the next strike requires
                         * another full PONG_TIMEOUT window of silence.  */
                        pthread_mutex_lock(&cam->mutex);
                        cam->last_pong_at = now;
                        pthread_mutex_unlock(&cam->mutex);
                    }
                }
            }

            // --- Proactive TURN permission refresh (job 3) --------------------
            // TURN CreatePermission effective lifetime observed ~180s (300s RFC
            // minus ICE negotiation time).  Refresh at TURN_REFRESH_INTERVAL
            // (default 120s) to stay well inside the window.
            if (cam->connected && cam->using_turn && !cam->needs_reconnect
                    && config.turn_refresh_interval > 0) {
                time_t turn_age = time(NULL) - cam->turn_since;
                if (turn_age >= (time_t)config.turn_refresh_interval) {
                    printf("[~] Camera %s: TURN session age %lds >= %ds — proactive refresh\n",
                           cam->camera_id, (long)turn_age, config.turn_refresh_interval);
                    pthread_mutex_lock(&cam->mutex);
                    cam->turn_refresh_needed = 1;
                    cam->needs_reconnect = 1;
                    cam->reconnect_attempts = 0;
                    cam->last_reconnect_time = 0;  // No backoff — time-sensitive
                    pthread_mutex_unlock(&cam->mutex);
                }
            }

            // --- Failure reconnect (job 1) -------------------------------------
            if (!cam->needs_reconnect) continue;

            // Exponential backoff capped at 30 s
            int exp = cam->reconnect_attempts < 3 ? cam->reconnect_attempts : 3;
            int backoff = 5 * (1 << exp);  // 5, 10, 20, 30 s
            long elapsed = (long)(time(NULL) - cam->last_reconnect_time);

            if (elapsed < backoff) {
                printf("[~] Camera %s: Reconnect in %lds (attempt %d)\n",
                       cam->camera_id, (long)backoff - elapsed,
                       cam->reconnect_attempts + 1);
                continue;
            }

            reconnect_camera_ice(cam);
        }
    }

    printf("[!] Reconnect monitor stopped\n");
    return NULL;
}

// ============================================================================
// CONNECT TO CAMERA
// ============================================================================

static int wait_for_camera_connected(camera_session_t* cam, int timeout_sec) {
    for (int i = 0; i < timeout_sec; i++) {
        sleep(1);
        if (cam->connected) {
            return 1;
        }
    }
    return 0;
}

static int connect_to_camera(const char* camera_id) {
    printf("\n[...] Connecting to camera: %s\n", camera_id);
    
    // Check if already connected / stale session exists
    camera_session_t* existing = find_camera_session(camera_id);
    if (existing) {
        if (existing->connected) {
            printf("[✓] Already connected to camera: %s (port %d)\n",
                   camera_id, existing->local_listen_port);
            return existing->local_listen_port;
        }

        // Avoid duplicate sessions with same camera_id; recreate cleanly.
        printf("[!] Camera %s has stale disconnected session; recreating session...\n", camera_id);
        free_camera_session(existing);
        usleep(150000);
    }

    // Gather ALL candidates (HOST + SRFLX + RELAY) in a single ICE agent.
    // ICE naturally picks the best path: HOST > SRFLX > RELAY.
    // If the direct path improves later, ICE connectivity checks will promote it
    // without any application-level intervention or stream interruption.
    // The old two-step approach (STUN first, then TURN) sent two CONSUMER_CONNECTs
    // to the signaling server — the second could be suppressed by provider debounce
    // (15s), causing the consumer to wait forever.
    int port = connect_to_camera_attempt(camera_id, 1, 30);
    if (port > 0) return port;

    return -1;
}

static int connect_to_camera_attempt(const char* camera_id, int use_turn, int timeout_sec) {
    // Allocate new session for this attempt
    camera_session_t* cam = allocate_camera_session(camera_id);
    if (!cam) {
        printf("[X] Failed to allocate session for camera: %s\n", camera_id);
        return -1;
    }

    // Generate a random peer_id for this session to avoid collisions
    char peer_id[32];
    snprintf(peer_id, sizeof(peer_id), "consumer_%u", (unsigned int)time(NULL) % 1000000);

    // Create ICE agent
    juice_config_t juice_config;
    memset(&juice_config, 0, sizeof(juice_config));
    juice_config.cb_state_changed = on_state_changed_camera;
    juice_config.cb_recv = on_recv_camera;
    juice_config.cb_candidate = on_candidate_camera;
    juice_config.cb_gathering_done = on_gathering_done_camera;
    juice_config.user_ptr = cam;
    juice_config.bind_address = "0.0.0.0";

    // STUN-first mode: always use STUN.
    // TURN mode: enable relay fallback in addition to STUN.
    if (config.turn_server_enabled && config.turn_server_host[0] != '\0') {
        juice_config.stun_server_host = config.turn_server_host;
        juice_config.stun_server_port = config.turn_server_port;

        if (use_turn) {
            memset(&g_turn_server, 0, sizeof(g_turn_server));
            g_turn_server.host = config.turn_server_host;
            g_turn_server.port = config.turn_server_port;
            if (config.turn_server_username[0] != '\0')
                g_turn_server.username = config.turn_server_username;
            if (config.turn_server_password[0] != '\0')
                g_turn_server.password = config.turn_server_password;
            juice_config.turn_servers = &g_turn_server;
            juice_config.turn_servers_count = 1;

            printf("[✓] Camera %s: Attempt mode TURN fallback enabled (%s:%d)\n",
                   camera_id, config.turn_server_host, config.turn_server_port);
        } else {
            printf("[✓] Camera %s: Attempt mode STUN-only (%s:%d)\n",
                   camera_id, config.turn_server_host, config.turn_server_port);
        }
    } else {
        juice_config.stun_server_host = "74.125.250.129";
        juice_config.stun_server_port = 19302;
        printf("[!] Camera %s: No coturn configured, using Google STUN only\n", camera_id);
    }

    cam->agent = juice_create(&juice_config);
    if (!cam->agent) {
        printf("[X] Failed to create ICE agent for camera: %s\n", camera_id);
        free_camera_session(cam);
        return -1;
    }

    // Generate local SDP
    char local_sdp[MAX_SDP_LEN];
    int ret = juice_get_local_description(cam->agent, local_sdp, MAX_SDP_LEN);
    if (ret < 0) {
        printf("[X] Failed to get local SDP\n");
        free_camera_session(cam);
        return -1;
    }

    printf("[...] Gathering ICE candidates...\n");
    cam->gathering_done = 0;  // Reset before triggering gathering
    juice_gather_candidates(cam->agent);
    // Wait for on_gathering_done_camera to fire — this guarantees the local SDP
    // contains ALL candidates (HOST + SRFLX + RELAY) before we send it to the
    // signaling server.  Do NOT use juice_get_state() != GATHERING: libjuice
    // transitions to CONNECTING after the first HOST candidate is ready, while
    // STUN/TURN responses (SRFLX/RELAY) are still in flight.
    /* 50 ms × 60 = 3 s.  See reconnect path for rationale.  */
    for (int gi = 0; gi < 60 && !cam->gathering_done; gi++) {
        usleep(50000);
    }
    if (!cam->gathering_done) {
        printf("[!] Camera %s: ICE gathering still pending at 3 s — proceeding with current candidates\n",
               camera_id);
    }

    ret = juice_get_local_description(cam->agent, local_sdp, MAX_SDP_LEN);
    if (ret < 0) {
        printf("[X] Failed to get updated SDP\n");
        free_camera_session(cam);
        return -1;
    }

    // Send connection request to signaling server
    char request_msg[MAX_SDP_LEN + 512];
    snprintf(request_msg, sizeof(request_msg),
             "type=REQUEST\nservice_id=%s\npeer_id=%s\nconsumer_sdp=%s",
             camera_id, peer_id, local_sdp);

    printf("[...] Sending connection request to signaling server...\n");
    char response[MAX_SDP_LEN + 512];
    int got_response = 0;

    for (int attempt = 0; attempt < 2 && !got_response; attempt++) {
        pthread_mutex_lock(&vms_state.signaling_mutex);
        int sent_ok = send_message(vms_state.signaling_socket, request_msg);
        int recv_ok = sent_ok ? recv_message(vms_state.signaling_socket, response, sizeof(response)) : 0;
        pthread_mutex_unlock(&vms_state.signaling_mutex);

        if (sent_ok && recv_ok) {
            got_response = 1;
            break;
        }

        // Only retry when send FAILED (REQUEST not delivered — safe to resend).
        if (!sent_ok && attempt == 0) {
            printf("[!] Send failed; reconnecting signaling socket and retrying...\n");
            pthread_mutex_lock(&vms_state.signaling_mutex);
            if (vms_state.signaling_socket >= 0) {
                close(vms_state.signaling_socket);
            }
            vms_state.signaling_socket = -1;
            pthread_mutex_unlock(&vms_state.signaling_mutex);

            if (!init_signaling_connection()) {
                printf("[X] Failed to reconnect signaling socket\n");
                free_camera_session(cam);
                return -1;
            }
        } else {
            // sent_ok=1 but recv_ok=0: REQUEST delivered but response lost.
            // Do NOT retry — the provider already got CONSUMER_CONNECT.
            break;
        }
    }

    if (!got_response) {
        printf("[X] Failed to receive response from signaling server\n");
        free_camera_session(cam);
        return -1;
    }

    if (strstr(response, "status=not_found") != NULL) {
        printf("[X] Camera %s not found on signaling server\n", camera_id);
        free_camera_session(cam);
        return -1;
    }
    if (strstr(response, "type=ERROR") != NULL) {
        printf("[X] Camera %s not found (Provider not registered with this ID)\n", camera_id);
        printf("[!] Ensure camera provider.conf has SERVICE_ID=%s\n", camera_id);
        free_camera_session(cam);
        return -1;
    }
    if (strstr(response, "type=PROVIDER_INFO") == NULL) {
        printf("[X] Invalid response from signaling server\n");
        free_camera_session(cam);
        return -1;
    }

    // Extract protocol (optional, default rtsp)
    char* proto_start = strstr(response, "\nprotocol=");
    if (proto_start) {
        proto_start += strlen("\nprotocol=");
    } else if (strncmp(response, "protocol=", 9) == 0) {
        proto_start = response + strlen("protocol=");
    }
    if (proto_start) {
        char* proto_end = strchr(proto_start, '\n');
        int proto_len = proto_end ? (int)(proto_end - proto_start) : (int)strlen(proto_start);
        while (proto_len > 0 && proto_start[proto_len - 1] == '\r') proto_len--;
        if (proto_len > 0 && proto_len < (int)sizeof(cam->service_protocol) - 1) {
            memcpy(cam->service_protocol, proto_start, (size_t)proto_len);
            cam->service_protocol[proto_len] = '\0';
        }
        printf("[✓] Camera %s: Protocol = %s\n", camera_id, cam->service_protocol);
    }

    // Extract codec (optional, default h264)
    char* codec_start = strstr(response, "\ncodec=");
    if (codec_start) {
        codec_start += strlen("\ncodec=");
    } else if (strncmp(response, "codec=", 6) == 0) {
        codec_start = response + strlen("codec=");
    }
    if (codec_start) {
        char* codec_end = strchr(codec_start, '\n');
        int codec_len = codec_end ? (int)(codec_end - codec_start) : (int)strlen(codec_start);
        while (codec_len > 0 && codec_start[codec_len - 1] == '\r') codec_len--;
        if (codec_len > 0 && codec_len < (int)sizeof(cam->service_codec) - 1) {
            memcpy(cam->service_codec, codec_start, (size_t)codec_len);
            cam->service_codec[codec_len] = '\0';
        }
        printf("[✓] Camera %s: Codec = %s\n", camera_id, cam->service_codec);
    }

    // Extract provider SDP
    char* sdp_start = strstr(response, "provider_sdp=");
    if (!sdp_start) {
        printf("[X] No provider SDP in response\n");
        free_camera_session(cam);
        return -1;
    }
    sdp_start += strlen("provider_sdp=");

    printf("[...] Setting remote SDP...\n");
    char sanitized_sdp[MAX_SDP_LEN];
    int dropped = sanitize_remote_sdp(sdp_start, sanitized_sdp, sizeof(sanitized_sdp));
    if (dropped < 0) {
        printf("[X] Camera %s: Remote SDP too large after sanitization\n", camera_id);
        free_camera_session(cam);
        return -1;
    }
    if (dropped > 0) {
        printf("[!] Camera %s: Dropped %d unsupported ICE candidate(s) from remote SDP\n",
               camera_id, dropped);
    }
    ret = juice_set_remote_description(cam->agent, sanitized_sdp);
    if (ret < 0) {
        printf("[X] Failed to set remote SDP\n");
        free_camera_session(cam);
        return -1;
    }
    // Tell libjuice that the remote side has finished gathering — all remote
    // candidates are in the SDP we just set (they come from the signaling server
    // in one shot, so no trickle ICE).  Without this call libjuice may delay
    // connectivity checks waiting for more remote candidates that will never arrive.
    juice_set_remote_gathering_done(cam->agent);

    printf("[✓] ICE negotiation started for camera: %s\n", camera_id);

    // Create local listen socket
    cam->listen_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (cam->listen_socket < 0) {
        printf("[X] Failed to create listen socket\n");
        free_camera_session(cam);
        return -1;
    }

    int opt = 1;
    setsockopt(cam->listen_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in listen_addr;
    memset(&listen_addr, 0, sizeof(listen_addr));
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_addr.s_addr = INADDR_ANY;
    listen_addr.sin_port = 0;

    if (bind(cam->listen_socket, (struct sockaddr*)&listen_addr, sizeof(listen_addr)) < 0) {
        printf("[X] Failed to bind listen socket\n");
        free_camera_session(cam);
        return -1;
    }

    socklen_t addr_len = sizeof(listen_addr);
    getsockname(cam->listen_socket, (struct sockaddr*)&listen_addr, &addr_len);
    cam->local_listen_port = ntohs(listen_addr.sin_port);

    if (listen(cam->listen_socket, 5) < 0) {
        printf("[X] Failed to listen\n");
        free_camera_session(cam);
        return -1;
    }

    printf("[✓] Camera %s: Local listen port %d\n", camera_id, cam->local_listen_port);

    // Start threads
    pthread_create(&cam->accept_thread,   NULL, accept_thread_func,   cam);
    pthread_create(&cam->cleanup_thread,  NULL, cleanup_thread_func,  cam);
    pthread_create(&cam->keepalive_thread,NULL, keepalive_thread_func,cam);
    /* Mark dispatch-running BEFORE create so the enqueue watchdog does not
       race and spawn a duplicate thread if a frame arrives instantly. */
    cam->dispatch_running = 1;
    pthread_create(&cam->dispatch_thread, NULL, dispatch_thread_func, cam);

    pthread_detach(cam->accept_thread);
    pthread_detach(cam->cleanup_thread);
    pthread_detach(cam->keepalive_thread);
    pthread_detach(cam->dispatch_thread);   /* previously leaked — not detached, never joined */

    printf("[...] Waiting for P2P connection...\n");
    if (wait_for_camera_connected(cam, timeout_sec)) {
        const char* ready_proto = (cam->service_protocol[0] != '\0') ? cam->service_protocol : "rtsp";
        printf("\n[✓] Camera %s: Ready! Stream URL: %s://localhost:%d/\n\n",
               camera_id, ready_proto, cam->local_listen_port);
        return cam->local_listen_port;
    }

    printf("[X] Timeout waiting for P2P connection (%s mode)\n", use_turn ? "TURN" : "STUN");
    free_camera_session(cam);
    return -1;
}

// ============================================================================
// SIGNALING CONNECTION
// ============================================================================

static int init_signaling_connection() {
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", config.signaling_port);

    printf("[...] Resolving signaling server %s:%d...\n",
           config.signaling_server, config.signaling_port);

    if (getaddrinfo(config.signaling_server, port_str, &hints, &res) != 0) {
        printf("[X] Failed to resolve signaling server %s\n", config.signaling_server);
        return 0;
    }

    vms_state.signaling_socket = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (vms_state.signaling_socket < 0) {
        printf("[X] Failed to create signaling socket\n");
        freeaddrinfo(res);
        return 0;
    }

    printf("[...] Connecting to signaling server...\n");

    if (connect(vms_state.signaling_socket, res->ai_addr, res->ai_addrlen) < 0) {
        printf("[X] Failed to connect to signaling server\n");
        freeaddrinfo(res);
        return 0;
    }

    freeaddrinfo(res);
    printf("[✓] Connected to signaling server\n");
    return 1;
}

// ============================================================================
// SIGNAL HANDLING
// ============================================================================

static void consumer_signal_handler(int sig) {
    (void)sig;
    vms_state.running = 0;
}

// ============================================================================
// MAIN
// ============================================================================

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printf("Usage: %s <config_file>\n", argv[0]);
        return 1;
    }

    g_log_file = fopen("consumer.log", "a");
    
    printf("\n╔════════════════════════════════════════════╗\n");
    printf(" Multi-Camera VMS Consumer\n");
    printf("════════════════════════════════════════════\n\n");

    // Suppress verbose ICE/STUN/TURN debug output — only warnings and errors.
    juice_set_log_level(JUICE_LOG_LEVEL_WARN);

    /* Raise the open-file-descriptor soft limit to the hard limit. The
       dispatch thread dup()s the client socket once per completed frame so
       it can safely close it after send(); with MAX_CAMERAS * MAX_CONNECTIONS
       * DISPATCH_QUEUE_SIZE potentially in flight, the default 1024 limit on
       many Linux distros would starve and drop frames at scale. */
    {
        struct rlimit rl;
        if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
            /* Aim for 65536; clamp to the kernel-enforced hard limit.
               (The previous version had a no-op `wanted = rl.rlim_max`
               assignment that never actually raised the soft limit.) */
            rlim_t target = 65536;
            if (target > rl.rlim_max) target = rl.rlim_max;
            if (rl.rlim_cur < target) {
                rl.rlim_cur = target;
                if (setrlimit(RLIMIT_NOFILE, &rl) != 0) {
                    printf("[!] setrlimit(RLIMIT_NOFILE, %lu) failed: %s\n",
                           (unsigned long)target, strerror(errno));
                } else {
                    printf("[✓] RLIMIT_NOFILE raised to %lu (hard=%lu)\n",
                           (unsigned long)target, (unsigned long)rl.rlim_max);
                }
            }
        }
    }

    // Ignore SIGPIPE — broken sockets return EPIPE instead of killing the process.
    // Register graceful shutdown handlers for SIGINT (Ctrl-C) and SIGTERM.
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT,  consumer_signal_handler);
    signal(SIGTERM, consumer_signal_handler);

    // Initialize VMS state
    memset(&vms_state, 0, sizeof(vms_state));
    pthread_mutex_init(&vms_state.global_mutex, NULL);
    pthread_mutex_init(&vms_state.signaling_mutex, NULL);
    vms_state.running = 1;
    vms_state.signaling_socket = -1;

    // Load configuration
    if (!load_config(argv[1], &config)) {
        return 1;
    }

    // Initialize signaling connection
    if (!init_signaling_connection()) {
        return 1;
    }

    printf("\n[✓] VMS Consumer initialized\n");
    printf("[✓] Ready to connect to cameras\n\n");

    // Start HTTP API server
    if (!start_http_api_server()) {
        printf("[X] Failed to start HTTP API server\n");
        return 1;
    }

    // Start auto-reconnect monitor
    pthread_t reconnect_thread;
    pthread_create(&reconnect_thread, NULL, reconnect_monitor_func, NULL);
    pthread_detach(reconnect_thread);

    printf("\n[✓] VMS Consumer running. Press Ctrl+C to stop.\n\n");

    while (vms_state.running) {
        sleep(1);
    }

    // Cleanup
    printf("\n[...] Shutting down...\n");
    vms_state.running = 0;

    for (int i = 0; i < MAX_CAMERAS; i++) {
        if (vms_state.cameras[i].active) {
            free_camera_session(&vms_state.cameras[i]);
        }
    }

    if (vms_state.signaling_socket >= 0) {
        close(vms_state.signaling_socket);
    }

    printf("[✓] VMS Consumer shutdown complete\n");
    return 0;
}
