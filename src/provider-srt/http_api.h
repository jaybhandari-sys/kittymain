// HTTP API Server for VMS Consumer
// This file contains the HTTP server implementation to be included in vms_consumer_api.c

#ifndef HTTP_API_H
#define HTTP_API_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ctype.h>

#define API_PORT 3000
#define HTTP_BUFFER_SIZE 8192
#define MAX_REQUEST_HANDLERS 10

// HTTP request structure
typedef struct {
    char method[16];        // GET, POST, etc.
    char path[256];         // /api/cameras/cam001/connect
    char body[4096];        // Request body
    char api_token[128];    // Value of X-API-Token header (empty if not present)
    int content_length;
} http_request_t;

// HTTP response builder
typedef struct {
    char buffer[HTTP_BUFFER_SIZE];
    int length;
} http_response_t;

// Global API server state
static int api_server_socket = -1;
static pthread_t api_server_thread_id;

// ============================================================================
// JSON BUILDERS
//
// All builders take a caller-supplied buffer instead of returning a static one.
// This makes every builder thread-safe (concurrent HTTP worker threads cannot
// clobber each other's output) and eliminates the strcat() buffer overflow
// that would crash the server once more than ~14 cameras were active.
// ============================================================================

// Build JSON for camera info. Returns `out` for chaining.
static char* build_camera_json(camera_session_t* cam, char* out, size_t out_size) {
    time_t uptime = cam->connected ? (time(NULL) - cam->connection_start_time) : 0;
    const char* proto = (cam->service_protocol[0] != '\0') ? cam->service_protocol : "rtsp";
    const char* codec = (cam->service_codec[0]    != '\0') ? cam->service_codec    : "h264";

    snprintf(out, out_size,
             "{"
             "\"camera_id\":\"%s\","
             "\"local_port\":%d,"
             "\"stream_url\":\"%s://localhost:%d/\","
             "\"protocol\":\"%s\","
             "\"codec\":\"%s\","
             "\"status\":\"%s\","
             "\"uptime\":%ld"
             "}",
             cam->camera_id,
             cam->local_listen_port,
             proto,
             cam->local_listen_port,
             proto,
             codec,
             cam->connected ? "connected" : "connecting",
             uptime);

    return out;
}

// Build JSON for camera list. Allocates a heap buffer sized to MAX_CAMERAS
// (caller must free()). Uses incremental snprintf with a tracked offset so a
// single active slot of oversized data can never overflow the buffer.
static char* build_camera_list_json() {
    /* Per-camera JSON is ~512 bytes; reserve a generous per-slot budget plus
       a fixed envelope so growth in fields doesn't silently start truncating. */
    const size_t PER_CAMERA = 768;
    const size_t ENVELOPE   = 64;
    size_t total = (size_t)MAX_CAMERAS * PER_CAMERA + ENVELOPE;
    char* json = (char*)malloc(total);
    if (!json) return NULL;

    size_t off = 0;
    int n = snprintf(json + off, total - off, "{\"cameras\":[");
    if (n < 0 || (size_t)n >= total - off) { free(json); return NULL; }
    off += (size_t)n;

    int count = 0;
    for (int i = 0; i < MAX_CAMERAS; i++) {
        if (!vms_state.cameras[i].active) continue;

        time_t uptime = vms_state.cameras[i].connected ?
                       (time(NULL) - vms_state.cameras[i].connection_start_time) : 0;

        const char* cam_proto = (vms_state.cameras[i].service_protocol[0] != '\0')
                                ? vms_state.cameras[i].service_protocol : "rtsp";
        const char* cam_codec = (vms_state.cameras[i].service_codec[0] != '\0')
                                ? vms_state.cameras[i].service_codec    : "h264";

        n = snprintf(json + off, total - off,
                 "%s{"
                 "\"camera_id\":\"%s\","
                 "\"local_port\":%d,"
                 "\"stream_url\":\"%s://localhost:%d/\","
                 "\"protocol\":\"%s\","
                 "\"codec\":\"%s\","
                 "\"status\":\"%s\","
                 "\"uptime\":%ld"
                 "}",
                 count > 0 ? "," : "",
                 vms_state.cameras[i].camera_id,
                 vms_state.cameras[i].local_listen_port,
                 cam_proto,
                 vms_state.cameras[i].local_listen_port,
                 cam_proto,
                 cam_codec,
                 vms_state.cameras[i].connected ? "connected" : "connecting",
                 uptime);
        if (n < 0 || (size_t)n >= total - off) break;  // safety: don't advance on truncation
        off += (size_t)n;
        count++;
    }

    n = snprintf(json + off, total - off, "],\"total\":%d}", count);
    if (n > 0 && (size_t)n < total - off) off += (size_t)n;

    return json;  // caller frees
}

// Build JSON error response into caller-supplied buffer.
static char* build_error_json(char* out, size_t out_size, const char* error) {
    snprintf(out, out_size, "{\"success\":false,\"error\":\"%s\"}", error);
    return out;
}

// Build JSON success response into caller-supplied buffer.
static char* build_success_json(char* out, size_t out_size, const char* message)
    __attribute__((unused));
static char* build_success_json(char* out, size_t out_size, const char* message) {
    snprintf(out, out_size, "{\"success\":true,\"message\":\"%s\"}", message);
    return out;
}

// ============================================================================
// HTTP UTILITIES
// ============================================================================

// Send HTTP response
static void send_http_response(int client_socket, int status_code, const char* status_text, const char* json) {
    char response[HTTP_BUFFER_SIZE];
    int json_len = strlen(json);

    // CORS: only emit the header when CORS_ORIGIN is explicitly configured.
    // Defaulting to "*" allows any webpage to read camera stream URLs, which is
    // a security risk.  Set CORS_ORIGIN in consumer.conf to enable cross-origin
    // access from a specific trusted dashboard domain.
    char cors_header[320] = "";
    if (config.cors_origin[0] != '\0') {
        snprintf(cors_header, sizeof(cors_header),
                 "Access-Control-Allow-Origin: %s\r\n", config.cors_origin);
    }

    int header_len = snprintf(response, sizeof(response),
                             "HTTP/1.1 %d %s\r\n"
                             "Content-Type: application/json\r\n"
                             "Content-Length: %d\r\n"
                             "%s"
                             "Connection: close\r\n"
                             "\r\n",
                             status_code, status_text, json_len, cors_header);

    // Send header
    send(client_socket, response, header_len, MSG_NOSIGNAL);

    // Send body
    send(client_socket, json, json_len, MSG_NOSIGNAL);
}

// Parse HTTP request
static int parse_http_request(const char* buffer, http_request_t* req) {
    memset(req, 0, sizeof(http_request_t));

    // Parse request line: METHOD /path HTTP/1.1
    const char* line_end = strstr(buffer, "\r\n");
    if (!line_end) return 0;

    char request_line[512];
    size_t line_len = (size_t)(line_end - buffer);
    if (line_len >= sizeof(request_line)) return 0;

    memcpy(request_line, buffer, line_len);
    request_line[line_len] = '\0';

    // Extract method and path
    char* space1 = strchr(request_line, ' ');
    if (!space1) return 0;
    *space1 = '\0';
    /* Reject method that exceeds field size (snprintf truncates safely). */
    if (strlen(request_line) >= sizeof(req->method)) return 0;
    snprintf(req->method, sizeof(req->method), "%s", request_line);

    char* space2 = strchr(space1 + 1, ' ');
    if (!space2) return 0;
    *space2 = '\0';
    if (strlen(space1 + 1) >= sizeof(req->path)) return 0;
    snprintf(req->path, sizeof(req->path), "%s", space1 + 1);

    // Find Content-Length header and validate it.
    const char* content_length_header = strstr(buffer, "Content-Length:");
    if (content_length_header) {
        int cl = atoi(content_length_header + 15);
        if (cl < 0) cl = 0;
        if (cl > (int)(sizeof(req->body) - 1)) cl = (int)(sizeof(req->body) - 1);
        req->content_length = cl;
    }

    // Find X-API-Token header
    const char* token_header = strstr(buffer, "X-API-Token:");
    if (token_header) {
        token_header += 12;
        while (*token_header == ' ') token_header++;
        const char* token_eol = strpbrk(token_header, "\r\n");
        size_t token_len = token_eol ? (size_t)(token_eol - token_header) : strlen(token_header);
        if (token_len > 0 && token_len < sizeof(req->api_token)) {
            memcpy(req->api_token, token_header, token_len);
            req->api_token[token_len] = '\0';
        }
    }

    // Find body (after \r\n\r\n)
    const char* body_start = strstr(buffer, "\r\n\r\n");
    if (body_start && req->content_length > 0) {
        body_start += 4;
        size_t body_len = (size_t)req->content_length;
        if (body_len >= sizeof(req->body)) body_len = sizeof(req->body) - 1;
        memcpy(req->body, body_start, body_len);
        req->body[body_len] = '\0';
    }

    return 1;
}

// Extract camera ID from path like "/api/cameras/cam001/connect" into a
// caller-supplied buffer. Returns 1 on success, 0 otherwise. Thread-safe
// (no shared static buffer).
static int extract_camera_id(const char* path, char* out, size_t out_size) {
    const char* prefix = "/api/cameras/";
    if (strncmp(path, prefix, strlen(prefix)) != 0) return 0;

    const char* id_start = path + strlen(prefix);
    const char* id_end   = strchr(id_start, '/');
    size_t id_len = id_end ? (size_t)(id_end - id_start) : strlen(id_start);

    if (id_len == 0 || id_len >= out_size) return 0;
    memcpy(out, id_start, id_len);
    out[id_len] = '\0';
    return 1;
}

// ============================================================================
// API ENDPOINT HANDLERS
// ============================================================================

// Handle: POST /api/cameras/:id/connect
static void handle_connect_camera(int client_socket, const char* camera_id) {
    printf("[API] Connect request for camera: %s\n", camera_id);
    char err[512];
    char json_buf[1024];

    // Check if already connected OR reconnect in progress
    camera_session_t* existing = find_camera_session(camera_id);
    if (existing) {
        if (existing->connected) {
            send_http_response(client_socket, 200, "OK",
                               build_camera_json(existing, json_buf, sizeof(json_buf)));
            return;
        }
        // Session exists but disconnected — auto-reconnect monitor may be active.
        // If a reconnect is scheduled or an ICE agent exists, don't interfere:
        // calling connect_to_camera() would destroy the session (including any
        // in-flight ICE negotiation) and send a duplicate REQUEST.
        if (existing->needs_reconnect || existing->agent != NULL) {
            printf("[API] Camera %s: reconnect in progress — returning existing listen port %d\n",
                   camera_id, existing->local_listen_port);
            snprintf(json_buf, sizeof(json_buf),
                     "{\"success\":true,\"camera_id\":\"%s\",\"local_port\":%d,"
                     "\"status\":\"reconnecting\",\"stream_url\":\"rtsp://localhost:%d/\"}",
                     camera_id, existing->local_listen_port, existing->local_listen_port);
            send_http_response(client_socket, 200, "OK", json_buf);
            return;
        }
    }

    // No session or session is truly dead — create fresh connection
    int port = connect_to_camera(camera_id);

    if (port > 0) {
        // Wait a bit for connection to establish
        sleep(2);

        camera_session_t* cam = find_camera_session(camera_id);
        if (cam) {
            const char* conn_proto = (cam->service_protocol[0] != '\0') ? cam->service_protocol : "rtsp";
            const char* conn_codec = (cam->service_codec[0]    != '\0') ? cam->service_codec    : "h264";
            snprintf(json_buf, sizeof(json_buf),
                     "{\"success\":true,\"camera_id\":\"%s\",\"local_port\":%d,\"protocol\":\"%s\",\"codec\":\"%s\",\"stream_url\":\"%s://localhost:%d/\"}",
                     camera_id, port, conn_proto, conn_codec, conn_proto, port);
            send_http_response(client_socket, 200, "OK", json_buf);
        } else {
            send_http_response(client_socket, 500, "Internal Server Error",
                             build_error_json(err, sizeof(err), "Failed to create camera session"));
        }
    } else {
        send_http_response(client_socket, 404, "Not Found",
                         build_error_json(err, sizeof(err), "Camera not found or connection failed"));
    }
}

// Handle: POST /api/cameras/:id/disconnect
static void handle_disconnect_camera(int client_socket, const char* camera_id) {
    printf("[API] Disconnect request for camera: %s\n", camera_id);
    char err[512];

    camera_session_t* cam = find_camera_session(camera_id);
    if (cam) {
        free_camera_session(cam);
        char response_json[256];
        snprintf(response_json, sizeof(response_json),
                 "{\"success\":true,\"camera_id\":\"%s\",\"status\":\"disconnected\"}",
                 camera_id);
        send_http_response(client_socket, 200, "OK", response_json);
    } else {
        send_http_response(client_socket, 404, "Not Found",
                         build_error_json(err, sizeof(err), "Camera not found"));
    }
}

// Handle: GET /api/cameras
static void handle_list_cameras(int client_socket) {
    printf("[API] List cameras request\n");
    char err[512];
    char* json = build_camera_list_json();
    if (!json) {
        send_http_response(client_socket, 500, "Internal Server Error",
                           build_error_json(err, sizeof(err), "Out of memory"));
        return;
    }
    send_http_response(client_socket, 200, "OK", json);
    free(json);
}

// Handle: GET /api/cameras/:id
static void handle_get_camera_status(int client_socket, const char* camera_id) {
    printf("[API] Status request for camera: %s\n", camera_id);
    char err[512];
    char json_buf[1024];

    camera_session_t* cam = find_camera_session(camera_id);
    if (cam) {
        send_http_response(client_socket, 200, "OK",
                           build_camera_json(cam, json_buf, sizeof(json_buf)));
    } else {
        send_http_response(client_socket, 404, "Not Found",
                         build_error_json(err, sizeof(err), "Camera not found"));
    }
}

// Handle: GET /health
static void handle_health_check(int client_socket) {
    char json[512];
    snprintf(json, sizeof(json),
             "{\"status\":\"ok\",\"cameras_connected\":%d}",
             vms_state.num_active_cameras);
    send_http_response(client_socket, 200, "OK", json);
}

// ============================================================================
// HTTP REQUEST ROUTER
// ============================================================================

static void* handle_http_request_thread(void* arg) {
    int client_socket = *(int*)arg;
    free(arg);
    
    char buffer[HTTP_BUFFER_SIZE];
    int bytes_read = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
    
    if (bytes_read <= 0) {
        close(client_socket);
        return NULL;
    }
    
    buffer[bytes_read] = '\0';

    http_request_t req;
    char err[512];
    if (!parse_http_request(buffer, &req)) {
        send_http_response(client_socket, 400, "Bad Request",
                         build_error_json(err, sizeof(err), "Invalid HTTP request"));
        close(client_socket);
        return NULL;
    }

    printf("[API] %s %s\n", req.method, req.path);

    // API token authentication.
    // /health is exempt so load-balancers and monitoring tools can probe without
    // credentials.  All other endpoints require a valid X-API-Token header when
    // API_TOKEN is set in consumer.conf.
    int is_health = (strcmp(req.path, "/health") == 0);
    if (!is_health && config.api_token[0] != '\0') {
        if (strcmp(req.api_token, config.api_token) != 0) {
            printf("[API] 401 Unauthorized — invalid or missing X-API-Token for %s %s\n",
                   req.method, req.path);
            send_http_response(client_socket, 401, "Unauthorized",
                               "{\"success\":false,\"error\":\"Invalid or missing X-API-Token\"}");
            close(client_socket);
            return NULL;
        }
    }

    // Route requests
    if (is_health && strcmp(req.method, "GET") == 0) {
        handle_health_check(client_socket);
    }
    else if (strcmp(req.path, "/api/cameras") == 0 && strcmp(req.method, "GET") == 0) {
        handle_list_cameras(client_socket);
    }
    else if (strstr(req.path, "/api/cameras/") == req.path) {
        char camera_id[128];
        if (!extract_camera_id(req.path, camera_id, sizeof(camera_id))) {
            send_http_response(client_socket, 400, "Bad Request",
                             build_error_json(err, sizeof(err), "Invalid camera ID"));
        }
        else if (strstr(req.path, "/connect") && strcmp(req.method, "POST") == 0) {
            handle_connect_camera(client_socket, camera_id);
        }
        else if (strstr(req.path, "/disconnect") && strcmp(req.method, "POST") == 0) {
            handle_disconnect_camera(client_socket, camera_id);
        }
        else if (strcmp(req.method, "GET") == 0) {
            handle_get_camera_status(client_socket, camera_id);
        }
        else {
            send_http_response(client_socket, 404, "Not Found",
                             build_error_json(err, sizeof(err), "Endpoint not found"));
        }
    }
    else {
        send_http_response(client_socket, 404, "Not Found",
                         build_error_json(err, sizeof(err), "Endpoint not found"));
    }

    close(client_socket);
    return NULL;
}

// ============================================================================
// HTTP SERVER THREAD
// ============================================================================

static void* http_server_thread(void* arg) {
    (void)arg;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    
    // Create socket
    api_server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (api_server_socket < 0) {
        printf("[X] Failed to create API server socket\n");
        return NULL;
    }
    
    // Set socket options
    int opt = 1;
    setsockopt(api_server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    // Bind socket
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(API_PORT);
    
    if (bind(api_server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        printf("[X] Failed to bind API server socket on port %d\n", API_PORT);
        close(api_server_socket);
        return NULL;
    }
    
    // Listen
    if (listen(api_server_socket, 10) < 0) {
        printf("[X] Failed to listen on API server socket\n");
        close(api_server_socket);
        return NULL;
    }
    
    printf("[✓] API Server listening on http://0.0.0.0:%d\n", API_PORT);
    printf("[✓] API Endpoints:\n");
    printf("    POST /api/cameras/:id/connect\n");
    printf("    POST /api/cameras/:id/disconnect\n");
    printf("    GET  /api/cameras\n");
    printf("    GET  /api/cameras/:id\n");
    printf("    GET  /health\n\n");
    
    // Accept connections
    while (vms_state.running) {
        int* client_socket_ptr = malloc(sizeof(int));
        *client_socket_ptr = accept(api_server_socket, (struct sockaddr*)&client_addr, &client_len);
        
        if (*client_socket_ptr < 0) {
            free(client_socket_ptr);
            if (!vms_state.running) break;
            continue;
        }
        
        // Handle request in new thread
        pthread_t request_thread;
        pthread_create(&request_thread, NULL, handle_http_request_thread, client_socket_ptr);
        pthread_detach(request_thread);
    }
    
    close(api_server_socket);
    return NULL;
}

// Start HTTP API server
static int start_http_api_server() {
    if (pthread_create(&api_server_thread_id, NULL, http_server_thread, NULL) != 0) {
        printf("[X] Failed to create API server thread\n");
        return 0;
    }
    
    pthread_detach(api_server_thread_id);
    return 1;
}

#endif // HTTP_API_H
