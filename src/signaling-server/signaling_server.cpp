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
//   - Default port: 3478 (shared with STUN)
//
// ICE (RFC 8445 — Interactive Connectivity Establishment):
//   - Orchestrates STUN/TURN to find the best connectivity path
//   - Candidate priority: HOST > SRFLX > PRFLX > RELAY
//
// NOTE: This signaling server operates over TCP (application-level protocol).
// The PING/PONG here is a TCP heartbeat for provider liveness detection,
// NOT STUN Binding Requests. STUN keepalives happen at the P2P/ICE layer
// between provider and consumer, handled by libjuice automatically.
// ============================================================================

#include <iostream>
#include <map>
#include <set>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <chrono>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <vector>
#include <sstream>
#include <fstream>
#include <iomanip>
#include <ctime>
#include <atomic>
#include <csignal>

// TeeStream to mirror output to a file
class TeeStream : public std::streambuf {
public:
    TeeStream(std::streambuf* sb1, std::streambuf* sb2) : sb1(sb1), sb2(sb2) {}
protected:
    virtual int overflow(int c) override {
        if (c == EOF) return !EOF;
        int r1 = sb1->sputc(c);
        int r2 = sb2->sputc(c);
        return (r1 == EOF || r2 == EOF) ? EOF : c;
    }
    virtual int sync() override {
        int r1 = sb1->pubsync();
        int r2 = sb2->pubsync();
        return (r1 == -1 || r2 == -1) ? -1 : 0;
    }
private:
    std::streambuf *sb1, *sb2;
};

std::ofstream log_file;
TeeStream* tee = nullptr;
std::streambuf* old_cout_buf = nullptr;

#define CONFIG_LINE_MAX 256
#define PING_INTERVAL 10       /* seconds between TCP heartbeats to providers */
#define MAX_MISSED_PONGS 3     /* 3 × 10s = 30s to detect dead provider */

// Resource monitoring structure
struct ResourceMetrics {
    std::atomic<double> cpu_usage{0.0};
    std::atomic<size_t> memory_usage_kb{0};
    std::atomic<size_t> network_rx_bytes{0};
    std::atomic<size_t> network_tx_bytes{0};
    std::atomic<size_t> network_rx_rate{0};
    std::atomic<size_t> network_tx_rate{0};
    std::time_t last_update{0};
    size_t prev_rx_bytes{0};
    size_t prev_tx_bytes{0};
};

// Configuration structure
struct Config {
    int signaling_port;
    int http_port;
    std::string turn_server_host;
    int turn_server_port;
    std::string turn_username;
    std::string turn_password;
    std::string auth_username;
    std::string auth_password;
    std::string api_token;
    int max_providers;   // 0 = unlimited

    Config() : signaling_port(8888), http_port(8889),
               turn_server_host(""), turn_server_port(3478),
               turn_username(""), turn_password(""),
               auth_username("admin"), auth_password("admin"),
               api_token(""), max_providers(0) {}
};

struct ProviderInfo {
    std::string peer_id;
    std::string ip;
    int p2p_port;
    std::string sdp;
    std::string service_protocol;  // "rtsp" or "rtsps"
    std::string service_codec;     // "h264" or "h265"
    int socket_fd;
    std::time_t connected_at;
    size_t messages_sent;
    size_t messages_received;
    size_t bytes_sent;        // Bytes sent to this provider
    size_t bytes_received;    // Bytes received from this provider
    std::string consumer_ip;  // Consumer IP if connected
    bool has_consumer;        // Whether consumer is connected
    int consumer_port{0};     // Local RTSP port assigned on the consumer side
    int desired_consumer_port{0}; // Port requested by provider config (0 = dynamic)
    std::time_t first_seen;   // First time this service_id connected today
    std::time_t last_disconnect; // Last disconnect time
    size_t total_downtime;    // Total downtime in seconds
    size_t disconnect_count;  // Number of disconnections
    std::time_t last_ping_at{0};
    std::time_t last_pong_at{0};
    int missed_pongs{0};
};

struct Stats {
    size_t total_providers;
    size_t total_consumers;
    size_t total_connections;
    std::time_t server_start_time;
    
    Stats() : total_providers(0), total_consumers(0), total_connections(0) {
        server_start_time = std::time(nullptr);
    }
};

// Provider history structure for tracking downtime
struct ProviderHistory {
    std::time_t first_seen;
    std::time_t last_disconnect;
    size_t total_downtime;
    size_t disconnect_count;
};

std::mutex registry_mutex;
std::map<std::string, ProviderInfo> providers;
std::map<std::string, ProviderHistory> provider_history;
Stats server_stats;
Config config;
ResourceMetrics resource_metrics;

// ============================================================================
// SRT P2P registry — sits alongside the libjuice `providers` map and is used
// for the new SRT rendezvous flow.  A camera that speaks SRT registers a
// single STUN-discovered SRFLX address (no SDPs, no ICE).  Consumers (phone
// app) ask for that SRFLX and the server brokers a peer-info exchange so
// both sides can do simultaneous srt_connect (rendezvous) directly.
//
// We use the camera's signaling TCP socket as the channel to push SRT_PEER
// messages back to it.  The libjuice path is left completely untouched.
// ============================================================================
struct SrtProviderInfo {
    std::string service_id;
    std::string srflx_ip;     // camera's STUN-discovered public IP
    int         srflx_port;   // camera's STUN-discovered public port
    std::string lan_ip;       // camera's primary LAN IPv4 (RFC1918), if known.
                              // Used to fix the same-NAT hairpinning case:
                              // when consumer SRFLX matches provider SRFLX,
                              // both peers are behind the same NAT so they
                              // must rendezvous via LAN, not public IP.
    int         socket_fd;    // signaling-server's TCP socket to this camera
    std::time_t connected_at;
    /* Tracking for the dashboard.  Real-P2P media never touches the cloud,
     * so we infer "consumer connected" from the SRT_REQUEST broker event
     * (which IS visible to us).  has_consumer flips to true when we
     * forward an SRT_PEER to the camera and stays true afterwards;
     * consumer_ip records the most recent phone's apparent IP. */
    bool        has_consumer{false};
    std::string consumer_ip;
    std::time_t last_consumer_at{0};
};

std::mutex srt_registry_mutex;
std::map<std::string, SrtProviderInfo> srt_providers;

// PendingSDP: used to synchronise the consumer's REQUEST handler (which waits
// for a fresh provider SDP) with the provider's handle_client thread (which
// receives the SDP_READY message and notifies the waiting consumer thread).
// This ensures every PROVIDER_INFO response contains current ICE candidates
// rather than the stale SDP that was stored at initial REGISTER time.
struct PendingSDP {
    std::mutex mtx;
    std::condition_variable cv;
    std::string sdp;
    bool ready{false};
};
std::map<std::string, std::shared_ptr<PendingSDP>> pending_sdp_requests;
std::mutex pending_sdp_mutex;

// Event subscribers: consumer sockets that sent SUBSCRIBE
std::set<int> subscriber_sockets;
std::mutex subscriber_mutex;

// ============================================================================
// Session management for dashboard authentication
// ============================================================================
struct Session {
    std::time_t created_at;
    std::time_t last_access;
};

std::map<std::string, Session> active_sessions;
std::mutex session_mutex;

#define SESSION_TIMEOUT 3600  // 1 hour

static std::string generate_session_token() {
    static const char hex[] = "0123456789abcdef";
    std::string token;
    token.reserve(64);
    std::srand(std::time(nullptr) ^ (unsigned long)pthread_self());
    // Read from /dev/urandom for better randomness
    unsigned char buf[32];
    FILE* f = fopen("/dev/urandom", "rb");
    if (f) {
        if (fread(buf, 1, sizeof(buf), f) != sizeof(buf)) std::memset(buf, 0, sizeof(buf));
        fclose(f);
        for (int i = 0; i < 32; i++) {
            token += hex[buf[i] >> 4];
            token += hex[buf[i] & 0x0f];
        }
    } else {
        for (int i = 0; i < 64; i++) {
            token += hex[std::rand() % 16];
        }
    }
    return token;
}

static std::string create_session() {
    std::string token = generate_session_token();
    std::lock_guard<std::mutex> lock(session_mutex);
    Session s;
    s.created_at = std::time(nullptr);
    s.last_access = s.created_at;
    active_sessions[token] = s;
    return token;
}

static bool validate_session(const std::string& token) {
    if (token.empty()) return false;
    std::lock_guard<std::mutex> lock(session_mutex);
    auto it = active_sessions.find(token);
    if (it == active_sessions.end()) return false;
    auto now = std::time(nullptr);
    if (now - it->second.last_access > SESSION_TIMEOUT) {
        active_sessions.erase(it);
        return false;
    }
    it->second.last_access = now;
    return true;
}

static void remove_session(const std::string& token) {
    std::lock_guard<std::mutex> lock(session_mutex);
    active_sessions.erase(token);
}

// Extract cookie value from HTTP request
static std::string extract_cookie(const std::string& request, const std::string& name) {
    std::string cookie_header = "Cookie:";
    size_t pos = request.find(cookie_header);
    if (pos == std::string::npos) {
        cookie_header = "cookie:";
        pos = request.find(cookie_header);
    }
    if (pos == std::string::npos) return "";

    size_t line_end = request.find("\r\n", pos);
    std::string cookies = request.substr(pos + cookie_header.length(),
                                         line_end == std::string::npos ? std::string::npos : line_end - pos - cookie_header.length());

    std::string search = name + "=";
    size_t start = cookies.find(search);
    if (start == std::string::npos) return "";
    start += search.length();
    size_t end = cookies.find(';', start);
    std::string value = cookies.substr(start, end == std::string::npos ? std::string::npos : end - start);
    // Trim whitespace
    size_t f = value.find_first_not_of(" \t");
    size_t l = value.find_last_not_of(" \t");
    if (f == std::string::npos) return "";
    return value.substr(f, l - f + 1);
}

// Extract API token from Authorization header or X-API-Token header
static std::string extract_api_token(const std::string& request) {
    // Check Authorization: Bearer <token>
    size_t pos = request.find("Authorization: Bearer ");
    if (pos == std::string::npos) pos = request.find("authorization: Bearer ");
    if (pos != std::string::npos) {
        size_t start = pos + 22;  // len("Authorization: Bearer ")
        size_t end = request.find("\r\n", start);
        std::string token = request.substr(start, end == std::string::npos ? std::string::npos : end - start);
        size_t f = token.find_first_not_of(" \t");
        size_t l = token.find_last_not_of(" \t");
        if (f != std::string::npos) return token.substr(f, l - f + 1);
    }
    // Check X-API-Token: <token>
    pos = request.find("X-API-Token: ");
    if (pos == std::string::npos) pos = request.find("x-api-token: ");
    if (pos != std::string::npos) {
        size_t start = pos + 13;  // len("X-API-Token: ")
        size_t end = request.find("\r\n", start);
        std::string token = request.substr(start, end == std::string::npos ? std::string::npos : end - start);
        size_t f = token.find_first_not_of(" \t");
        size_t l = token.find_last_not_of(" \t");
        if (f != std::string::npos) return token.substr(f, l - f + 1);
    }
    return "";
}

// Check if request is authenticated (session cookie OR API token)
static bool is_authenticated(const std::string& request) {
    // Check API token first (for internal/programmatic access)
    if (!config.api_token.empty()) {
        std::string token = extract_api_token(request);
        if (!token.empty() && token == config.api_token) return true;
    }
    // Check session cookie (for browser/dashboard access)
    std::string session = extract_cookie(request, "p2p_session");
    return validate_session(session);
}

// URL decode
static std::string url_decode(const std::string& str) {
    std::string result;
    for (size_t i = 0; i < str.length(); i++) {
        if (str[i] == '%' && i + 2 < str.length()) {
            int val;
            std::istringstream iss(str.substr(i + 1, 2));
            if (iss >> std::hex >> val) {
                result += static_cast<char>(val);
                i += 2;
            } else {
                result += str[i];
            }
        } else if (str[i] == '+') {
            result += ' ';
        } else {
            result += str[i];
        }
    }
    return result;
}

// Extract form field from POST body
static std::string extract_form_field(const std::string& body, const std::string& field) {
    std::string search = field + "=";
    size_t start = body.find(search);
    if (start == std::string::npos) return "";
    start += search.length();
    size_t end = body.find('&', start);
    return url_decode(body.substr(start, end == std::string::npos ? std::string::npos : end - start));
}

static bool send_all(int socket_fd, const void* buf, size_t len);  // forward declaration

// Push NEW_PROVIDER event to all subscribed consumers.
// Sends outside the lock so a slow subscriber can't stall the registry.
void notify_subscribers(const std::string& service_id) {
    std::string msg = "type=NEW_PROVIDER\nservice_id=" + service_id + "\n";
    uint32_t msg_len = htonl((uint32_t)msg.size());

    std::vector<int> sockets;
    {
        std::lock_guard<std::mutex> lock(subscriber_mutex);
        sockets.assign(subscriber_sockets.begin(), subscriber_sockets.end());
    }

    std::set<int> failed;
    for (int fd : sockets) {
        bool ok = send_all(fd, &msg_len, sizeof(msg_len)) &&
                  send_all(fd, msg.c_str(), msg.size());
        if (!ok) failed.insert(fd);
    }

    if (!failed.empty()) {
        std::lock_guard<std::mutex> lock(subscriber_mutex);
        for (int fd : failed) {
            subscriber_sockets.erase(fd);
            close(fd);
        }
    }
}

// Trim whitespace
std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

// Load configuration from file
bool load_config(const char* config_file, Config& cfg) {
    std::ifstream file(config_file);
    if (!file.is_open()) {
        std::cout << "[X] Failed to open config file: " << config_file << "\n";
        return false;
    }
    
    std::string line;
    int line_num = 0;
    
    while (std::getline(file, line)) {
        line_num++;
        line = trim(line);
        
        if (line.empty() || line[0] == '#') continue;
        
        size_t pos = line.find('=');
        if (pos == std::string::npos) {
            std::cout << "[!] Warning: Invalid line " << line_num << " in config file\n";
            continue;
        }
        
        std::string key = trim(line.substr(0, pos));
        std::string value = trim(line.substr(pos + 1));
        
        if (key == "SIGNALING_PORT") {
            cfg.signaling_port = std::stoi(value);
            std::cout << "[✓] Config: SIGNALING_PORT = " << cfg.signaling_port << "\n";
        } else if (key == "HTTP_PORT") {
            cfg.http_port = std::stoi(value);
            std::cout << "[✓] Config: HTTP_PORT = " << cfg.http_port << "\n";
        } else if (key == "TURN_SERVER_HOST") {
            cfg.turn_server_host = value;
            std::cout << "[✓] Config: TURN_SERVER_HOST = " << cfg.turn_server_host << "\n";
        } else if (key == "TURN_SERVER_PORT") {
            cfg.turn_server_port = std::stoi(value);
            std::cout << "[✓] Config: TURN_SERVER_PORT = " << cfg.turn_server_port << "\n";
        } else if (key == "TURN_USERNAME") {
            cfg.turn_username = value;
            std::cout << "[✓] Config: TURN_USERNAME = " << cfg.turn_username << "\n";
        } else if (key == "TURN_PASSWORD") {
            cfg.turn_password = value;
            std::cout << "[✓] Config: TURN_PASSWORD = ****\n";
        } else if (key == "AUTH_USERNAME") {
            cfg.auth_username = value;
            std::cout << "[✓] Config: AUTH_USERNAME = " << cfg.auth_username << "\n";
        } else if (key == "AUTH_PASSWORD") {
            cfg.auth_password = value;
            std::cout << "[✓] Config: AUTH_PASSWORD = ****\n";
        } else if (key == "API_TOKEN") {
            cfg.api_token = value;
            std::cout << "[✓] Config: API_TOKEN = ****\n";
        } else if (key == "MAX_PROVIDERS") {
            cfg.max_providers = std::stoi(value);
            std::cout << "[✓] Config: MAX_PROVIDERS = "
                      << (cfg.max_providers == 0 ? "unlimited" : std::to_string(cfg.max_providers)) << "\n";
        } else {
            std::cout << "[!] Warning: Unknown config key '" << key << "' on line " << line_num << "\n";
        }
    }
    
    file.close();
    return true;
}

// Get CPU usage
double get_cpu_usage() {
    static unsigned long long prev_total = 0;
    static unsigned long long prev_idle = 0;
    
    std::ifstream file("/proc/stat");
    if (!file.is_open()) return 0.0;
    
    std::string line;
    std::getline(file, line);
    file.close();
    
    if (line.substr(0, 3) != "cpu") return 0.0;
    
    std::istringstream iss(line);
    std::string cpu;
    unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
    iss >> cpu >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal;
    
    unsigned long long total = user + nice + system + idle + iowait + irq + softirq + steal;
    unsigned long long total_idle = idle + iowait;
    
    unsigned long long diff_total = total - prev_total;
    unsigned long long diff_idle = total_idle - prev_idle;
    
    prev_total = total;
    prev_idle = total_idle;
    
    if (diff_total == 0) return 0.0;
    
    return 100.0 * (diff_total - diff_idle) / diff_total;
}

// Get memory usage
size_t get_memory_usage() {
    std::ifstream file("/proc/self/status");
    if (!file.is_open()) return 0;
    
    std::string line;
    while (std::getline(file, line)) {
        if (line.substr(0, 6) == "VmRSS:") {
            std::istringstream iss(line);
            std::string label;
            size_t value;
            iss >> label >> value;
            file.close();
            return value;
        }
    }
    file.close();
    return 0;
}

// Get network statistics
void get_network_stats(size_t& rx_bytes, size_t& tx_bytes) {
    std::ifstream file("/proc/net/dev");
    if (!file.is_open()) return;
    
    std::string line;
    rx_bytes = 0;
    tx_bytes = 0;
    
    std::getline(file, line);
    std::getline(file, line);
    
    while (std::getline(file, line)) {
        if (line.find("lo:") != std::string::npos) continue;
        
        size_t colon_pos = line.find(':');
        if (colon_pos == std::string::npos) continue;
        
        std::string data = line.substr(colon_pos + 1);
        std::istringstream iss(data);
        
        size_t rx, tx;
        size_t dummy;
        
        iss >> rx;
        for (int i = 0; i < 7; i++) iss >> dummy;
        iss >> tx;
        
        rx_bytes += rx;
        tx_bytes += tx;
    }
    
    file.close();
}

// Resource monitoring thread
void resource_monitor_thread() {
    while (true) {
        resource_metrics.cpu_usage = get_cpu_usage();
        resource_metrics.memory_usage_kb = get_memory_usage();
        
        size_t rx_bytes, tx_bytes;
        get_network_stats(rx_bytes, tx_bytes);
        
        auto now = std::time(nullptr);
        auto elapsed = now - resource_metrics.last_update;
        
        if (elapsed > 0 && resource_metrics.last_update > 0) {
            resource_metrics.network_rx_rate = (rx_bytes - resource_metrics.prev_rx_bytes) / elapsed;
            resource_metrics.network_tx_rate = (tx_bytes - resource_metrics.prev_tx_bytes) / elapsed;
        }
        
        resource_metrics.network_rx_bytes = rx_bytes;
        resource_metrics.network_tx_bytes = tx_bytes;
        resource_metrics.prev_rx_bytes = rx_bytes;
        resource_metrics.prev_tx_bytes = tx_bytes;
        resource_metrics.last_update = now;
        
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

// TCP application-level heartbeat: the signaling server sends PING to providers
// over the persistent TCP signaling connection. Providers respond with PONG.
// This is NOT STUN — STUN Binding Requests happen at the P2P/ICE layer between
// provider and consumer (handled by libjuice). This TCP heartbeat only detects
// whether the provider's signaling connection is still alive.
//
// IMPORTANT: registry_mutex is NOT held during send(). A stalled TCP send (full
// kernel buffer) would otherwise block the entire registry for up to minutes,
// preventing any new provider/consumer from registering or disconnecting.

void ping_monitor_thread() {
    struct PingTarget {
        int fd;
        std::string service_id;
        std::string msg_str;
        uint32_t msg_len_net;
        size_t payload_bytes;
    };

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(PING_INTERVAL));

        auto now = std::time(nullptr);
        std::vector<PingTarget> targets;
        std::set<int> stale_fds;

        // Step 1: collect send targets under lock — no I/O inside the lock.
        {
            std::lock_guard<std::mutex> lock(registry_mutex);
            for (auto& [service_id, info] : providers) {
                if (stale_fds.count(info.socket_fd)) continue;

                if (info.missed_pongs >= MAX_MISSED_PONGS) {
                    std::cout << "[!] Provider " << service_id << " missed " << info.missed_pongs
                              << " pongs — marking socket " << info.socket_fd << " for removal\n";
                    stale_fds.insert(info.socket_fd);
                    continue;
                }

                std::string msg = "type=PING\nservice_id=" + service_id +
                                  "\ntimestamp=" + std::to_string(now) + "\n";
                uint32_t len_net = htonl((uint32_t)msg.size());
                size_t total = sizeof(len_net) + msg.size();
                targets.push_back({info.socket_fd, service_id, std::move(msg), len_net, total});
            }
        }

        // Step 2: send without holding registry_mutex.
        std::set<int> failed_fds;
        for (auto& t : targets) {
            bool ok = send_all(t.fd, &t.msg_len_net, sizeof(t.msg_len_net)) &&
                      send_all(t.fd, t.msg_str.c_str(), t.msg_str.size());
            if (!ok) {
                std::cout << "[!] Failed to send ping to " << t.service_id
                          << " (fd=" << t.fd << ") — marking for removal\n";
                failed_fds.insert(t.fd);
            }
        }

        // Step 3: update stats and fold failed sockets into stale_fds.
        {
            std::lock_guard<std::mutex> lock(registry_mutex);
            for (auto& t : targets) {
                if (failed_fds.count(t.fd)) {
                    stale_fds.insert(t.fd);
                    continue;
                }
                auto it = providers.find(t.service_id);
                if (it != providers.end()) {
                    it->second.last_ping_at = now;
                    it->second.missed_pongs++;
                    it->second.bytes_sent += t.payload_bytes;
                    it->second.messages_sent++;
                }
            }
        }

        // Step 4: remove all providers bound to stale sockets.
        for (int fd : stale_fds) {
            std::cout << "[X] Removing dead provider socket: " << fd << "\n";
            {
                std::lock_guard<std::mutex> lock(registry_mutex);
                for (auto it = providers.begin(); it != providers.end(); ) {
                    if (it->second.socket_fd == fd) {
                        if (provider_history.count(it->first)) {
                            provider_history[it->first].last_disconnect = std::time(nullptr);
                            provider_history[it->first].disconnect_count++;
                        }
                        it = providers.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            close(fd);
        }
    }
}

// Reliable send — loops until all bytes are written or the peer is gone.
static bool send_all(int socket_fd, const void* buf, size_t len) {
    const char* ptr = static_cast<const char*>(buf);
    while (len > 0) {
        ssize_t n = send(socket_fd, ptr, len, MSG_NOSIGNAL);
        if (n <= 0) return false;
        ptr += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

// Simple message protocol: LENGTH(4 bytes) + DATA
bool send_message(int socket_fd, const std::string& msg) {
    uint32_t msg_len_net = htonl(static_cast<uint32_t>(msg.size()));
    if (!send_all(socket_fd, &msg_len_net, sizeof(msg_len_net))) return false;
    if (!send_all(socket_fd, msg.c_str(), msg.size()))          return false;

    // Track bytes sent for this provider (accounting only — lock held briefly).
    std::lock_guard<std::mutex> lock(registry_mutex);
    for (auto& [service_id, info] : providers) {
        if (info.socket_fd == socket_fd) {
            info.bytes_sent += sizeof(msg_len_net) + msg.size();
            break;
        }
    }

    return true;
}

bool recv_message(int socket_fd, std::string& msg) {
    uint32_t msg_len;
    if (recv(socket_fd, &msg_len, sizeof(msg_len), MSG_WAITALL) != sizeof(msg_len)) return false;
    msg_len = ntohl(msg_len);
    if (msg_len > 64 * 1024) return false;  // max 64 KB — protects against memory exhaustion
    
    std::vector<char> buffer(msg_len);
    if (recv(socket_fd, buffer.data(), msg_len, MSG_WAITALL) != (ssize_t)msg_len) return false;
    msg = std::string(buffer.begin(), buffer.end());
    
    // Track bytes received from this provider
    std::lock_guard<std::mutex> lock(registry_mutex);
    for (auto& [service_id, info] : providers) {
        if (info.socket_fd == socket_fd) {
            info.bytes_received += sizeof(msg_len) + msg_len;
            break;
        }
    }
    
    return true;
}

std::string get_client_ip(int socket_fd) {
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    if (getpeername(socket_fd, (struct sockaddr*)&addr, &addr_len) == 0) {
        return inet_ntoa(addr.sin_addr);
    }
    return "";
}

// Parse simple key=value message
std::map<std::string, std::string> parse_message(const std::string& msg) {
    std::map<std::string, std::string> result;
    std::istringstream iss(msg);
    std::string line;
    std::string current_key;
    std::string current_value;
    bool in_multiline = false;
    
    while (std::getline(iss, line)) {
        if (!in_multiline) {
            size_t pos = line.find('=');
            if (pos != std::string::npos) {
                current_key = line.substr(0, pos);
                current_value = line.substr(pos + 1);
                
                if (current_key == "sdp" || current_key == "consumer_sdp") {
                    in_multiline = true;
                    current_value += "\n";
                } else {
                    result[current_key] = current_value;
                }
            }
        } else {
            size_t pos = line.find('=');
            if (pos != std::string::npos && line.find("a=") != 0 && line.find("v=") != 0 
                && line.find("o=") != 0 && line.find("s=") != 0 && line.find("t=") != 0 
                && line.find("c=") != 0 && line.find("m=") != 0) {
                result[current_key] = current_value;
                current_key = line.substr(0, pos);
                current_value = line.substr(pos + 1);
                in_multiline = false;
            } else {
                current_value += line + "\n";
            }
        }
    }
    
    if (!current_key.empty()) {
        result[current_key] = current_value;
    }
    
    return result;
}

std::string create_message(const std::map<std::string, std::string>& data) {
    std::ostringstream oss;
    for (const auto& [key, value] : data) {
        oss << key << "=" << value;
        if (key != "sdp" && key != "consumer_sdp") {
            oss << "\n";
        }
    }
    return oss.str();
}

std::string get_uptime() {
    auto now = std::time(nullptr);
    auto uptime_seconds = now - server_stats.server_start_time;
    auto hours = uptime_seconds / 3600;
    auto minutes = (uptime_seconds % 3600) / 60;
    auto seconds = uptime_seconds % 60;
    
    std::ostringstream oss;
    oss << hours << "h " << minutes << "m " << seconds << "s";
    return oss.str();
}

std::string format_bytes(size_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit_index = 0;
    double size = bytes;
    
    while (size >= 1024.0 && unit_index < 4) {
        size /= 1024.0;
        unit_index++;
    }
    
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << size << " " << units[unit_index];
    return oss.str();
}

std::string generate_json_stats() {
    // Snapshot under lock — hold registry_mutex only for the copy, not for JSON
    // building (which can be slow with many providers).
    std::map<std::string, ProviderInfo> providers_snap;
    std::map<std::string, SrtProviderInfo> srt_providers_snap;
    Stats stats_snap;
    bool turn_configured;
    int sig_port, http_port;
    {
        std::lock_guard<std::mutex> lock(registry_mutex);
        providers_snap = providers;
        stats_snap     = server_stats;
        turn_configured = !config.turn_server_host.empty();
        sig_port  = config.signaling_port;
        http_port = config.http_port;
    }
    {
        std::lock_guard<std::mutex> lock(srt_registry_mutex);
        srt_providers_snap = srt_providers;
    }

    auto now = std::time(nullptr);
    std::ostringstream json;
    json << std::fixed << std::setprecision(2);
    json << "{\n";
    json << "  \"server\": {\n";
    json << "    \"uptime\": \"" << get_uptime() << "\",\n";
    json << "    \"signaling_port\": " << sig_port << ",\n";
    json << "    \"http_port\": " << http_port << ",\n";
    json << "    \"turn_configured\": " << (turn_configured ? "true" : "false") << ",\n";
    json << "    \"protocol_note\": \"STUN (RFC 5389) Binding Request/Response for NAT traversal; TURN (RFC 5766) extends STUN to relay traffic\"\n";
    json << "  },\n";
    json << "  \"resources\": {\n";
    json << "    \"cpu_usage\": " << resource_metrics.cpu_usage.load() << ",\n";
    json << "    \"memory_usage_mb\": " << (resource_metrics.memory_usage_kb.load() / 1024.0) << ",\n";
    json << "    \"network_rx_bytes\": " << resource_metrics.network_rx_bytes.load() << ",\n";
    json << "    \"network_tx_bytes\": " << resource_metrics.network_tx_bytes.load() << ",\n";
    json << "    \"network_rx_rate\": " << resource_metrics.network_rx_rate.load() << ",\n";
    json << "    \"network_tx_rate\": " << resource_metrics.network_tx_rate.load() << ",\n";
    json << "    \"network_rx_formatted\": \"" << format_bytes(resource_metrics.network_rx_bytes.load()) << "\",\n";
    json << "    \"network_tx_formatted\": \"" << format_bytes(resource_metrics.network_tx_bytes.load()) << "\",\n";
    json << "    \"network_rx_rate_formatted\": \"" << format_bytes(resource_metrics.network_rx_rate.load()) << "/s\",\n";
    json << "    \"network_tx_rate_formatted\": \"" << format_bytes(resource_metrics.network_tx_rate.load()) << "/s\"\n";
    json << "  },\n";
    json << "  \"stats\": {\n";
    json << "    \"active_providers\": " << (providers_snap.size() + srt_providers_snap.size()) << ",\n";
    json << "    \"active_libjuice_providers\": " << providers_snap.size() << ",\n";
    json << "    \"active_srt_providers\": " << srt_providers_snap.size() << ",\n";
    json << "    \"total_providers\": " << stats_snap.total_providers << ",\n";
    json << "    \"total_consumers\": " << stats_snap.total_consumers << ",\n";
    json << "    \"total_connections\": " << stats_snap.total_connections << "\n";
    json << "  },\n";
    json << "  \"providers\": [\n";

    bool first = true;
    for (const auto& [service_id, info] : providers_snap) {
        if (!first) json << ",\n";
        first = false;

        auto connected_duration = now - info.connected_at;

        json << "    {\n";
        json << "      \"service_id\": \"" << service_id << "\",\n";
        json << "      \"peer_id\": \"" << info.peer_id << "\",\n";
        json << "      \"ip\": \"" << info.ip << "\",\n";
        json << "      \"p2p_port\": " << info.p2p_port << ",\n";
        json << "      \"connected_duration\": " << connected_duration << ",\n";
        json << "      \"messages_sent\": " << info.messages_sent << ",\n";
        json << "      \"messages_received\": " << info.messages_received << ",\n";
        json << "      \"bytes_sent\": " << info.bytes_sent << ",\n";
        json << "      \"bytes_received\": " << info.bytes_received << ",\n";
        json << "      \"bytes_sent_formatted\": \"" << format_bytes(info.bytes_sent) << "\",\n";
        json << "      \"bytes_received_formatted\": \"" << format_bytes(info.bytes_received) << "\",\n";
        json << "      \"consumer_ip\": \"" << info.consumer_ip << "\",\n";
        json << "      \"has_consumer\": " << (info.has_consumer ? "true" : "false") << ",\n";
        json << "      \"total_downtime\": " << info.total_downtime << ",\n";
        json << "      \"disconnect_count\": " << info.disconnect_count << ",\n";

        auto time_since_first_seen = now - info.first_seen;
        double uptime_percentage = 0.0;
        if (time_since_first_seen > 0) {
            auto total_uptime = time_since_first_seen - info.total_downtime;
            uptime_percentage = (total_uptime * 100.0) / time_since_first_seen;
        }
        json << "      \"uptime_percentage\": " << uptime_percentage << ",\n";
        json << "      \"consumer_port\": " << info.consumer_port << ",\n";
        json << "      \"protocol\": \"" << info.service_protocol << "\",\n";
        json << "      \"codec\": \"" << (info.service_codec.empty() ? "h264" : info.service_codec) << "\",\n";
        json << "      \"transport\": \"libjuice\"\n";
        json << "    }";
    }

    /* Append SRT-registered providers as additional entries with transport=srt
     * so the dashboard's Active Services list shows them too. */
    for (const auto& [service_id, sp] : srt_providers_snap) {
        if (!first) json << ",\n";
        first = false;
        auto duration = now - sp.connected_at;
        json << "    {\n";
        json << "      \"service_id\": \"" << service_id << "\",\n";
        json << "      \"peer_id\": \"srt\",\n";
        json << "      \"ip\": \"" << sp.srflx_ip << "\",\n";
        json << "      \"p2p_port\": " << sp.srflx_port << ",\n";
        json << "      \"connected_duration\": " << duration << ",\n";
        json << "      \"messages_sent\": 0,\n";
        json << "      \"messages_received\": 0,\n";
        json << "      \"bytes_sent\": 0,\n";
        json << "      \"bytes_received\": 0,\n";
        json << "      \"bytes_sent_formatted\": \"P2P-direct\",\n";
        json << "      \"bytes_received_formatted\": \"P2P-direct\",\n";
        json << "      \"consumer_ip\": \"" << sp.consumer_ip << "\",\n";
        json << "      \"has_consumer\": " << (sp.has_consumer ? "true" : "false") << ",\n";
        json << "      \"total_downtime\": 0,\n";
        json << "      \"disconnect_count\": 0,\n";
        json << "      \"uptime_percentage\": 100,\n";
        json << "      \"consumer_port\": 0,\n";
        json << "      \"protocol\": \"srt-rendezvous\",\n";
        json << "      \"codec\": \"opaque\",\n";
        json << "      \"transport\": \"srt\"\n";
        json << "    }";
    }

    json << "\n  ]\n";
    json << "}\n";

    return json.str();
}

std::string generate_login_page(const std::string& error = "") {
    std::string error_html;
    if (!error.empty()) {
        error_html = "<div class=\"error\">" + error + "</div>";
    }
    return R"HTML(<!DOCTYPE html>
<html>
<head>
    <title>P2P Server - Login</title>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Arial, sans-serif; background: #0f172a; color: #e2e8f0; display: flex; justify-content: center; align-items: center; min-height: 100vh; }
        .login-container { background: #1e293b; border-radius: 16px; padding: 40px; border: 1px solid #334155; width: 100%; max-width: 400px; box-shadow: 0 25px 50px -12px rgba(0, 0, 0, 0.25); }
        h1 { font-size: 1.5rem; color: #60a5fa; margin-bottom: 8px; text-align: center; }
        .subtitle { color: #94a3b8; font-size: 0.875rem; text-align: center; margin-bottom: 30px; }
        .form-group { margin-bottom: 20px; }
        label { display: block; font-size: 0.875rem; color: #94a3b8; margin-bottom: 6px; font-weight: 600; }
        input[type="text"], input[type="password"] { width: 100%; padding: 12px 16px; background: #0f172a; border: 1px solid #334155; border-radius: 8px; color: #e2e8f0; font-size: 1rem; outline: none; transition: border-color 0.2s; }
        input:focus { border-color: #3b82f6; }
        button { width: 100%; padding: 12px; background: #3b82f6; color: white; border: none; border-radius: 8px; font-size: 1rem; font-weight: 600; cursor: pointer; transition: background 0.2s; }
        button:hover { background: #2563eb; }
        .error { background: #7f1d1d; color: #fca5a5; padding: 10px 16px; border-radius: 8px; font-size: 0.875rem; margin-bottom: 20px; text-align: center; }
        .icon { text-align: center; font-size: 3rem; margin-bottom: 16px; }
    </style>
</head>
<body>
    <div class="login-container">
        <div class="icon">&#x1f510;</div>
        <h1>P2P Signaling Server</h1>
        <p class="subtitle">Sign in to access the dashboard</p>
        )HTML" + error_html + R"HTML(
        <form method="POST" action="/login">
            <div class="form-group">
                <label>Username</label>
                <input type="text" name="username" required autofocus>
            </div>
            <div class="form-group">
                <label>Password</label>
                <input type="password" name="password" required>
            </div>
            <button type="submit">Sign In</button>
        </form>
    </div>
</body>
</html>)HTML";
}

std::string generate_html_dashboard() {
    return R"HTML(<!DOCTYPE html>
<html>
<head>
    <title>P2P Signaling Server Dashboard</title>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Arial, sans-serif; background: #0f172a; color: #e2e8f0; padding: 20px; }
        .container { max-width: 1400px; margin: 0 auto; }
        h1 { font-size: 2rem; margin-bottom: 10px; color: #60a5fa; }
        .subtitle { color: #94a3b8; margin-bottom: 30px; }
        .section-title { font-size: 1.25rem; color: #60a5fa; margin: 30px 0 15px 0; }
        .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(200px, 1fr)); gap: 20px; margin-bottom: 30px; }
        .card { background: #1e293b; border-radius: 12px; padding: 20px; border: 1px solid #334155; }
        .card h3 { font-size: 0.875rem; color: #94a3b8; text-transform: uppercase; letter-spacing: 0.05em; margin-bottom: 10px; }
        .card .value { font-size: 1.75rem; font-weight: bold; color: #60a5fa; }
        .card .label { font-size: 0.75rem; color: #64748b; margin-top: 5px; }
        .card.resource { border-left: 3px solid #10b981; }
        .card.resource .value { color: #10b981; }
        .providers { background: #1e293b; border-radius: 12px; padding: 20px; border: 1px solid #334155; margin-top: 20px; }
        .providers h2 { font-size: 1.5rem; margin-bottom: 20px; color: #60a5fa; }
        table { width: 100%; border-collapse: collapse; }
        th { text-align: left; padding: 12px; background: #0f172a; color: #94a3b8; font-size: 0.875rem; text-transform: uppercase; letter-spacing: 0.05em; }
        td { padding: 12px; border-top: 1px solid #334155; color: #e2e8f0; font-size: 0.875rem; }
        .status-dot { display: inline-block; width: 8px; height: 8px; border-radius: 50%; background: #10b981; margin-right: 8px; animation: pulse 2s infinite; }
        .status-dot.waiting { background: #f59e0b; }
        @keyframes pulse { 0%, 100% { opacity: 1; } 50% { opacity: 0.5; } }
        .refresh { background: #3b82f6; color: white; border: none; padding: 10px 20px; border-radius: 8px; cursor: pointer; font-size: 0.875rem; font-weight: 600; }
        .refresh:hover { background: #2563eb; }
        .logout { background: #dc2626; color: white; border: none; padding: 8px 16px; border-radius: 8px; cursor: pointer; font-size: 0.8rem; font-weight: 600; text-decoration: none; }
        .logout:hover { background: #b91c1c; }
        .top-bar { display: flex; justify-content: space-between; align-items: center; margin-bottom: 10px; }
        .empty { text-align: center; padding: 40px; color: #64748b; }

        /* Protocol badge */
        .badge { display: inline-block; padding: 2px 8px; border-radius: 999px; font-size: 0.7rem; font-weight: 700; text-transform: uppercase; letter-spacing: 0.05em; }
        .badge-rtsp  { background: #1e3a5f; color: #60a5fa; }
        .badge-rtsps { background: #1e3a5f; color: #93c5fd; }
        .badge-http  { background: #14532d; color: #4ade80; }
        .badge-https { background: #14532d; color: #86efac; }
        .badge-tcp   { background: #3b1f6b; color: #c084fc; }
        .badge-other { background: #292524; color: #a8a29e; }

        /* Tabs */
        .tab-bar { display: flex; gap: 4px; margin-bottom: 20px; flex-wrap: wrap; }
        .tab { padding: 8px 18px; border-radius: 8px 8px 0 0; cursor: pointer; font-size: 0.875rem; font-weight: 600;
               background: #0f172a; color: #64748b; border: 1px solid #334155; border-bottom: none; transition: all 0.15s; }
        .tab:hover { color: #e2e8f0; background: #1e293b; }
        .tab.active { background: #1e293b; color: #60a5fa; border-color: #3b82f6; border-bottom: 1px solid #1e293b; }
        .tab .count { display: inline-block; background: #334155; color: #94a3b8; border-radius: 999px;
                      font-size: 0.7rem; padding: 1px 7px; margin-left: 6px; }
        .tab.active .count { background: #1d4ed8; color: #bfdbfe; }
        .tab-content { display: none; }
        .tab-content.active { display: block; }
    </style>
</head>
<body>
    <div class="container">
        <div class="top-bar">
            <div>
                <h1>&#x1f310; P2P Signaling Server</h1>
                <p class="subtitle">Real-time monitoring dashboard with resource tracking</p>
            </div>
            <a href="/logout" class="logout">Logout</a>
        </div>

        <h3 class="section-title">Server Status</h3>
        <div class="grid">
            <div class="card">
                <h3>Server Uptime</h3>
                <div class="value" id="uptime">--</div>
            </div>
            <div class="card">
                <h3>Active Providers</h3>
                <div class="value" id="active-providers">0</div>
            </div>
            <div class="card">
                <h3>Total Connections</h3>
                <div class="value" id="total-connections">0</div>
            </div>
            <div class="card">
                <h3>Total Consumers</h3>
                <div class="value" id="total-consumers">0</div>
            </div>
        </div>

        <h3 class="section-title">Resource Usage (Proves Minimal Bandwidth During P2P)</h3>
        <div class="grid">
            <div class="card resource">
                <h3>CPU Usage</h3>
                <div class="value" id="cpu-usage">0%</div>
                <div class="label">System-wide</div>
            </div>
            <div class="card resource">
                <h3>Memory Usage</h3>
                <div class="value" id="memory-usage">0 MB</div>
                <div class="label">Server process</div>
            </div>
            <div class="card resource">
                <h3>Network RX</h3>
                <div class="value" id="network-rx">0 B</div>
                <div class="label" id="network-rx-rate">0 B/s</div>
            </div>
            <div class="card resource">
                <h3>Network TX</h3>
                <div class="value" id="network-tx">0 B</div>
                <div class="label" id="network-tx-rate">0 B/s</div>
            </div>
        </div>

        <div class="providers">
            <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 16px;">
                <h2>Active Services</h2>
                <button class="refresh" onclick="loadStats()">Refresh</button>
            </div>

            <!-- Tab bar (built dynamically) -->
            <div class="tab-bar" id="tab-bar"></div>

            <!-- Tab content panels (built dynamically) -->
            <div id="tab-panels"></div>
        </div>
    </div>

    <script>
        function formatDuration(seconds) {
            const h = Math.floor(seconds / 3600);
            const m = Math.floor((seconds % 3600) / 60);
            const s = seconds % 60;
            return h + 'h ' + m + 'm ' + s + 's';
        }

        function badgeClass(proto) {
            const p = (proto || '').toLowerCase();
            if (p === 'rtsp')  return 'badge badge-rtsp';
            if (p === 'rtsps') return 'badge badge-rtsps';
            if (p === 'http')  return 'badge badge-http';
            if (p === 'https') return 'badge badge-https';
            if (p === 'tcp')   return 'badge badge-tcp';
            return 'badge badge-other';
        }

        // Group protocols into tab categories
        function tabGroup(proto) {
            const p = (proto || '').toLowerCase();
            if (p === 'rtsp' || p === 'rtsps') return 'RTSP';
            if (p === 'http' || p === 'https') return 'HTTP';
            if (p === 'tcp')                   return 'TCP';
            return proto ? proto.toUpperCase() : 'Other';
        }

        function buildProviderTable(providers) {
            if (providers.length === 0) return '<div class="empty">No services in this category</div>';
            let html = '<table><thead><tr>'
                + '<th>Status</th><th>Service ID</th><th>Protocol</th>'
                + '<th>Provider IP</th><th>Consumer IP</th><th>Port</th>'
                + '<th>Duration</th><th>Downtime</th><th>Uptime %</th>'
                + '<th>Network</th><th>Messages</th>'
                + '</tr></thead><tbody>';
            providers.forEach(p => {
                const active = p.has_consumer;
                html += '<tr>';
                html += '<td><span class="status-dot' + (active ? '' : ' waiting') + '"></span>'
                      + (active ? 'P2P Active' : 'Waiting') + '</td>';
                html += '<td>' + p.service_id + '</td>';
                html += '<td><span class="' + badgeClass(p.protocol) + '">' + (p.protocol || 'tcp') + '</span></td>';
                html += '<td>' + p.ip + ':' + p.p2p_port + '</td>';
                html += '<td>' + (p.consumer_ip || '-') + '</td>';
                html += '<td>' + (p.consumer_port ? p.consumer_port : '-') + '</td>';
                html += '<td>' + formatDuration(p.connected_duration) + '</td>';
                html += '<td>' + formatDuration(p.total_downtime) + ' (' + p.disconnect_count + 'x)</td>';
                html += '<td>' + p.uptime_percentage.toFixed(1) + '%</td>';
                html += '<td>&#8593;' + p.bytes_sent_formatted + ' &#8595;' + p.bytes_received_formatted + '</td>';
                html += '<td>&#8593;' + p.messages_sent + ' &#8595;' + p.messages_received + '</td>';
                html += '</tr>';
            });
            html += '</tbody></table>';
            return html;
        }

        let activeTab = 'All';

        function renderTabs(providers) {
            // Build groups
            const groups = { 'All': providers };
            providers.forEach(p => {
                const g = tabGroup(p.protocol);
                if (!groups[g]) groups[g] = [];
                groups[g].push(p);
            });

            // If activeTab no longer exists, reset to All
            if (!groups[activeTab]) activeTab = 'All';

            // Tab bar
            const tabBar = document.getElementById('tab-bar');
            tabBar.innerHTML = Object.keys(groups).map(name =>
                '<div class="tab' + (name === activeTab ? ' active' : '') + '" onclick="switchTab(\'' + name + '\')">'
                + name + '<span class="count">' + groups[name].length + '</span></div>'
            ).join('');

            // Tab panels
            const panels = document.getElementById('tab-panels');
            panels.innerHTML = Object.keys(groups).map(name =>
                '<div class="tab-content' + (name === activeTab ? ' active' : '') + '" id="tab-' + name + '">'
                + buildProviderTable(groups[name])
                + '</div>'
            ).join('');
        }

        function switchTab(name) {
            activeTab = name;
            document.querySelectorAll('.tab').forEach(t => t.classList.remove('active'));
            document.querySelectorAll('.tab-content').forEach(t => t.classList.remove('active'));
            const tab = [...document.querySelectorAll('.tab')].find(t => t.textContent.startsWith(name));
            if (tab) tab.classList.add('active');
            const panel = document.getElementById('tab-' + name);
            if (panel) panel.classList.add('active');
        }

        function loadStats() {
            fetch('/api/stats')
                .then(res => res.json())
                .then(data => {
                    document.getElementById('uptime').textContent = data.server.uptime;
                    document.getElementById('active-providers').textContent = data.stats.active_providers;
                    document.getElementById('total-connections').textContent = data.stats.total_connections;
                    document.getElementById('total-consumers').textContent = data.stats.total_consumers;

                    document.getElementById('cpu-usage').textContent = data.resources.cpu_usage.toFixed(1) + '%';
                    document.getElementById('memory-usage').textContent = data.resources.memory_usage_mb.toFixed(1) + ' MB';
                    document.getElementById('network-rx').textContent = data.resources.network_rx_formatted;
                    document.getElementById('network-tx').textContent = data.resources.network_tx_formatted;
                    document.getElementById('network-rx-rate').textContent = data.resources.network_rx_rate_formatted;
                    document.getElementById('network-tx-rate').textContent = data.resources.network_tx_rate_formatted;

                    renderTabs(data.providers);
                })
                .catch(err => console.error('Failed to load stats:', err));
        }

        loadStats();
        setInterval(loadStats, 5000);
    </script>
</body>
</html>)HTML";
}

void handle_http_request(int client_socket) {
    char buffer[8192];
    ssize_t bytes = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
    if (bytes <= 0) {
        close(client_socket);
        return;
    }
    buffer[bytes] = '\0';

    std::string request(buffer);
    std::string response;

    // --- Login page (GET /login) — always accessible ---
    if (request.find("GET /login") == 0) {
        std::string html = generate_login_page();
        response = "HTTP/1.1 200 OK\r\n";
        response += "Content-Type: text/html\r\n";
        response += "Content-Length: " + std::to_string(html.length()) + "\r\n";
        response += "\r\n";
        response += html;
        send(client_socket, response.c_str(), response.length(), 0);
        close(client_socket);
        return;
    }

    // --- Login handler (POST /login) — always accessible ---
    if (request.find("POST /login") == 0) {
        // Extract body
        std::string body;
        size_t body_start = request.find("\r\n\r\n");
        if (body_start != std::string::npos) {
            body = request.substr(body_start + 4);
        }

        std::string username = extract_form_field(body, "username");
        std::string password = extract_form_field(body, "password");

        if (username == config.auth_username && password == config.auth_password) {
            std::string token = create_session();
            response = "HTTP/1.1 302 Found\r\n";
            response += "Set-Cookie: p2p_session=" + token + "; Path=/; HttpOnly; SameSite=Strict\r\n";
            response += "Location: /\r\n";
            response += "Content-Length: 0\r\n";
            response += "\r\n";
            std::cout << "[Auth] Login successful: " << username << "\n";
        } else {
            std::string html = generate_login_page("Invalid username or password");
            response = "HTTP/1.1 200 OK\r\n";
            response += "Content-Type: text/html\r\n";
            response += "Content-Length: " + std::to_string(html.length()) + "\r\n";
            response += "\r\n";
            response += html;
            std::cout << "[Auth] Login failed: " << username << "\n";
        }
        send(client_socket, response.c_str(), response.length(), 0);
        close(client_socket);
        return;
    }

    // --- Logout (GET /logout) ---
    if (request.find("GET /logout") == 0) {
        std::string session = extract_cookie(request, "p2p_session");
        if (!session.empty()) {
            remove_session(session);
        }
        response = "HTTP/1.1 302 Found\r\n";
        response += "Set-Cookie: p2p_session=; Path=/; HttpOnly; Max-Age=0\r\n";
        response += "Location: /login\r\n";
        response += "Content-Length: 0\r\n";
        response += "\r\n";
        send(client_socket, response.c_str(), response.length(), 0);
        close(client_socket);
        return;
    }

    // --- All other routes require authentication ---
    if (!is_authenticated(request)) {
        // Browser requests → redirect to login, API requests → 401 JSON
        if (request.find("GET /api/") == 0) {
            std::string json = "{\"error\":\"Authentication required\",\"hint\":\"Use Authorization: Bearer <token> or X-API-Token: <token>\"}";
            response = "HTTP/1.1 401 Unauthorized\r\n";
            response += "Content-Type: application/json\r\n";
            response += "Content-Length: " + std::to_string(json.length()) + "\r\n";
            response += "\r\n";
            response += json;
        } else {
            response = "HTTP/1.1 302 Found\r\n";
            response += "Location: /login\r\n";
            response += "Content-Length: 0\r\n";
            response += "\r\n";
        }
        send(client_socket, response.c_str(), response.length(), 0);
        close(client_socket);
        return;
    }

    // --- Authenticated routes ---
    if (request.find("GET /api/stats") == 0) {
        std::string json = generate_json_stats();
        response = "HTTP/1.1 200 OK\r\n";
        response += "Content-Type: application/json\r\n";
        response += "Access-Control-Allow-Origin: *\r\n";
        response += "Content-Length: " + std::to_string(json.length()) + "\r\n";
        response += "\r\n";
        response += json;
    } else if (request.find("GET / ") == 0 || request.find("GET /index.html") == 0) {
        std::string html = generate_html_dashboard();
        response = "HTTP/1.1 200 OK\r\n";
        response += "Content-Type: text/html\r\n";
        response += "Content-Length: " + std::to_string(html.length()) + "\r\n";
        response += "\r\n";
        response += html;
    } else {
        response = "HTTP/1.1 404 Not Found\r\n\r\n";
    }

    send(client_socket, response.c_str(), response.length(), 0);
    close(client_socket);
}

void http_server_thread() {
    int listen_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_socket < 0) {
        std::cerr << "[X] Failed to create HTTP socket\n";
        return;
    }
    
    int opt = 1;
    setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(config.http_port);
    server_addr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(listen_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "[X] Failed to bind HTTP socket\n";
        return;
    }
    
    if (listen(listen_socket, 10) < 0) {
        std::cerr << "[X] Failed to listen on HTTP socket\n";
        return;
    }
    
    std::cout << "[✓] HTTP dashboard server started on port " << config.http_port << "\n";
    
    while (true) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_socket = accept(listen_socket, (struct sockaddr*)&client_addr, &client_len);
        if (client_socket < 0) continue;
        
        std::thread(handle_http_request, client_socket).detach();
    }
}

void handle_client(int client_socket) {
    std::string client_ip = get_client_ip(client_socket);
    std::cout << "[+] Client connected from " << client_ip << "\n";
    
    {
        std::lock_guard<std::mutex> lock(registry_mutex);
        server_stats.total_connections++;
    }
    
    while (true) {
        std::string msg;
        if (!recv_message(client_socket, msg)) {
            break;
        }
        
        auto parsed = parse_message(msg);
        std::string msg_type = parsed["type"];
        
        if (msg_type == "REGISTER") {
            std::string service_id = parsed["service_id"];
            std::string peer_id = parsed["peer_id"];
            int p2p_port = std::stoi(parsed["p2p_port"]);
            std::string sdp = parsed["sdp"];
            std::string service_protocol = parsed.count("protocol") ? parsed["protocol"] : "rtsp";
            std::string service_codec    = parsed.count("codec")    ? parsed["codec"]    : "h264";
            int desired_consumer_port = parsed.count("consumer_port") ? std::stoi(parsed["consumer_port"]) : 0;

            // Authenticate provider: if api_token is configured, provider must match.
            if (!config.api_token.empty()) {
                std::string tok = parsed.count("api_token") ? parsed["api_token"] : "";
                if (tok != config.api_token) {
                    std::cout << "[X] REGISTER rejected (bad api_token) from " << client_ip
                              << " service_id=" << service_id << "\n";
                    std::map<std::string, std::string> err;
                    err["type"] = "ERROR";
                    err["message"] = "Authentication failed";
                    send_message(client_socket, create_message(err));
                    break;  // close connection immediately
                }
            }

            // Enforce provider capacity limit (allow re-registration from existing ID).
            // 0 = unlimited. Check under lock, send error OUTSIDE to avoid deadlock
            // (send_message() also acquires registry_mutex for byte-count tracking).
            bool at_capacity = false;
            {
                std::lock_guard<std::mutex> lock(registry_mutex);
                bool is_reconnect = (providers.find(service_id) != providers.end());
                at_capacity = (config.max_providers > 0 &&
                               !is_reconnect &&
                               (int)providers.size() >= config.max_providers);
            }
            if (at_capacity) {
                std::cout << "[X] Provider limit (" << config.max_providers << ") reached — rejecting "
                          << service_id << " from " << client_ip << "\n";
                std::map<std::string, std::string> err;
                err["type"] = "ERROR";
                err["message"] = "Server at capacity";
                send_message(client_socket, create_message(err));
                break;
            }

            std::cout << "[✓] Provider registered: " << service_id << "\n";
            std::cout << "    IP: " << client_ip << ":" << p2p_port << "\n";
            std::cout << "    Protocol: " << service_protocol << "  Codec: " << service_codec << "\n";
            std::cout << "    SDP length: " << sdp.length() << " bytes\n";
            
            {
                std::lock_guard<std::mutex> lock(registry_mutex);
                
                auto now = std::time(nullptr);
                
                // Check if this provider has history (reconnection)
                if (provider_history.find(service_id) != provider_history.end()) {
                    // Reconnection - calculate downtime
                    auto& history = provider_history[service_id];
                    if (history.last_disconnect > 0) {
                        size_t downtime = now - history.last_disconnect;
                        history.total_downtime += downtime;
                        history.disconnect_count++;
                        std::cout << "    [Reconnection] Downtime: " << downtime << " seconds\n";
                    }
                } else {
                    // First connection today
                    ProviderHistory history;
                    history.first_seen = now;
                    history.last_disconnect = 0;
                    history.total_downtime = 0;
                    history.disconnect_count = 0;
                    provider_history[service_id] = history;
                }
                
                ProviderInfo info;
                info.peer_id = peer_id;
                info.ip = client_ip;
                info.p2p_port = p2p_port;
                info.sdp = sdp;
                info.service_protocol = service_protocol;
                info.service_codec    = service_codec;
                info.desired_consumer_port = desired_consumer_port;
                info.socket_fd = client_socket;
                info.connected_at = now;
                info.messages_sent = 0;
                info.messages_received = 1;
                info.bytes_sent = 0;
                info.bytes_received = 0;
                info.consumer_ip = "";
                info.has_consumer = false;
                info.first_seen = provider_history[service_id].first_seen;
                info.last_disconnect = provider_history[service_id].last_disconnect;
                info.total_downtime = provider_history[service_id].total_downtime;
                info.disconnect_count = provider_history[service_id].disconnect_count;
                
                providers[service_id] = info;
                server_stats.total_providers++;
            }
            
            std::map<std::string, std::string> response;
            response["type"] = "REGISTERED";
            response["status"] = "ok";
            
            if (!config.turn_server_host.empty()) {
                response["turn_host"] = config.turn_server_host;
                response["turn_port"] = std::to_string(config.turn_server_port);
                response["turn_username"] = config.turn_username;
                response["turn_password"] = config.turn_password;
            }
            
            send_message(client_socket, create_message(response));

            {
                std::lock_guard<std::mutex> lock(registry_mutex);
                if (providers.find(service_id) != providers.end()) {
                    providers[service_id].messages_sent++;
                }
            }

            // Push instant notification to all subscribed consumers
            notify_subscribers(service_id);
        }
        else if (msg_type == "SRT_REGISTER") {
            // Camera (provider_srt) registers its STUN-discovered SRFLX so
            // phone-side consumers can do SRT rendezvous to it directly.
            // Camera's signaling TCP socket stays alive so we can push
            // SRT_PEER messages to it later when a phone connects.
            std::string service_id = parsed["service_id"];
            std::string srflx_ip   = parsed["srflx_ip"];
            int         srflx_port = parsed.count("srflx_port") ? std::stoi(parsed["srflx_port"]) : 0;
            std::string lan_ip     = parsed.count("lan_ip") ? parsed["lan_ip"] : "";

            if (service_id.empty() || srflx_ip.empty() || srflx_port <= 0) {
                std::map<std::string, std::string> err;
                err["type"]    = "ERROR";
                err["message"] = "SRT_REGISTER requires service_id, srflx_ip, srflx_port";
                send_message(client_socket, create_message(err));
                continue;
            }
            // Optional API token check, identical to libjuice REGISTER.
            if (!config.api_token.empty()) {
                std::string tok = parsed.count("api_token") ? parsed["api_token"] : "";
                if (tok != config.api_token) {
                    std::cout << "[X] SRT_REGISTER rejected (bad api_token) " << service_id
                              << " from " << client_ip << "\n";
                    std::map<std::string, std::string> err;
                    err["type"]    = "ERROR";
                    err["message"] = "Authentication failed";
                    send_message(client_socket, create_message(err));
                    break;
                }
            }

            {
                std::lock_guard<std::mutex> lock(srt_registry_mutex);
                SrtProviderInfo info;
                info.service_id   = service_id;
                info.srflx_ip     = srflx_ip;
                info.srflx_port   = srflx_port;
                info.lan_ip       = lan_ip;
                info.socket_fd    = client_socket;
                info.connected_at = std::time(nullptr);
                srt_providers[service_id] = info;
            }
            std::cout << "[✓] SRT provider registered: " << service_id
                      << "  srflx=" << srflx_ip << ":" << srflx_port
                      << (lan_ip.empty() ? std::string() : "  lan=" + lan_ip)
                      << "  via " << client_ip << "\n";

            std::map<std::string, std::string> response;
            response["type"]   = "SRT_REGISTERED";
            response["status"] = "ok";
            // Hand the camera the TURN credentials so it can fall back to
            // relay mode if direct rendezvous fails on a symmetric NAT.
            if (!config.turn_server_host.empty()) {
                response["turn_host"]     = config.turn_server_host;
                response["turn_port"]     = std::to_string(config.turn_server_port);
                response["turn_username"] = config.turn_username;
                response["turn_password"] = config.turn_password;
            }
            send_message(client_socket, create_message(response));
        }
        else if (msg_type == "SRT_REQUEST") {
            // Phone-side consumer asks for the camera's SRFLX so it can
            // start an SRT rendezvous.  Three-way exchange:
            //   1. consumer sends SRT_REQUEST + its own SRFLX
            //   2. server pushes SRT_PEER (consumer's SRFLX) to camera
            //      so camera fires its srt_connect at the consumer
            //   3. server replies SRT_PROVIDER (camera's SRFLX) to
            //      consumer so consumer fires its srt_connect at the camera
            // Both peers' srt_connect happen near-simultaneously,
            // SRT does the simultaneous-open NAT punch.
            std::string service_id          = parsed["service_id"];
            std::string consumer_srflx_ip   = parsed["srflx_ip"];
            int         consumer_srflx_port = parsed.count("srflx_port") ? std::stoi(parsed["srflx_port"]) : 0;
            std::string consumer_lan_ip     = parsed.count("lan_ip") ? parsed["lan_ip"] : "";

            if (service_id.empty() || consumer_srflx_ip.empty() || consumer_srflx_port <= 0) {
                std::map<std::string, std::string> err;
                err["type"]    = "ERROR";
                err["message"] = "SRT_REQUEST requires service_id, srflx_ip, srflx_port";
                send_message(client_socket, create_message(err));
                continue;
            }

            std::cout << "[→] SRT consumer request: " << service_id
                      << "  consumer_srflx=" << consumer_srflx_ip << ":" << consumer_srflx_port
                      << "  from " << client_ip << "\n";

            SrtProviderInfo provider;
            bool found = false;
            {
                std::lock_guard<std::mutex> lock(srt_registry_mutex);
                auto it = srt_providers.find(service_id);
                if (it != srt_providers.end()) { provider = it->second; found = true; }
            }
            if (!found) {
                std::map<std::string, std::string> err;
                err["type"]    = "ERROR";
                err["message"] = "SRT provider not found: " + service_id;
                send_message(client_socket, create_message(err));
                continue;
            }

            // 1. push SRT_PEER to the camera over its registered TCP socket
            std::map<std::string, std::string> peer_msg;
            peer_msg["type"]       = "SRT_PEER";
            peer_msg["service_id"] = service_id;
            peer_msg["srflx_ip"]   = consumer_srflx_ip;
            peer_msg["srflx_port"] = std::to_string(consumer_srflx_port);
            if (!consumer_lan_ip.empty()) peer_msg["lan_ip"] = consumer_lan_ip;

            /* Update the registry's bookkeeping so the dashboard can show
             * "Active" instead of "Waiting".  The cloud never sees actual
             * media bytes (that's the point of P2P) so we infer activity
             * from this broker event. */
            {
                std::lock_guard<std::mutex> lock(srt_registry_mutex);
                auto it = srt_providers.find(service_id);
                if (it != srt_providers.end()) {
                    it->second.has_consumer    = true;
                    it->second.consumer_ip     = client_ip;
                    it->second.last_consumer_at = std::time(nullptr);
                }
            }
            if (!send_message(provider.socket_fd, create_message(peer_msg))) {
                // Camera's signaling socket has gone stale.  Drop it from
                // the registry so the camera's next SRT_REGISTER builds a
                // fresh entry.
                {
                    std::lock_guard<std::mutex> lock(srt_registry_mutex);
                    srt_providers.erase(service_id);
                }
                std::map<std::string, std::string> err;
                err["type"]    = "ERROR";
                err["message"] = "SRT provider not reachable (stale connection) — retry";
                send_message(client_socket, create_message(err));
                continue;
            }

            // 2. reply with the camera's SRFLX so the consumer can
            //    srt_connect simultaneously.
            std::map<std::string, std::string> response;
            response["type"]       = "SRT_PROVIDER";
            response["service_id"] = service_id;
            response["srflx_ip"]   = provider.srflx_ip;
            response["srflx_port"] = std::to_string(provider.srflx_port);
            if (!provider.lan_ip.empty()) response["lan_ip"] = provider.lan_ip;
            if (!config.turn_server_host.empty()) {
                response["turn_host"]     = config.turn_server_host;
                response["turn_port"]     = std::to_string(config.turn_server_port);
                response["turn_username"] = config.turn_username;
                response["turn_password"] = config.turn_password;
            }
            send_message(client_socket, create_message(response));
            std::cout << "[✓] SRT broker complete: consumer "
                      << consumer_srflx_ip << ":" << consumer_srflx_port
                      << " <-> provider " << provider.srflx_ip << ":" << provider.srflx_port << "\n";
        }
        else if (msg_type == "SUBSCRIBE") {
            // Consumer wants push events — add socket to subscriber list
            std::cout << "[✓] Event subscriber: " << client_ip << "\n";
            {
                std::lock_guard<std::mutex> sub_lock(subscriber_mutex);
                subscriber_sockets.insert(client_socket);
            }
            // Immediately send all currently connected providers
            {
                std::lock_guard<std::mutex> lock(registry_mutex);
                for (const auto& [sid, info] : providers) {
                    std::string notify = "type=NEW_PROVIDER\nservice_id=" + sid + "\n";
                    uint32_t nlen = htonl((uint32_t)notify.size());
                    send(client_socket, &nlen, sizeof(nlen), MSG_NOSIGNAL);
                    send(client_socket, notify.c_str(), notify.size(), MSG_NOSIGNAL);
                }
            }
            // Keep looping — connection stays alive for future events
        }
        else if (msg_type == "REQUEST") {
            std::string service_id = parsed["service_id"];
            std::string consumer_sdp = parsed["consumer_sdp"];
            
            std::cout << "[→] Consumer request: " << service_id << " from " << client_ip << "\n";
            std::cout << "    Consumer SDP length: " << consumer_sdp.length() << " bytes\n";
            
            {
                std::lock_guard<std::mutex> lock(registry_mutex);
                server_stats.total_consumers++;
                server_stats.total_connections++;
            }
            
            ProviderInfo provider_info;
            bool found = false;
            
            {
                std::lock_guard<std::mutex> lock(registry_mutex);
                auto it = providers.find(service_id);
                if (it != providers.end()) {
                    provider_info = it->second;
                    found = true;
                    it->second.messages_received++;
                }
            }
            
            if (!found) {
                std::cout << "[X] Provider not found\n";
                std::map<std::string, std::string> response;
                response["type"] = "ERROR";
                response["message"] = "Provider not found";
                send_message(client_socket, create_message(response));
                continue;
            }
            
            // Reset consumer status — a fresh ICE negotiation is about to start.
            // has_consumer stays false until the provider confirms it has new candidates.
            {
                std::lock_guard<std::mutex> lock(registry_mutex);
                if (providers.count(service_id)) {
                    providers[service_id].has_consumer = false;
                    providers[service_id].consumer_ip = "";
                }
            }

            // Step 1: Register a PendingSDP slot BEFORE sending CONSUMER_CONNECT.
            // This avoids a race where SDP_READY arrives before we start waiting.
            auto pending_sdp = std::make_shared<PendingSDP>();
            {
                std::lock_guard<std::mutex> lock(pending_sdp_mutex);
                pending_sdp_requests[service_id] = pending_sdp;
            }

            // Step 2: forward consumer SDP to provider.
            // If the provider's TCP socket has gone stale (e.g. provider reconnected
            // after a network blip and the registry still holds the old fd), the send
            // will fail here — before we waste time starting ICE on the consumer side.
            std::cout << "[→] Forwarding consumer SDP to provider " << service_id << "\n";
            std::map<std::string, std::string> provider_msg;
            provider_msg["type"] = "CONSUMER_CONNECT";
            provider_msg["service_id"] = service_id;
            provider_msg["consumer_ip"] = client_ip;
            // Always force a fresh ICE rebuild on the provider.  With
            // libjuice consent freshness disabled, the provider cannot
            // detect that an old consumer's path went dead — its
            // svc->connected flag stays true forever, blocking a returning
            // consumer's CONSUMER_CONNECT.  force_reconnect=1 ensures a new
            // REQUEST always reclaims the camera; the provider's
            // last_consumer_connect_time debounce still prevents reconnect
            // storms within a 15 s window.
            provider_msg["force_reconnect"] = "1";
            provider_msg["consumer_sdp"] = consumer_sdp;

            if (!send_message(provider_info.socket_fd, create_message(provider_msg))) {
                // Stale socket — provider TCP connection dropped since last REGISTER.
                // Remove stale entry so the provider's next REGISTER creates a fresh one.
                std::cout << "[X] Provider " << service_id
                          << ": socket " << provider_info.socket_fd
                          << " is stale — removing from registry, telling consumer to retry\n";
                {
                    std::lock_guard<std::mutex> lock(pending_sdp_mutex);
                    pending_sdp_requests.erase(service_id);
                }
                {
                    std::lock_guard<std::mutex> lock(registry_mutex);
                    providers.erase(service_id);
                }
                std::map<std::string, std::string> err_response;
                err_response["type"] = "ERROR";
                err_response["message"] = "Provider not reachable (stale connection) — retry";
                send_message(client_socket, create_message(err_response));
                continue;
            }

            // Step 3: wait up to 5 seconds for the provider to send SDP_READY with
            // its fresh local SDP (gathered after recreating the ICE agent).
            // The provider's gather window is now capped at ~3 s, so 5 s here
            // gives a comfortable margin without bloating per-reconnect latency.
            // If it times out we fall back to the cached SDP — this is harmless
            // for the very first connection (the original agent is reused, same
            // candidates) and is a safe degraded path for any future case where
            // the provider is unexpectedly slow.
            std::string provider_sdp_to_use = provider_info.sdp;  // fallback = cached
            {
                std::unique_lock<std::mutex> plock(pending_sdp->mtx);
                bool got_fresh = pending_sdp->cv.wait_for(
                    plock,
                    std::chrono::seconds(5),
                    [&]{ return pending_sdp->ready; });
                if (got_fresh) {
                    provider_sdp_to_use = pending_sdp->sdp;
                    std::cout << "[✓] Fresh provider SDP received — ICE candidates are current\n";
                } else {
                    std::cout << "[!] Timeout waiting for provider SDP_READY"
                              << " — using cached SDP (first-connection or provider delay)\n";
                }
            }
            // Clean up the pending slot regardless of outcome
            {
                std::lock_guard<std::mutex> lock(pending_sdp_mutex);
                pending_sdp_requests.erase(service_id);
            }

            // Step 4: provider reached — send PROVIDER_INFO to consumer with the
            // fresh (or cached) provider SDP so ICE uses current candidates.
            std::cout << "[✓] Provider reached — sending PROVIDER_INFO to consumer\n";
            std::map<std::string, std::string> response;
            response["type"] = "PROVIDER_INFO";
            response["provider_ip"] = provider_info.ip;
            response["provider_port"] = std::to_string(provider_info.p2p_port);
            response["provider_peer_id"] = provider_info.peer_id;
            response["protocol"] = provider_info.service_protocol;
            response["codec"]    = provider_info.service_codec.empty() ? "h264" : provider_info.service_codec;
            response["consumer_port"] = std::to_string(provider_info.desired_consumer_port);
            response["provider_sdp"] = provider_sdp_to_use;

            if (!config.turn_server_host.empty()) {
                response["turn_host"] = config.turn_server_host;
                response["turn_port"] = std::to_string(config.turn_server_port);
                response["turn_username"] = config.turn_username;
                response["turn_password"] = config.turn_password;
            }

            send_message(client_socket, create_message(response));

            {
                std::lock_guard<std::mutex> lock(registry_mutex);
                if (providers.find(service_id) != providers.end()) {
                    providers[service_id].messages_sent++;
                    providers[service_id].consumer_ip = client_ip;
                    providers[service_id].has_consumer = true;
                }
            }

            std::cout << "[✓] SDP exchange complete — ICE negotiation underway\n";
            std::cout << "    Consumer IP: " << client_ip << "\n";
        }
        else if (msg_type == "PONG") {
            // Provider responds to PING
            std::string service_id = parsed.count("service_id") ? parsed["service_id"] : "";
            std::lock_guard<std::mutex> lock(registry_mutex);
            auto now = std::time(nullptr);

            // Liveness is actually per TCP socket (one provider process can register
            // multiple service_ids). A pong for any one service on that socket proves
            // the socket is alive, so reset missed_pongs for all services on it.
            int fd = -1;
            if (!service_id.empty()) {
                auto it = providers.find(service_id);
                if (it != providers.end()) {
                    fd = it->second.socket_fd;
                }
            }
            if (fd < 0) {
                fd = client_socket;
            }

            for (auto& [sid, info] : providers) {
                if (info.socket_fd == fd) {
                    info.last_pong_at = now;
                    info.missed_pongs = 0;
                }
            }
        }
        else if (msg_type == "PORT_UPDATE") {
            // Consumer reports the local RTSP port it assigned to this camera
            std::string service_id = parsed.count("service_id") ? parsed["service_id"] : "";
            int port = parsed.count("local_port") ? std::stoi(parsed["local_port"]) : 0;
            if (!service_id.empty() && port > 0) {
                std::lock_guard<std::mutex> lock(registry_mutex);
                if (providers.find(service_id) != providers.end()) {
                    providers[service_id].consumer_port = port;
                    std::cout << "[✓] Port update: " << service_id
                              << " → consumer RTSP port " << port << "\n";
                }
            }
        }
        else if (msg_type == "SDP_READY") {
            // Provider sends its freshly gathered local SDP immediately after
            // recreating the ICE agent in response to a CONSUMER_CONNECT.
            // We update the registry so future consumers also get current candidates,
            // then wake any consumer REQUEST handler that is waiting for this SDP.
            std::string service_id = parsed.count("service_id") ? parsed["service_id"] : "";
            std::string new_sdp    = parsed.count("sdp")        ? parsed["sdp"]        : "";
            if (!service_id.empty() && !new_sdp.empty()) {
                {
                    std::lock_guard<std::mutex> lock(registry_mutex);
                    if (providers.count(service_id)) {
                        providers[service_id].sdp = new_sdp;
                        std::cout << "[✓] SDP_READY: updated stored SDP for " << service_id << "\n";
                    }
                }
                // Notify the consumer REQUEST thread waiting for this service_id
                std::shared_ptr<PendingSDP> pending;
                {
                    std::lock_guard<std::mutex> lock(pending_sdp_mutex);
                    auto it = pending_sdp_requests.find(service_id);
                    if (it != pending_sdp_requests.end()) {
                        pending = it->second;  // grab shared_ptr before releasing lock
                    }
                }
                if (pending) {
                    std::lock_guard<std::mutex> plock(pending->mtx);
                    pending->sdp   = new_sdp;
                    pending->ready = true;
                    pending->cv.notify_one();
                }
            }
        }
        else if (msg_type == "PROVIDER_DISCONNECTED") {
            // Provider notifies us that its ICE connection failed (JUICE_STATE_FAILED).
            // Clear the consumer status so the dashboard shows "Waiting" instead of
            // the permanently-stuck "P2P Active" that would appear otherwise.
            std::string service_id = parsed.count("service_id") ? parsed["service_id"] : "";
            if (!service_id.empty()) {
                std::lock_guard<std::mutex> lock(registry_mutex);
                if (providers.count(service_id)) {
                    providers[service_id].has_consumer = false;
                    providers[service_id].consumer_ip  = "";
                    std::cout << "[!] PROVIDER_DISCONNECTED: cleared consumer status for "
                              << service_id << "\n";
                }
            }
        }
    }
    
    std::cout << "[-] Client disconnected: " << client_ip << "\n";

    // Remove from subscriber list if it was a subscriber
    {
        std::lock_guard<std::mutex> sub_lock(subscriber_mutex);
        subscriber_sockets.erase(client_socket);
    }

    // Clean up any SRT registration belonging to this socket.
    {
        std::lock_guard<std::mutex> lock(srt_registry_mutex);
        for (auto it = srt_providers.begin(); it != srt_providers.end(); ) {
            if (it->second.socket_fd == client_socket) {
                std::cout << "[!] SRT provider removed: " << it->first << "\n";
                it = srt_providers.erase(it);
            } else {
                ++it;
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(registry_mutex);
        for (auto it = providers.begin(); it != providers.end(); ) {
            if (it->second.socket_fd == client_socket) {
                std::string service_id = it->first;
                std::cout << "[!] Provider removed: " << service_id << "\n";
                
                // Update history with disconnect time
                if (provider_history.find(service_id) != provider_history.end()) {
                    provider_history[service_id].last_disconnect = std::time(nullptr);
                    
                    auto connected_duration = std::time(nullptr) - it->second.connected_at;
                    std::cout << "    Connected duration: " << connected_duration << " seconds\n";
                    std::cout << "    Total downtime today: " << provider_history[service_id].total_downtime << " seconds\n";
                    std::cout << "    Disconnect count: " << provider_history[service_id].disconnect_count << "\n";
                }
                
                it = providers.erase(it);
            } else {
                ++it;
            }
        }
    }
    
    close(client_socket);
}

int main(int argc, char* argv[]) {
    // Ignore SIGPIPE globally: broken TCP peers / log pipes return EPIPE on
    // send/write instead of killing the process. (MEM-5)
    std::signal(SIGPIPE, SIG_IGN);

    log_file.open("signaling.log", std::ios::app);
    if (log_file.is_open()) {
        old_cout_buf = std::cout.rdbuf();
        tee = new TeeStream(old_cout_buf, log_file.rdbuf());
        std::cout.rdbuf(tee);
    }

    std::cout << "\n╔════════════════════════════════════════════╗\n";
    std::cout << "║   P2P Signaling Server (Stable Enhanced)  ║\n";
    std::cout << "╚════════════════════════════════════════════╝\n\n";
    
    const char* config_file = (argc >= 2) ? argv[1] : "signaling.conf";
    
    std::cout << "[...] Loading configuration from: " << config_file << "\n";
    if (!load_config(config_file, config)) {
        std::cout << "[!] Using default configuration\n";
    }
    std::cout << "[✓] Configuration loaded successfully\n\n";
    
    std::cout << "🌐 Signaling Port: " << config.signaling_port << "\n";
    std::cout << "📊 Dashboard Port: " << config.http_port << "\n";
    std::cout << "🔒 Auth: username=" << config.auth_username << ", API token " << (config.api_token.empty() ? "not set" : "configured") << "\n";
    if (!config.turn_server_host.empty()) {
        std::cout << "🔄 STUN/TURN Server (RFC 5389/5766): " << config.turn_server_host << ":" << config.turn_server_port << "\n";
        std::cout << "   STUN: Binding Request/Response for public IP discovery\n";
        std::cout << "   TURN: Extends STUN to relay traffic when direct P2P fails\n";
    }
    std::cout << "\n";
    
    // Start resource monitoring thread
    std::thread resource_thread(resource_monitor_thread);
    resource_thread.detach();
    std::cout << "[✓] Resource monitoring started\n";
    
    // Start ping monitor thread
    std::thread ping_thread(ping_monitor_thread);
    ping_thread.detach();
    std::cout << "[✓] Ping monitor started\n";
    
    // Start HTTP dashboard server
    std::thread http_thread(http_server_thread);
    http_thread.detach();
    
    int listen_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_socket < 0) {
        std::cerr << "[X] Failed to create socket\n";
        return 1;
    }
    
    int opt = 1;
    setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(config.signaling_port);
    server_addr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(listen_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "[X] Failed to bind\n";
        return 1;
    }
    
    if (listen(listen_socket, 10) < 0) {
        std::cerr << "[X] Failed to listen\n";
        return 1;
    }
    
    std::cout << "[✓] Signaling server started\n";
    std::cout << "════════════════════════════════════════════\n";
    std::cout << " Ready for connections\n";
    std::cout << " Dashboard: http://localhost:" << config.http_port << "\n";
    std::cout << "════════════════════════════════════════════\n\n";
    
    while (true) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_socket = accept(listen_socket, (struct sockaddr*)&client_addr, &client_len);
        if (client_socket < 0) continue;

        // OS-level TCP keepalive: kernel probes silently-dead peers without
        // waiting for the application PING/PONG cycle (~15s detection instead of
        // up to PING_INTERVAL * MAX_MISSED_PONGS = 30s).
        { int v = 1;  setsockopt(client_socket, SOL_SOCKET,  SO_KEEPALIVE,  &v, sizeof(v)); }
        { int v = 15; setsockopt(client_socket, IPPROTO_TCP, TCP_KEEPIDLE,  &v, sizeof(v)); }
        { int v = 5;  setsockopt(client_socket, IPPROTO_TCP, TCP_KEEPINTVL, &v, sizeof(v)); }
        { int v = 3;  setsockopt(client_socket, IPPROTO_TCP, TCP_KEEPCNT,   &v, sizeof(v)); }
        // TCP_NODELAY: disable Nagle so small signaling messages are sent immediately.
        { int v = 1;  setsockopt(client_socket, IPPROTO_TCP, TCP_NODELAY,   &v, sizeof(v)); }

        std::thread(handle_client, client_socket).detach();
    }
    
    return 0;
}