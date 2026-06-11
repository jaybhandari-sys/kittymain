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
// ============================================================================

#include "juice/juice.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <time.h>
#include <signal.h>
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
#define MAX_SERVICES 8
#define REASSEMBLY_TIMEOUT 30
/* KEEPALIVE_INTERVAL removed — see comment above the deleted keepalive_thread.  */
#define HEADER_SIZE 16         /* conn_id(4)+seq(4)+total_size(4)+offset(4) */
#define CONFIG_LINE_MAX 256
#define MAX_CHUNK_PAYLOAD (MAX_UDP_PAYLOAD - HEADER_SIZE)                /* 1184 */
#define MAX_CHUNKS_PER_FRAME ((BUFFER_SIZE / MAX_CHUNK_PAYLOAD) + 2)    /* ~444 */

// ============================================================================
// DATA STRUCTURES
// ============================================================================

// Per-service configuration (one [section] in config file)
typedef struct {
    char service_id[128];
    char protocol[16];
    char codec[16];         // "h264" or "h265" (default "h264")
    int  local_port;
    int  p2p_port;
    int  consumer_port;     // Desired port on consumer side (0 = let consumer decide)
} service_config_t;

// Global configuration (shared by all services)
typedef struct {
    char signaling_server[256];
    int  signaling_port;
    char api_token[256];        // Must match signaling server API_TOKEN if set
    // TURN server configuration
    char turn_server_host[256];
    uint16_t turn_server_port;
    char turn_server_username[256];
    char turn_server_password[256];
    int turn_server_enabled;
} global_config_t;

// Per-TCP-connection state (forwarding a single TCP stream over P2P)
typedef struct {
    uint32_t conn_id;
    int local_socket;
    pthread_t forward_thread;
    int active;
    uint8_t  *reassembly_buffer;   /* heap-allocated (BUFFER_SIZE bytes) */
    uint8_t  *spare_buffer;        /* standby buffer swapped in when a frame */
                                   /* completes, so the ICE thread never     */
                                   /* memcpy's 4 MB while holding svc->mutex. */
    uint8_t  *chunk_received;      /* heap-allocated (MAX_CHUNKS_PER_FRAME bytes) */
    uint32_t  expected_size;       /* total frame size announced by sender */
    uint32_t  bytes_received;      /* bytes accumulated so far (out-of-order safe) */
    uint32_t  last_seq_num;
    time_t    last_activity;
} connection_t;

// Forward declaration
struct service_state;

// Argument passed to forward thread
typedef struct {
    connection_t *conn;
    struct service_state *svc;
} forward_arg_t;

// Per-service runtime state
typedef struct service_state {
    service_config_t cfg;
    juice_agent_t *agent;
    juice_config_t juice_cfg;
    int connected;
    pthread_mutex_t mutex;
    connection_t connections[MAX_CONNECTIONS];
    uint32_t next_conn_id;
    uint32_t global_seq_num;
    time_t connection_start_time;
    int keepalive_enabled;
    int has_had_consumer;
    char saved_ufrag[64];
    char saved_pwd[64];
    // ICE gathering completion — set by on_gathering_done, polled before sending SDP
    volatile int gathering_done;
    // Debounce rapid CONSUMER_CONNECTs — if a second arrives within this cooldown,
    // ignore it so the first ICE negotiation can complete uninterrupted.
    time_t last_consumer_connect_time;
    // Set to 1 by on_state_changed (libjuice thread) when ICE reaches FAILED.
    // Main loop reads this flag and sends PROVIDER_DISCONNECTED to the signaling
    // server so the dashboard clears "P2P Active" correctly.
    volatile int needs_disconnect_notify;
} service_state_t;

// Globals
static global_config_t g_config;
static service_state_t services[MAX_SERVICES];
static int num_services = 0;
static int sig_socket = -1;
static volatile sig_atomic_t g_running = 1;

// TURN server (shared, static so pointers remain valid)
static juice_turn_server_t g_turn_server;

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

static int is_separator_line(const char* str) {
    // Ignore decorative divider lines to avoid noisy parser warnings.
    const unsigned char* p = (const unsigned char*)str;
    while (*p) {
        if (*p == ' ' || *p == '\t' || *p == '-' || *p == '=' || *p == '*' || *p == '_') {
            p++;
            continue;
        }
        return 0;
    }
    return 1;
}

/* Forward declaration — defined later after config parser */
static ssize_t send_all(int fd, const void* buf, size_t len, int flags);

static int send_message(int socket_fd, const char* msg) {
    uint32_t msg_len_net = htonl((uint32_t)strlen(msg));
    if (send_all(socket_fd, &msg_len_net, sizeof(msg_len_net), MSG_NOSIGNAL) < 0) return 0;
    if (send_all(socket_fd, msg, strlen(msg), MSG_NOSIGNAL) < 0) return 0;
    return 1;
}

static int recv_message(int socket_fd, char* buffer, size_t buffer_size) {
    uint32_t msg_len;
    if (recv(socket_fd, &msg_len, sizeof(msg_len), MSG_WAITALL) != sizeof(msg_len)) return 0;
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
    if (recv(socket_fd, buffer, msg_len, MSG_WAITALL) != (ssize_t)msg_len) return 0;
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

// Reliable send — loops until all bytes are written or error.
// Used ONLY on threads where blocking is acceptable (signaling thread, etc).
// MUST NOT be called from the libjuice ICE callback thread — use
// send_best_effort_nonblocking() there instead.
static ssize_t send_all(int fd, const void* buf, size_t len, int flags) {
    size_t sent = 0;
    int stall_budget = 6;   /* up to 6 × 50 ms = 300 ms if reader is stalled */
    while (sent < len) {
        ssize_t n = send(fd, (const char*)buf + sent, len - sent, flags);
        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (stall_budget-- <= 0) return -1;
            struct pollfd pfd = { .fd = fd, .events = POLLOUT, .revents = 0 };
            int pr = poll(&pfd, 1, 50);
            if (pr > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) return -1;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return -1;
    }
    return (ssize_t)sent;
}

/* Strictly non-blocking send for use on the libjuice ICE callback thread.
 * The ICE thread MUST return to libjuice's internal poll loop quickly — any
 * wall-clock time spent here is time that incoming UDP datagrams from the
 * consumer accumulate in the kernel rx queue (and get dropped once the queue
 * overflows).  A 300 ms blocking send_all on this thread was the single
 * biggest reason the stream stalled after a few seconds of apparent success.
 *
 * Strategy: try send() up to ~1 ms total wall time (sched_yield between
 * attempts).  If bytes can't be pushed in that window, drop the frame.
 * Returns:  >= 0  bytes actually written (may be short if partial)
 *            -1   error (peer closed, EPIPE, etc.)
 */
static ssize_t send_best_effort_nonblocking(int fd, const void* buf, size_t len, int flags) {
    size_t sent = 0;
    /* Up to ~8 micro-retries; each waits 100 µs — total worst-case ~800 µs. */
    for (int attempt = 0; attempt < 8 && sent < len; attempt++) {
        ssize_t n = send(fd, (const char*)buf + sent, len - sent,
                         flags | MSG_DONTWAIT);
        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* Local service's kernel tx buffer is full — yield briefly and retry. */
                usleep(100);
                continue;
            }
            if (errno == EINTR) continue;
            /* EPIPE / ECONNRESET / any other fatal */
            return -1;
        }
    }
    return (ssize_t)sent;
}

// ============================================================================
// CONFIG PARSER (supports INI-style sections + legacy flat format)
// ============================================================================

static int load_config(const char* config_file) {
    FILE* fp = fopen(config_file, "r");
    if (!fp) {
        printf("[X] Failed to open config file: %s\n", config_file);
        return 0;
    }

    // Set global defaults
    g_config.signaling_server[0] = '\0';
    g_config.signaling_port = 8888;
    g_config.turn_server_enabled = 0;
    g_config.turn_server_port = 3478;
    g_config.turn_server_host[0] = '\0';
    g_config.turn_server_username[0] = '\0';
    g_config.turn_server_password[0] = '\0';
    num_services = 0;

    // Legacy flat-format fields (backward compat)
    char legacy_service_id[128] = "";
    char legacy_protocol[16] = "rtsp";
    char legacy_codec[16] = "h264";
    int legacy_local_port = 554;
    int legacy_p2p_port = 9100;

    int current_section = -1;  // -1 = global scope
    char line[CONFIG_LINE_MAX];
    int line_num = 0;

    while (fgets(line, sizeof(line), fp)) {
        line_num++;
        char* trimmed = trim_whitespace(line);
        if (strlen(trimmed) == 0 || trimmed[0] == '#') continue;

        // Detect [SECTION_NAME]
        if (trimmed[0] == '[') {
            char* end_bracket = strchr(trimmed, ']');
            if (!end_bracket) {
                printf("[!] Warning: Malformed section header on line %d\n", line_num);
                continue;
            }
            if (num_services >= MAX_SERVICES) {
                printf("[X] Too many services (max %d)\n", MAX_SERVICES);
                continue;
            }
            *end_bracket = '\0';
            char* section_name = trim_whitespace(trimmed + 1);

            current_section = num_services;
            memset(&services[current_section], 0, sizeof(service_state_t));
            strncpy(services[current_section].cfg.service_id, section_name, sizeof(services[0].cfg.service_id) - 1);
            strcpy(services[current_section].cfg.protocol, "rtsp");
            strcpy(services[current_section].cfg.codec, "h264");
            services[current_section].cfg.local_port = 554;
            services[current_section].cfg.p2p_port = 9100 + num_services;
            services[current_section].cfg.consumer_port = 0;  // 0 = let consumer decide
            services[current_section].keepalive_enabled = 1;
            services[current_section].next_conn_id = 1;
            pthread_mutex_init(&services[current_section].mutex, NULL);
            num_services++;

            printf("[✓] Config: Service [%s]\n", section_name);
            continue;
        }

        // Parse key=value
        char* equals = strchr(trimmed, '=');
        if (!equals) {
            if (is_separator_line(trimmed)) continue;
            printf("[!] Warning: Invalid line %d (no '=' found)\n", line_num);
            continue;
        }
        *equals = '\0';
        char* key = trim_whitespace(trimmed);
        char* value = trim_whitespace(equals + 1);

        if (current_section < 0) {
            // Global scope keys
            if (strcmp(key, "SIGNALING_SERVER") == 0) {
                strncpy(g_config.signaling_server, value, sizeof(g_config.signaling_server) - 1);
                printf("[✓] Config: SIGNALING_SERVER = %s\n", g_config.signaling_server);
            } else if (strcmp(key, "SIGNALING_PORT") == 0) {
                g_config.signaling_port = atoi(value);
                printf("[✓] Config: SIGNALING_PORT = %d\n", g_config.signaling_port);
            } else if (strcmp(key, "TURN_SERVER_HOST") == 0) {
                strncpy(g_config.turn_server_host, value, sizeof(g_config.turn_server_host) - 1);
                g_config.turn_server_enabled = 1;
                printf("[✓] Config: TURN_SERVER_HOST = %s\n", g_config.turn_server_host);
            } else if (strcmp(key, "TURN_SERVER_PORT") == 0) {
                g_config.turn_server_port = (uint16_t)atoi(value);
                printf("[✓] Config: TURN_SERVER_PORT = %d\n", g_config.turn_server_port);
            } else if (strcmp(key, "TURN_SERVER_USERNAME") == 0) {
                strncpy(g_config.turn_server_username, value, sizeof(g_config.turn_server_username) - 1);
                printf("[✓] Config: TURN_SERVER_USERNAME = %s\n", g_config.turn_server_username);
            } else if (strcmp(key, "TURN_SERVER_PASSWORD") == 0) {
                strncpy(g_config.turn_server_password, value, sizeof(g_config.turn_server_password) - 1);
                printf("[✓] Config: TURN_SERVER_PASSWORD = (hidden)\n");
            } else if (strcmp(key, "API_TOKEN") == 0) {
                strncpy(g_config.api_token, value, sizeof(g_config.api_token) - 1);
                printf("[✓] Config: API_TOKEN = (hidden)\n");
            }
            // Legacy flat-format keys (no [section] headers)
            else if (strcmp(key, "SERVICE_ID") == 0) {
                strncpy(legacy_service_id, value, sizeof(legacy_service_id) - 1);
                printf("[✓] Config: SERVICE_ID = %s\n", legacy_service_id);
            } else if (strcmp(key, "SERVICE_PROTOCOL") == 0) {
                strncpy(legacy_protocol, value, sizeof(legacy_protocol) - 1);
                printf("[✓] Config: SERVICE_PROTOCOL = %s\n", legacy_protocol);
            } else if (strcmp(key, "SERVICE_CODEC") == 0 || strcmp(key, "CODEC") == 0) {
                strncpy(legacy_codec, value, sizeof(legacy_codec) - 1);
                printf("[✓] Config: SERVICE_CODEC = %s\n", legacy_codec);
            } else if (strcmp(key, "LOCAL_SERVICE_PORT") == 0) {
                legacy_local_port = atoi(value);
                printf("[✓] Config: LOCAL_SERVICE_PORT = %d\n", legacy_local_port);
            } else if (strcmp(key, "P2P_PORT") == 0) {
                legacy_p2p_port = atoi(value);
                printf("[✓] Config: P2P_PORT = %d\n", legacy_p2p_port);
            } else {
                printf("[!] Warning: Unknown config key '%s' on line %d\n", key, line_num);
            }
        } else {
            // Section scope keys
            service_config_t* sc = &services[current_section].cfg;
            if (strcmp(key, "PROTOCOL") == 0) {
                strncpy(sc->protocol, value, sizeof(sc->protocol) - 1);
                printf("[✓]   PROTOCOL = %s\n", sc->protocol);
            } else if (strcmp(key, "CODEC") == 0 || strcmp(key, "SERVICE_CODEC") == 0) {
                strncpy(sc->codec, value, sizeof(sc->codec) - 1);
                printf("[✓]   CODEC = %s\n", sc->codec);
            } else if (strcmp(key, "LOCAL_PORT") == 0) {
                sc->local_port = atoi(value);
                printf("[✓]   LOCAL_PORT = %d\n", sc->local_port);
            } else if (strcmp(key, "P2P_PORT") == 0) {
                sc->p2p_port = atoi(value);
                printf("[✓]   P2P_PORT = %d\n", sc->p2p_port);
            } else if (strcmp(key, "CONSUMER_PORT") == 0) {
                sc->consumer_port = atoi(value);
                printf("[✓]   CONSUMER_PORT = %d\n", sc->consumer_port);
            } else {
                printf("[!] Warning: Unknown section key '%s' on line %d\n", key, line_num);
            }
        }
    }

    fclose(fp);

    // Backward compatibility: if no [sections] found but SERVICE_ID was set, create one service
    if (num_services == 0 && legacy_service_id[0] != '\0') {
        memset(&services[0], 0, sizeof(service_state_t));
        snprintf(services[0].cfg.service_id, sizeof(services[0].cfg.service_id), "%s", legacy_service_id);
        snprintf(services[0].cfg.protocol,   sizeof(services[0].cfg.protocol),   "%s", legacy_protocol);
        snprintf(services[0].cfg.codec,      sizeof(services[0].cfg.codec),      "%s", legacy_codec);
        services[0].cfg.local_port = legacy_local_port;
        services[0].cfg.p2p_port = legacy_p2p_port;
        services[0].keepalive_enabled = 1;
        services[0].next_conn_id = 1;
        pthread_mutex_init(&services[0].mutex, NULL);
        num_services = 1;
        printf("[✓] Using legacy single-service config\n");
    }

    return 1;
}

// ============================================================================
// PER-SERVICE CONNECTION MANAGEMENT
// ============================================================================

static int connect_to_local_service(int port) {
    int local_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (local_socket < 0) return -1;

    struct sockaddr_in local_addr;
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_port = htons(port);
    local_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (connect(local_socket, (struct sockaddr*)&local_addr, sizeof(local_addr)) < 0) {
        close(local_socket);
        return -1;
    }

    // Disable Nagle: PTZ/control messages are small; Nagle adds up to 200ms latency.
    int nd = 1;
    setsockopt(local_socket, IPPROTO_TCP, TCP_NODELAY, &nd, sizeof(nd));

    /* Enlarge kernel receive buffer so H.264 / H.265 keyframes from the local
       camera service don't back-pressure into the camera process while the
       provider is still chunking the previous frame over ICE. 4 MB matches
       the reassembly buffer size on both ends. */
    int rcvbuf = 4 * 1024 * 1024;
    setsockopt(local_socket, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    int sndbuf = 1 * 1024 * 1024;
    setsockopt(local_socket, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    set_nonblocking(local_socket);
    return local_socket;
}

static connection_t* find_connection(service_state_t* svc, uint32_t conn_id) {
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (svc->connections[i].active && svc->connections[i].conn_id == conn_id) {
            return &svc->connections[i];
        }
    }
    return NULL;
}

static connection_t* create_connection(service_state_t* svc, uint32_t conn_id) {
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (!svc->connections[i].active) {
            connection_t *c = &svc->connections[i];
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
            c->conn_id       = conn_id;
            c->local_socket  = -1;
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

// ============================================================================
// DATA FORWARDING (per service)
// ============================================================================

static void* forward_local_to_p2p(void* arg) {
    forward_arg_t* farg = (forward_arg_t*)arg;
    connection_t* conn = farg->conn;
    service_state_t* svc = farg->svc;
    free(farg);

    uint8_t *buffer = malloc(BUFFER_SIZE);
    uint8_t  packet[MAX_UDP_PAYLOAD];
    if (!buffer) return NULL;

    while (svc->connected && conn->active && conn->local_socket >= 0) {
        ssize_t bytes_read = recv(conn->local_socket, buffer, BUFFER_SIZE, 0);

        if (bytes_read > 0) {
            // Build and send each chunk, holding mutex only briefly per juice_send
            size_t offset = 0;
            while (offset < (size_t)bytes_read) {
                size_t chunk_size = bytes_read - offset;
                if (chunk_size > MAX_UDP_PAYLOAD - HEADER_SIZE)
                    chunk_size = MAX_UDP_PAYLOAD - HEADER_SIZE;

                uint32_t seq = __sync_fetch_and_add(&svc->global_seq_num, 1);
                uint32_t conn_id_net  = htonl(conn->conn_id);
                uint32_t seq_net      = htonl(seq);
                uint32_t total_net    = htonl((uint32_t)bytes_read);
                uint32_t offset_net   = htonl((uint32_t)offset);

                memcpy(packet,      &conn_id_net,  4);
                memcpy(packet + 4,  &seq_net,      4);
                memcpy(packet + 8,  &total_net,    4);
                memcpy(packet + 12, &offset_net,   4);
                memcpy(packet + HEADER_SIZE, buffer + offset, chunk_size);

                /* Send with bounded retry.  juice_send() can fail transiently
                 * when the underlying UDP send buffer is momentarily full
                 * (EAGAIN/ENOBUFS from sendto) — e.g. during an ICE pair
                 * switch or a brief kernel tx queue overflow.  Previously we
                 * broke out on the first <0 return, which silently discarded
                 * the REST of the frame (often 200+ chunks of a 500 KB
                 * keyframe).  That single line caused most of the "some
                 * seconds of video then frozen" reports.  Now we retry with
                 * short backoff up to 5 attempts before giving up. */
                int ret = -1;
                for (int attempt = 0; attempt < 5; attempt++) {
                    pthread_mutex_lock(&svc->mutex);
                    if (svc->agent && svc->connected) {
                        ret = juice_send(svc->agent, (const char*)packet, chunk_size + HEADER_SIZE);
                    } else {
                        pthread_mutex_unlock(&svc->mutex);
                        ret = -1;
                        goto send_done;
                    }
                    pthread_mutex_unlock(&svc->mutex);
                    if (ret >= 0) break;
                    /* 500 µs, 1 ms, 2 ms, 4 ms — give the UDP tx queue a chance to drain */
                    usleep(500 << attempt);
                }
            send_done:
                if (ret < 0) {
                    /* Best-effort: drop THIS chunk but continue with the rest of
                     * the frame.  The consumer will detect the hole (via its
                     * chunk_received[] bitmap) and the frame will eventually
                     * time out OR the next offset==0 will start a fresh frame.
                     * Historically this ran `break`, which killed the whole
                     * frame on any transient send hiccup. */
                }
                offset += chunk_size;

                /* No per-chunk usleep.  The old 150µs pacing was rounded up by
                 * the Linux HRtimer to ~100–300 µs, which at 420 chunks/frame
                 * × 30 fps meant the provider could NOT keep up with the
                 * camera's output rate — TCP back-pressure filled the kernel
                 * rx buffer and the camera started dropping frames within a
                 * few seconds.  If bursts genuinely overrun the consumer's
                 * UDP rx buffer, the right fix is a larger rx buffer on the
                 * consumer (or a TURN-path-specific token bucket), not blind
                 * per-chunk sleeping. */
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
    pthread_mutex_lock(&svc->mutex);
    close_connection(conn);
    pthread_mutex_unlock(&svc->mutex);
    return NULL;
}

// ============================================================================
// ICE CALLBACKS (per service, dispatched via user_ptr)
// ============================================================================

static void on_recv(juice_agent_t *agent, const char *data, size_t size, void *user_ptr) {
    (void)agent;
    service_state_t* svc = (service_state_t*)user_ptr;
    if (!svc || size < HEADER_SIZE) return;

    uint32_t conn_id   = ntohl(*(uint32_t*)data);
    uint32_t seq_num   = ntohl(*(uint32_t*)(data + 4));
    uint32_t total_size = ntohl(*(uint32_t*)(data + 8));
    uint32_t offset    = ntohl(*(uint32_t*)(data + 12));
    size_t chunk_size  = size - HEADER_SIZE;

    // Handle heartbeats (conn_id == 0)
    // Consumer sends PING (seq=1); provider only responds with PONG (seq=2).
    // This mirrors the STUN Binding Request/Response pattern (RFC 5389).
    if (conn_id == 0 && total_size == 0 && offset == 0) {
        if (seq_num == 1) { // PING from consumer
            // Respond with PONG — provider never initiates keepalives
            uint8_t pong[HEADER_SIZE];
            memset(pong, 0, HEADER_SIZE);
            *(uint32_t*)(pong + 4) = htonl(2); // seq_num = 2 for PONG

            juice_agent_t *agent_snap = NULL;
            pthread_mutex_lock(&svc->mutex);
            if (svc->agent && svc->connected)
                agent_snap = svc->agent;
            pthread_mutex_unlock(&svc->mutex);

            if (agent_snap)
                juice_send(agent_snap, (const char*)pong, HEADER_SIZE);
        }
        return;
    }

    // Variables for deferred work outside the lock
    int need_connect = 0;
    uint8_t *send_buf = NULL;
    size_t send_size = 0;
    int send_fd = -1;
    int spare_consumed = 0;
    uint32_t saved_conn_id_for_refill = 0;

    pthread_mutex_lock(&svc->mutex);

    connection_t* conn = find_connection(svc, conn_id);

    if (offset == 0 && !conn) {
        conn = create_connection(svc, conn_id);
        if (!conn) {
            pthread_mutex_unlock(&svc->mutex);
            return;
        }
        need_connect = 1;
    }

    if (!conn) {
        pthread_mutex_unlock(&svc->mutex);
        return;
    }

    // If new connection, do TCP connect OUTSIDE the lock to avoid blocking all services
    if (need_connect) {
        uint32_t saved_conn_id = conn->conn_id;
        int local_port = svc->cfg.local_port;
        pthread_mutex_unlock(&svc->mutex);

        int local_socket = connect_to_local_service(local_port);

        pthread_mutex_lock(&svc->mutex);
        // Re-find connection (could have been cleaned up while unlocked)
        conn = find_connection(svc, saved_conn_id);
        if (!conn) {
            pthread_mutex_unlock(&svc->mutex);
            if (local_socket >= 0) close(local_socket);
            return;
        }

        if (local_socket < 0) {
            close_connection(conn);
            pthread_mutex_unlock(&svc->mutex);
            return;
        }

        conn->local_socket = local_socket;

        forward_arg_t* farg = malloc(sizeof(forward_arg_t));
        if (!farg) {
            close(local_socket);
            close_connection(conn);
            pthread_mutex_unlock(&svc->mutex);
            return;
        }
        farg->conn = conn;
        farg->svc = svc;
        pthread_create(&conn->forward_thread, NULL, forward_local_to_p2p, farg);
        pthread_detach(conn->forward_thread);
    }

    conn->last_activity  = time(NULL);
    conn->last_seq_num   = seq_num;

    // New frame: reset state when first chunk (offset==0) arrives
    if (offset == 0) {
        // Reject frames that are zero-length or exceed the reassembly buffer.
        // Prevents silent data corruption when sender sends an oversized frame.
        if (total_size == 0 || total_size > BUFFER_SIZE) {
            pthread_mutex_unlock(&svc->mutex);
            if (total_size > BUFFER_SIZE)
                printf("[!] [%s] Oversized frame dropped (size=%u > buffer=%u) — increase BUFFER_SIZE if legitimate\n",
                       svc->cfg.service_id, total_size, (uint32_t)BUFFER_SIZE);
            return;
        }
        memset(conn->chunk_received, 0, MAX_CHUNKS_PER_FRAME);
        conn->expected_size  = total_size;
        conn->bytes_received = 0;
    }

    // Guard: we must have seen offset=0 before accepting other chunks
    if (conn->expected_size == 0 || conn->reassembly_buffer == NULL) {
        pthread_mutex_unlock(&svc->mutex);
        return;
    }

    uint32_t chunk_index = offset / MAX_CHUNK_PAYLOAD;
    int frame_complete = 0;

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
            /* Zero-copy handoff: swap the full reassembly_buffer with the
               connection's pre-allocated spare. The 4 MB memcpy and 4 MB
               malloc that this replaces were the dominant source of
               tail-latency spikes in the provider's ICE thread — during
               them, outgoing stream chunks waiting on svc->mutex couldn't
               fire juice_send(), which visibly stuttered the feed. */
            if (conn->spare_buffer && conn->local_socket >= 0) {
                send_buf  = conn->reassembly_buffer;
                send_size = conn->expected_size;
                conn->reassembly_buffer = conn->spare_buffer;
                conn->spare_buffer = NULL;  /* replenished below, outside lock */
                send_fd = dup(conn->local_socket);
                spare_consumed = 1;
                saved_conn_id_for_refill = conn->conn_id;
            }
            conn->bytes_received = 0;
            conn->expected_size  = 0;
            frame_complete = 1;
        }
    }

    pthread_mutex_unlock(&svc->mutex);

    /* Replenish the consumed spare buffer OUTSIDE svc->mutex, so the next
       frame never waits on a 4 MB malloc inside the ICE callback. */
    if (spare_consumed) {
        uint8_t *new_spare = malloc(BUFFER_SIZE);
        if (new_spare) {
            pthread_mutex_lock(&svc->mutex);
            connection_t *c2 = find_connection(svc, saved_conn_id_for_refill);
            if (c2 && c2->active && c2->spare_buffer == NULL) {
                c2->spare_buffer = new_spare;
                new_spare = NULL;
            }
            pthread_mutex_unlock(&svc->mutex);
            free(new_spare);
        }
    }

    if (frame_complete && send_fd >= 0 && send_buf && send_size > 0) {
        /* On the ICE callback thread — use the non-blocking variant so libjuice
         * can resume reading the UDP socket immediately.  A partial/short write
         * here means we dropped the tail of a control frame, which the local
         * service will treat as a short/aborted request.  That is vastly
         * preferable to stalling the whole P2P stream for hundreds of ms. */
        ssize_t w = send_best_effort_nonblocking(send_fd, send_buf, send_size, MSG_NOSIGNAL);
        if (w < 0)
            printf("[!] Provider: local-service send error (fd=%d): %s\n",
                   send_fd, strerror(errno));
        else if ((size_t)w < send_size)
            printf("[!] Provider: local-service short write %zd/%zu (dropped — busy)\n",
                   w, send_size);
        close(send_fd);
    }
    free(send_buf);
}

static void on_state_changed(juice_agent_t *agent, juice_state_t juice_state, void *user_ptr) {
    service_state_t* svc = (service_state_t*)user_ptr;
    if (!svc) return;

    const char *state_names[] = {"DISCONNECTED","GATHERING","CONNECTING","CONNECTED","COMPLETED","FAILED"};
    const char *sn = (juice_state >= 0 && juice_state <= 5) ? state_names[juice_state] : "UNKNOWN";
    printf("[ICE] [%s] State -> %s\n", svc->cfg.service_id, sn);

    if (juice_state == JUICE_STATE_CONNECTED || juice_state == JUICE_STATE_COMPLETED) {
        pthread_mutex_lock(&svc->mutex);
        if (!svc->connected) {
            svc->connected = 1;
            svc->connection_start_time = time(NULL);
        }
        pthread_mutex_unlock(&svc->mutex);

        // Get and display selected candidate pair to show connection method
        char local_cand[256] = {0};
        char remote_cand[256] = {0};
        juice_get_selected_candidates(agent, local_cand, sizeof(local_cand), remote_cand, sizeof(remote_cand));

        int has_relay = (strstr(local_cand, " relay") || strstr(remote_cand, " relay")) ? 1 : 0;
        int has_srflx = (strstr(local_cand, " srflx") || strstr(remote_cand, " srflx")) ? 1 : 0;
        int has_prflx = (strstr(local_cand, " prflx") || strstr(remote_cand, " prflx")) ? 1 : 0;
        int has_host  = (strstr(local_cand, " host")  || strstr(remote_cand, " host"))  ? 1 : 0;

        const char *method = "UNKNOWN";
        const char *detail = "";
        if (has_relay) {
            method = "TURN RELAY";
            detail = " (traffic relayed through Coturn server)";
        } else if (has_host || has_prflx) {
            method = "HOST (Direct LAN)";
            detail = " (direct connection, no STUN/TURN needed — best possible)";
        } else if (has_srflx) {
            method = "STUN (Server Reflexive)";
            detail = " (direct P2P across internet, Coturn only discovered public IPs)";
        }

        printf("╔══════════════════════════════════════════════════════════╗\n");
        printf("║  [%s] P2P CONNECTED                                    \n", svc->cfg.service_id);
        printf("║  Connection Method: %s%s\n", method, detail);
        printf("║  Local  candidate: %s\n", local_cand[0] ? local_cand : "N/A");
        printf("║  Remote candidate: %s\n", remote_cand[0] ? remote_cand : "N/A");
        printf("╚══════════════════════════════════════════════════════════╝\n");
    } else if (juice_state == JUICE_STATE_FAILED) {
        printf("[X] [%s] ICE connection failed\n", svc->cfg.service_id);
        pthread_mutex_lock(&svc->mutex);
        svc->connected = 0;
        svc->needs_disconnect_notify = 1;  // Main loop will notify signaling server
        pthread_mutex_unlock(&svc->mutex);
    } else if (juice_state == JUICE_STATE_DISCONNECTED) {
        printf("[!] [%s] ICE connection disconnected\n", svc->cfg.service_id);
        pthread_mutex_lock(&svc->mutex);
        svc->connected = 0;
        pthread_mutex_unlock(&svc->mutex);
    }
}

static void on_gathering_done(juice_agent_t *agent, void *user_ptr) {
    (void)agent;
    service_state_t* svc = (service_state_t*)user_ptr;
    if (!svc) return;
    printf("[ICE] [%s] ===== All candidates gathered =====\n", svc->cfg.service_id);
    svc->gathering_done = 1;
}

static void on_candidate(juice_agent_t *agent, const char *sdp, void *user_ptr) {
    (void)agent;
    service_state_t* svc = (service_state_t*)user_ptr;
    if (!svc) return;

    // Detect candidate type from SDP line
    if (strstr(sdp, " host ")) {
        printf("[ICE] [%s] Candidate found -> HOST (direct LAN)\n", svc->cfg.service_id);
    } else if (strstr(sdp, " srflx ")) {
        printf("[ICE] [%s] Candidate found -> SRFLX (STUN - public IP discovered)\n", svc->cfg.service_id);
    } else if (strstr(sdp, " relay ")) {
        printf("[ICE] [%s] Candidate found -> RELAY (TURN - traffic will be relayed)\n", svc->cfg.service_id);
    } else if (strstr(sdp, " prflx ")) {
        printf("[ICE] [%s] Candidate found -> PRFLX (peer reflexive)\n", svc->cfg.service_id);
    } else {
        printf("[ICE] [%s] Candidate found -> %s\n", svc->cfg.service_id, sdp);
    }
}

// ============================================================================
// KEEPALIVE THREAD (single thread, iterates all services)
// ============================================================================
// Both peers (provider and consumer) send keepalive messages to maintain their
// respective NAT bindings. This is analogous to STUN Binding Requests (RFC 5389
// Section 10) — each side must independently refresh its own NAT mapping.
// The remote peer responds with PONG; it never initiates keepalives itself.

/* Provider-side keepalive removed.
 *
 * The previous implementation sent packets with header (conn_id=0, seq=0,
 * total_size=0, offset=0).  The consumer's on_recv_camera only updates its
 * liveness watchdog on PONG (seq=2), so seq=0 packets were silently dropped
 * by the receiver — the thread was a no-op for application liveness.
 *
 * NAT-mapping refresh on the provider→consumer direction is already handled
 * by:
 *   1. the constant flow of video chunks during an active session;
 *   2. libjuice's own STUN keepalive (Binding Indications every 15 s with
 *      DISABLE_CONSENT_FRESHNESS=ON) — these refresh the 5-tuple even when
 *      the camera momentarily produces no video;
 *   3. PONG responses to consumer-initiated PING (handled in on_recv).
 *
 * Removing the thread also eliminates a small race against juice_destroy()
 * during reconnect (the keepalive accessed svc->agent under svc->mutex but
 * the destroy path holds the same mutex differently).
 */

// ============================================================================
// CLEANUP THREAD (single thread, iterates all services)
// ============================================================================

static void* cleanup_thread(void* arg) {
    (void)arg;
    while (g_running) {
        sleep(10);
        time_t now = time(NULL);
        for (int s = 0; s < num_services; s++) {
            service_state_t* svc = &services[s];
            pthread_mutex_lock(&svc->mutex);
            for (int i = 0; i < MAX_CONNECTIONS; i++) {
                if (svc->connections[i].active) {
                    if (now - svc->connections[i].last_activity > REASSEMBLY_TIMEOUT) {
                        close_connection(&svc->connections[i]);
                    }
                }
            }
            pthread_mutex_unlock(&svc->mutex);
        }
    }
    return NULL;
}

// ============================================================================
// SERVICE HELPERS
// ============================================================================

static service_state_t* find_service_by_id(const char* service_id) {
    for (int s = 0; s < num_services; s++) {
        if (strcmp(services[s].cfg.service_id, service_id) == 0)
            return &services[s];
    }
    return NULL;
}

static void save_ice_credentials(service_state_t* svc, const char* sdp) {
    char* ufrag_pos = strstr(sdp, "a=ice-ufrag:");
    if (ufrag_pos) {
        ufrag_pos += strlen("a=ice-ufrag:");
        char* end = strpbrk(ufrag_pos, "\r\n");
        if (end) {
            int len = end - ufrag_pos;
            if (len < (int)sizeof(svc->saved_ufrag)) {
                strncpy(svc->saved_ufrag, ufrag_pos, len);
                svc->saved_ufrag[len] = '\0';
            }
        }
    }
    char* pwd_pos = strstr(sdp, "a=ice-pwd:");
    if (pwd_pos) {
        pwd_pos += strlen("a=ice-pwd:");
        char* end = strpbrk(pwd_pos, "\r\n");
        if (end) {
            int len = end - pwd_pos;
            if (len < (int)sizeof(svc->saved_pwd)) {
                strncpy(svc->saved_pwd, pwd_pos, len);
                svc->saved_pwd[len] = '\0';
            }
        }
    }
}

// Handle CONSUMER_CONNECT for a specific service.
// Returns 1 when ICE was set up successfully (caller should send SDP_READY).
// Returns 0 when the message was ignored (debounce / active session / error).
static int handle_consumer_connect(service_state_t* svc, const char* consumer_msg) {
    printf("\n[→] [%s] Consumer connection request\n", svc->cfg.service_id);

    // If we already have a healthy active session, ignore unsolicited
    // reconnect requests. Otherwise a duplicate CONSUMER_CONNECT would tear
    // down a good ICE session and can lead to reconnect loops.
    //
    // Optional override: include "force_reconnect=1" in signaling message.
    int force_reconnect = (strstr(consumer_msg, "force_reconnect=1") != NULL);
    if (svc->connected && !force_reconnect) {
        long uptime = (long)(time(NULL) - svc->connection_start_time);
        printf("[!] [%s] Ignoring CONSUMER_CONNECT while session is active (uptime=%lds). "
               "Set force_reconnect=1 to override.\n",
               svc->cfg.service_id, uptime);
        return 0;
    }

    // Debounce: if a CONSUMER_CONNECT already arrived recently and ICE is still
    // in progress, ignore the duplicate.  Without this, rapid-fire REQUESTs from
    // the consumer (e.g. STUN→TURN fallback, or reconnect storms) cause each new
    // CONSUMER_CONNECT to destroy the previous ICE negotiation before it finishes,
    // resulting in a permanent timeout loop.
    time_t now = time(NULL);
    if (svc->has_had_consumer && svc->last_consumer_connect_time > 0) {
        long since_last = (long)(now - svc->last_consumer_connect_time);
        if (since_last <= 15 && !svc->connected) {
            // ICE from the last CONSUMER_CONNECT is still in progress (<=15s)
            // and hasn't connected yet — let it finish.
            // Window is 15s (not 10s) to safely exceed the consumer's 10s backoff:
            // the old <10 check allowed a CONSUMER_CONNECT at exactly 10s through,
            // destroying live ICE negotiation at the worst possible moment.
            printf("[!] [%s] Ignoring duplicate CONSUMER_CONNECT (%lds since last, ICE still in progress)\n",
                   svc->cfg.service_id, since_last);
            return 0;
        }
    }
    svc->last_consumer_connect_time = now;

    // On reconnection (not first connect), clean up the previous session
    if (svc->has_had_consumer) {
        printf("[!] [%s] Cleaning up previous session...\n", svc->cfg.service_id);
        svc->connected = 0;

        pthread_mutex_lock(&svc->mutex);
        for (int i = 0; i < MAX_CONNECTIONS; i++) {
            if (svc->connections[i].active) {
                close_connection(&svc->connections[i]);
            }
        }
        /* Do NOT reset next_conn_id on reconnect — late chunks from the
         * previous session may still arrive with the same low conn_id
         * value, colliding with newly assigned connections.  uint32 wrap is
         * safe at any realistic connection rate.  */
        pthread_mutex_unlock(&svc->mutex);

        juice_destroy(svc->agent);
        svc->agent = NULL;
        usleep(100000);

        // Recreate ICE agent with same credentials (matches registered SDP)
        svc->agent = juice_create(&svc->juice_cfg);
        if (!svc->agent) {
            printf("[X] [%s] Failed to recreate ICE agent\n", svc->cfg.service_id);
            return 0;
        }
        juice_set_local_ice_attributes(svc->agent, svc->saved_ufrag, svc->saved_pwd);
        svc->gathering_done = 0;
        juice_gather_candidates(svc->agent);
        /* 50 ms × 60 = 3 s.  TURN/SRFLX candidates typically arrive within
         * 1 s; waiting 15 s on every reconnect was the largest single
         * contributor to "long freezes" perceived by viewers.  ICE will
         * still ingest late candidates after we send SDP_READY.  */
        for (int i = 0; i < 60 && !svc->gathering_done; i++) {
            usleep(50000);
        }
        if (!svc->gathering_done)
            printf("[!] [%s] Reconnect ICE gathering still pending at 3 s — proceeding with current candidates\n",
                   svc->cfg.service_id);
        printf("[✓] [%s] ICE agent recreated with same credentials\n", svc->cfg.service_id);
    }
    svc->has_had_consumer = 1;

    // Set consumer's remote SDP to start ICE negotiation
    char* sdp_start = strstr(consumer_msg, "consumer_sdp=");
    if (sdp_start) {
        sdp_start += strlen("consumer_sdp=");
        printf("[...] [%s] Setting remote SDP...\n", svc->cfg.service_id);
        char sanitized_sdp[MAX_SDP_LEN];
        int dropped = sanitize_remote_sdp(sdp_start, sanitized_sdp, sizeof(sanitized_sdp));
        if (dropped < 0) {
            printf("[X] [%s] Remote SDP too large after sanitization\n", svc->cfg.service_id);
            return 0;
        }
        if (dropped > 0) {
            printf("[!] [%s] Dropped %d unsupported ICE candidate(s) from remote SDP\n",
                   svc->cfg.service_id, dropped);
        }

        int ret = juice_set_remote_description(svc->agent, sanitized_sdp);
        if (ret < 0) {
            printf("[X] [%s] Failed to set remote SDP\n", svc->cfg.service_id);
            return 0;
        }
        juice_set_remote_gathering_done(svc->agent);
        printf("[✓] [%s] ICE negotiation in progress...\n", svc->cfg.service_id);
        return 1;  // ICE set up — caller should send SDP_READY to signaling server
    }
    return 0;
}

// ============================================================================
// SIGNAL HANDLING
// ============================================================================

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

// ============================================================================
// MAIN
// ============================================================================

int main(int argc, char* argv[]) {
    g_log_file = fopen("provider.log", "a");
    setvbuf(stdout, NULL, _IONBF, 0);

    // Ignore SIGPIPE so that broken TCP connections (remote end closed, log pipe
    // dies, etc.) return EPIPE from send/write instead of killing the process.
    signal(SIGPIPE, SIG_IGN);
    printf("\n╔════════════════════════════════════════════╗\n");
    printf("║  P2P Provider (Multi-Service - libjuice)  ║\n");
    printf("╚════════════════════════════════════════════╝\n\n");

    if (argc < 2) {
        printf("Usage: %s <config_file>\n", argv[0]);
        printf("   or: %s <signaling_server_ip> <config_file>  (legacy)\n", argv[0]);
        return 1;
    }

    const char* config_file;
    const char* signaling_server_arg = NULL;

    if (argc == 2) {
        config_file = argv[1];
    } else {
        signaling_server_arg = argv[1];
        config_file = argv[2];
    }

    // Load configuration
    printf("[...] Loading configuration from: %s\n", config_file);
    if (!load_config(config_file)) {
        printf("[X] Failed to load configuration\n");
        return 1;
    }

    if (num_services == 0) {
        printf("[X] No services defined in config file\n");
        return 1;
    }

    printf("[✓] Configuration loaded: %d service(s)\n\n", num_services);

    // Resolve signaling server
    const char* signaling_server;
    if (signaling_server_arg != NULL) {
        signaling_server = signaling_server_arg;
    } else if (g_config.signaling_server[0] != '\0') {
        signaling_server = g_config.signaling_server;
    } else {
        printf("[X] No signaling server specified.\n");
        return 1;
    }
    int signaling_port = g_config.signaling_port;

    printf("Signaling Server: %s:%d\n", signaling_server, signaling_port);
    printf("Services:\n");
    for (int s = 0; s < num_services; s++) {
        printf("  [%d] %s  (%s, port %d, p2p %d)\n", s + 1,
               services[s].cfg.service_id, services[s].cfg.protocol,
               services[s].cfg.local_port, services[s].cfg.p2p_port);
    }
    /* Provider-side keepalive removed; libjuice STUN binding indications
     * (every 15 s with DISABLE_CONSENT_FRESHNESS=ON) handle NAT refresh.  */


    /* Raise the open-file-descriptor soft limit to the hard maximum. Each
       completed frame dup()s the local service socket so it can close()
       independently of the main stream; at scale this can exceed the default
       1024-fd limit on many Linux distros. */
    {
        struct rlimit rl;
        if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
            rl.rlim_cur = rl.rlim_max;
            setrlimit(RLIMIT_NOFILE, &rl);
        }
    }

    juice_set_log_level(JUICE_LOG_LEVEL_WARN);

    // ========================================================================
    // Create ICE agent for each service
    // ========================================================================
    for (int s = 0; s < num_services; s++) {
        service_state_t* svc = &services[s];

        memset(&svc->juice_cfg, 0, sizeof(juice_config_t));
        svc->juice_cfg.bind_address = "0.0.0.0";
        svc->juice_cfg.local_port_range_begin = svc->cfg.p2p_port;
        // Avoid reusing the exact same UDP 5-tuple on rapid reconnects.
        // A tiny port range reduces TURN 437 (Allocation Mismatch) after session churn.
        svc->juice_cfg.local_port_range_end = svc->cfg.p2p_port + 10;
        svc->juice_cfg.cb_state_changed = on_state_changed;
        svc->juice_cfg.cb_recv = on_recv;
        svc->juice_cfg.cb_gathering_done = on_gathering_done;
        svc->juice_cfg.cb_candidate = on_candidate;
        svc->juice_cfg.user_ptr = svc;

        // STUN (RFC 5389) + TURN (RFC 5766) configuration
        // STUN discovers public IP via Binding Request/Response.
        // TURN extends STUN to relay traffic when direct P2P fails.
        // Both share port 3478 on the Coturn server.
        if (g_config.turn_server_enabled && g_config.turn_server_host[0] != '\0') {
            // Use Coturn as both STUN and TURN server
            svc->juice_cfg.stun_server_host = g_config.turn_server_host;
            svc->juice_cfg.stun_server_port = g_config.turn_server_port;

            memset(&g_turn_server, 0, sizeof(g_turn_server));
            g_turn_server.host = g_config.turn_server_host;
            g_turn_server.port = g_config.turn_server_port;
            if (g_config.turn_server_username[0] != '\0')
                g_turn_server.username = g_config.turn_server_username;
            if (g_config.turn_server_password[0] != '\0')
                g_turn_server.password = g_config.turn_server_password;
            svc->juice_cfg.turn_servers = &g_turn_server;
            svc->juice_cfg.turn_servers_count = 1;

            printf("[✓] [%s] Using Coturn STUN+TURN %s:%d\n",
                   svc->cfg.service_id, g_config.turn_server_host, g_config.turn_server_port);
        } else {
            // Fallback: no TURN configured, use Google public STUN
            svc->juice_cfg.stun_server_host = "74.125.250.129";
            svc->juice_cfg.stun_server_port = 19302;
            printf("[!] [%s] No TURN configured, using Google STUN only\n", svc->cfg.service_id);
        }

        svc->agent = juice_create(&svc->juice_cfg);
        if (!svc->agent) {
            printf("[X] [%s] Failed to create ICE agent\n", svc->cfg.service_id);
            return 1;
        }
        printf("[✓] [%s] ICE agent created\n", svc->cfg.service_id);

        // Gather candidates — wait for on_gathering_done so the registered SDP
        // contains ALL candidates (HOST + SRFLX + RELAY).
        svc->gathering_done = 0;
        juice_gather_candidates(svc->agent);
        for (int i = 0; i < 300 && !svc->gathering_done; i++) {
            usleep(50000);   // 50 ms × 300 = 15 s max (allows TURN relay candidate to arrive)
        }
        if (!svc->gathering_done)
            printf("[!] [%s] ICE gathering timed out — proceeding with partial candidates\n",
                   svc->cfg.service_id);

        printf("[✓] [%s] ICE candidates ready\n", svc->cfg.service_id);
    }

    // Register signal handlers for graceful shutdown (once, outside reconnect loop).
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // Start background threads (once, outside reconnect loop).
    pthread_t cleanup_tid;
    pthread_create(&cleanup_tid, NULL, cleanup_thread, NULL);
    pthread_detach(cleanup_tid);

    // ========================================================================
    // Signaling reconnect loop: reconnects automatically if the server drops.
    // P2P ICE connections between provider and consumer continue independently
    // of the signaling TCP connection — only new consumer connections need it.
    // ========================================================================
    int reconnect_delay = 5;
    while (g_running) {
        // ── Connect ─────────────────────────────────────────────────────────
        printf("[...] Connecting to signaling server %s:%d...\n",
               signaling_server, signaling_port);

        sig_socket = socket(AF_INET, SOCK_STREAM, 0);
        if (sig_socket < 0) {
            printf("[X] Failed to create signaling socket: %s\n", strerror(errno));
            break;  // unrecoverable
        }

        struct sockaddr_in sig_addr;
        memset(&sig_addr, 0, sizeof(sig_addr));
        sig_addr.sin_family = AF_INET;
        sig_addr.sin_port = htons(signaling_port);
        sig_addr.sin_addr.s_addr = inet_addr(signaling_server);

        if (connect(sig_socket, (struct sockaddr*)&sig_addr, sizeof(sig_addr)) < 0) {
            printf("[!] Failed to connect to signaling server — retrying in %ds\n",
                   reconnect_delay);
            close(sig_socket);
            sig_socket = -1;
            for (int w = 0; w < reconnect_delay && g_running; w++) sleep(1);
            if (reconnect_delay < 60) reconnect_delay = (reconnect_delay * 2 < 60) ? reconnect_delay * 2 : 60;
            continue;
        }
        reconnect_delay = 5;  // reset on success

        // TCP_NODELAY: small signaling messages must not wait in Nagle buffer.
        { int v = 1; setsockopt(sig_socket, IPPROTO_TCP, TCP_NODELAY, &v, sizeof(v)); }

        printf("[✓] Connected to signaling server\n");

        // ── Register each service ────────────────────────────────────────────
        int reg_failed = 0;
        for (int s = 0; s < num_services && !reg_failed; s++) {
            service_state_t* svc = &services[s];

            char local_sdp[MAX_SDP_LEN];
            if (juice_get_local_description(svc->agent, local_sdp, MAX_SDP_LEN) < 0) {
                printf("[X] [%s] Failed to get SDP\n", svc->cfg.service_id);
                reg_failed = 1;
                break;
            }

            save_ice_credentials(svc, local_sdp);

            char register_msg[MAX_SDP_LEN + 512];
            if (g_config.api_token[0] != '\0') {
                snprintf(register_msg, sizeof(register_msg),
                         "type=REGISTER\nservice_id=%s\npeer_id=provider1\np2p_port=%d"
                         "\nprotocol=%s\ncodec=%s\nconsumer_port=%d\napi_token=%s\nsdp=%s",
                         svc->cfg.service_id, svc->cfg.p2p_port, svc->cfg.protocol,
                         svc->cfg.codec[0] ? svc->cfg.codec : "h264",
                         svc->cfg.consumer_port, g_config.api_token, local_sdp);
            } else {
                snprintf(register_msg, sizeof(register_msg),
                         "type=REGISTER\nservice_id=%s\npeer_id=provider1\np2p_port=%d"
                         "\nprotocol=%s\ncodec=%s\nconsumer_port=%d\nsdp=%s",
                         svc->cfg.service_id, svc->cfg.p2p_port, svc->cfg.protocol,
                         svc->cfg.codec[0] ? svc->cfg.codec : "h264",
                         svc->cfg.consumer_port, local_sdp);
            }

            if (!send_message(sig_socket, register_msg)) {
                printf("[X] [%s] Failed to send REGISTER\n", svc->cfg.service_id);
                reg_failed = 1;
                break;
            }

            char response[1024];
            if (!recv_message(sig_socket, response, sizeof(response))) {
                printf("[X] [%s] Failed to receive REGISTERED response\n", svc->cfg.service_id);
                reg_failed = 1;
                break;
            }

            if (strstr(response, "type=REGISTERED")) {
                printf("[✓] [%s] Registered (%s on port %d)\n",
                       svc->cfg.service_id, svc->cfg.protocol, svc->cfg.local_port);
            } else if (strstr(response, "type=ERROR")) {
                printf("[X] [%s] Registration rejected by server: %s\n",
                       svc->cfg.service_id, response);
                reg_failed = 1;
                break;
            } else {
                printf("[!] [%s] Unexpected registration response\n", svc->cfg.service_id);
            }
        }

        if (reg_failed) {
            close(sig_socket);
            sig_socket = -1;
            printf("[!] Registration failed — retrying in %ds\n", reconnect_delay);
            for (int w = 0; w < reconnect_delay && g_running; w++) sleep(1);
            if (reconnect_delay < 60) reconnect_delay = (reconnect_delay * 2 < 60) ? reconnect_delay * 2 : 60;
            continue;
        }

        printf("\n════════════════════════════════════════════\n");
        printf(" Provider ready — %d service(s) registered\n", num_services);
        printf("════════════════════════════════════════════\n\n");

        // Set recv timeout so the loop can check g_running without blocking forever.
        {
            struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
            setsockopt(sig_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        }

        // ── Main message loop ────────────────────────────────────────────────
        while (g_running) {
            char consumer_msg[MAX_SDP_LEN + 512];
            if (!recv_message(sig_socket, consumer_msg, sizeof(consumer_msg))) {
                if (!g_running) break;
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // Idle tick — flush any pending disconnect notifications.
                    for (int s = 0; s < num_services; s++) {
                        if (services[s].needs_disconnect_notify) {
                            services[s].needs_disconnect_notify = 0;
                            char disc_msg[256];
                            snprintf(disc_msg, sizeof(disc_msg),
                                     "type=PROVIDER_DISCONNECTED\nservice_id=%.127s",
                                     services[s].cfg.service_id);
                            if (send_message(sig_socket, disc_msg)) {
                                printf("[!] [%s] Notified signaling server: ICE connection lost\n",
                                       services[s].cfg.service_id);
                            }
                        }
                    }
                    continue;
                }
                printf("[X] Signaling server connection lost — will reconnect\n");
                break;
            }

            if (strstr(consumer_msg, "type=PING")) {
                char pong_msg[256];
                char* sid_start = strstr(consumer_msg, "service_id=");
                if (sid_start) {
                    char sid[64] = "";
                    sscanf(sid_start, "service_id=%63s", sid);
                    snprintf(pong_msg, sizeof(pong_msg), "type=PONG\nservice_id=%s", sid);
                } else {
                    snprintf(pong_msg, sizeof(pong_msg), "type=PONG\nservice_id=%s",
                             num_services > 0 ? services[0].cfg.service_id : "unknown");
                }
                send_message(sig_socket, pong_msg);
                continue;
            }

            if (!strstr(consumer_msg, "type=CONSUMER_CONNECT")) continue;

            // Extract service_id from message.
            char target_id[128] = "";
            char* sid_start = strstr(consumer_msg, "\nservice_id=");
            if (sid_start) {
                sid_start += strlen("\nservice_id=");
            } else if (strncmp(consumer_msg, "service_id=", 11) == 0) {
                sid_start = consumer_msg + 11;
            }
            if (sid_start) {
                char* sid_end = strpbrk(sid_start, "\r\n");
                int len = sid_end ? (int)(sid_end - sid_start) : (int)strlen(sid_start);
                if (len > 0 && len < (int)sizeof(target_id)) {
                    strncpy(target_id, sid_start, len);
                    target_id[len] = '\0';
                }
            }

            service_state_t* svc = NULL;
            if (target_id[0] != '\0') svc = find_service_by_id(target_id);
            if (!svc && num_services == 1) svc = &services[0];
            if (!svc) {
                printf("[X] Unknown service_id in CONSUMER_CONNECT: '%s'\n", target_id);
                continue;
            }

            int ice_ready = handle_consumer_connect(svc, consumer_msg);

            // Send fresh SDP to signaling server so consumers get current ICE candidates.
            if (ice_ready > 0 && svc->agent) {
                char fresh_sdp[MAX_SDP_LEN];
                if (juice_get_local_description(svc->agent, fresh_sdp, MAX_SDP_LEN) >= 0) {
                    char sdp_ready_msg[MAX_SDP_LEN + 256];
                    snprintf(sdp_ready_msg, sizeof(sdp_ready_msg),
                             "type=SDP_READY\nservice_id=%s\nsdp=%s",
                             svc->cfg.service_id, fresh_sdp);
                    if (send_message(sig_socket, sdp_ready_msg)) {
                        printf("[✓] [%s] SDP_READY sent to signaling server\n",
                               svc->cfg.service_id);
                    } else {
                        printf("[!] [%s] Failed to send SDP_READY — connection may be lost\n",
                               svc->cfg.service_id);
                    }
                }
            }

            // Flush any pending ICE-failed notifications.
            for (int s = 0; s < num_services; s++) {
                if (services[s].needs_disconnect_notify) {
                    services[s].needs_disconnect_notify = 0;
                    char disc_msg[256];
                    snprintf(disc_msg, sizeof(disc_msg),
                             "type=PROVIDER_DISCONNECTED\nservice_id=%.127s",
                             services[s].cfg.service_id);
                    if (send_message(sig_socket, disc_msg)) {
                        printf("[!] [%s] Notified signaling server: ICE connection lost\n",
                               services[s].cfg.service_id);
                    }
                }
            }
        }  // inner main loop

        close(sig_socket);
        sig_socket = -1;

        if (!g_running) break;

        printf("[!] Reconnecting to signaling server in %ds...\n", reconnect_delay);
        for (int w = 0; w < reconnect_delay && g_running; w++) sleep(1);
        if (reconnect_delay < 60) reconnect_delay = (reconnect_delay * 2 < 60) ? reconnect_delay * 2 : 60;
    }  // outer reconnect loop

    // Cleanup
    g_running = 0;
    for (int s = 0; s < num_services; s++) {
        services[s].keepalive_enabled = 0;
        if (services[s].agent) juice_destroy(services[s].agent);
    }
    if (sig_socket >= 0) close(sig_socket);
    return 0;
}
