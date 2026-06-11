#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <MQTTClient.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <netdb.h>
#include "cJSON.h"
#include <sys/types.h>
#include <sys/wait.h>
#include "N_HTTPS.h"
// OpenSSL headers not needed - paho-mqtt handles TLS internally
#include <pthread.h>
#include <signal.h>
#include <curl/curl.h>

#define SAVE_PATH "/etc/jffs2/ambicam/P2Pambicam_min.ini"
// #define STREM_CONFIG "/etc/jffs2/ambicam/config.json"
#define STREM_CONFIG "/mny/mtd/ipc/ambicam/config.json"
#define JSON_BUFFER_SIZE 2048
#define BUFFER_SIZE 4096
#define RECONNECT_INTERVAL 5
#define TIMEOUT 10000L
#define QOS 0

#define SERVER_IP "127.0.0.1"  
#define SERVER_PORT 80        
#define CHUNK_SIZE 180        
#define ABUFFER_SIZE 1024  
#define MAX_RETRY 5

int reconnect_and_subscribe();
int send_https_request(const char *url, const char *method, const char *body, char *response, size_t response_size);
int is_password_change_success(const char *response);
int update_password(const char *new_password);
const char* get_system_info();
void sanitize_response(char *response);
int connect_broker(const char *host);
void resubscribe_topics(void);
int send_HTTPSPTZ_request(const char *act, char *response, size_t response_size);
void publish_mqtt_message(MQTTClient client, const char* message, int caseValue);
int execute_command(const char *command, char *output_buffer, size_t buffer_size);
void get_ini_files(void);
void cameraname_Change(void);
void run_startup_cases(void);
void run_main_loop(void);
void handle_case_0(void);
void handle_case_2(void);
void handle_case_6(void);
void handle_case_38(void);
void handle_case_39(void);
void handle_case_23(void);
void handle_case_34(void);

char MQTT_CLIENT_ID[24];
char MQTT_TOPIC_RECEIVE[50];
char MQTT_TOPIC_SEND[50];
char publishTopic[256];
int inifile_COUNT = 0;
char response[8192];
char MQTT_MAIN_HOST[256];
char MQTT_BACKUP_HOST[256];
char CURRENT_HOST[256];
int using_backup = 0;

char MQTT_HOST[128];
char USERNAME[64];   /* MQTT broker username (from config.json) */
char PASSWORD[64];   /* MQTT broker password (from config.json) */

/* Camera HTTP API credentials (received from server via case 80) */
char CAM_USERNAME[64] = "admin";
char CAM_PASSWORD[64] = "";
char CAM_AUTH_BASIC[256] = "Basic YWRtaW46";  /* base64(admin:) — rebuilt dynamically */
pthread_mutex_t cred_mutex = PTHREAD_MUTEX_INITIALIZER;

MQTTClient client;
MQTTClient_connectOptions conn_opts = MQTTClient_connectOptions_initializer;
pthread_mutex_t cpu_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mqtt_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile sig_atomic_t shutdown_requested = 0;

void handle_signal(int sig) {
    printf("Received signal %d, shutting down...\n", sig);
    shutdown_requested = 1;
}

/* Base64 encode a string (for HTTP Basic auth header) */
static void base64_encode(const char *input, char *output, size_t out_size) {
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t len = strlen(input);
    size_t i, j = 0;
    for (i = 0; i < len && j + 4 < out_size; i += 3) {
        unsigned int a = (unsigned char)input[i];
        unsigned int b = (i + 1 < len) ? (unsigned char)input[i + 1] : 0;
        unsigned int c = (i + 2 < len) ? (unsigned char)input[i + 2] : 0;
        unsigned int triple = (a << 16) | (b << 8) | c;
        output[j++] = b64[(triple >> 18) & 0x3F];
        output[j++] = b64[(triple >> 12) & 0x3F];
        output[j++] = (i + 1 < len) ? b64[(triple >> 6) & 0x3F] : '=';
        output[j++] = (i + 2 < len) ? b64[triple & 0x3F] : '=';
    }
    output[j] = '\0';
}

/* Rebuild the Basic auth header from current CAM_USERNAME:CAM_PASSWORD */
void rebuild_basic_auth(void) {
    char raw[128];
    char encoded[192];
    snprintf(raw, sizeof(raw), "%s:%s", CAM_USERNAME, CAM_PASSWORD);
    base64_encode(raw, encoded, sizeof(encoded));
    snprintf(CAM_AUTH_BASIC, sizeof(CAM_AUTH_BASIC), "Basic %s", encoded);
    printf("Camera auth updated for user: %s\n", CAM_USERNAME);
}

/* Thread-safe: copy current camera credentials into local buffers */
void get_cam_credentials(char *user, size_t user_size, char *pass, size_t pass_size) {
    pthread_mutex_lock(&cred_mutex);
    strncpy(user, CAM_USERNAME, user_size - 1);
    user[user_size - 1] = '\0';
    strncpy(pass, CAM_PASSWORD, pass_size - 1);
    pass[pass_size - 1] = '\0';
    pthread_mutex_unlock(&cred_mutex);
}

/* Thread-safe: copy current Basic auth header */
void get_cam_auth_basic(char *auth, size_t auth_size) {
    pthread_mutex_lock(&cred_mutex);
    strncpy(auth, CAM_AUTH_BASIC, auth_size - 1);
    auth[auth_size - 1] = '\0';
    pthread_mutex_unlock(&cred_mutex);
}

int is_password_change_success(const char *response) {
    if (response == NULL) return 0;
    if (strstr(response, "<success>") != NULL || strstr(response, "200") != NULL) {
        return 1;
    }
    return 0;
}

int update_password(const char *new_password) {
    if (new_password == NULL || strlen(new_password) == 0) return -1;
    /* Update camera HTTP credentials (thread-safe) */
    pthread_mutex_lock(&cred_mutex);
    snprintf(CAM_PASSWORD, sizeof(CAM_PASSWORD), "%s", new_password);
    rebuild_basic_auth();
    pthread_mutex_unlock(&cred_mutex);
    printf("Camera HTTP password updated\n");
    return 0;
}

int load_mqtt_config(const char *filename) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        perror("Failed to open config.json");
        return -1;
    }

    char buffer[1024];
    size_t read_size = fread(buffer, 1, sizeof(buffer) - 1, fp);
    fclose(fp);
    buffer[read_size] = '\0';

    cJSON *json = cJSON_Parse(buffer);
    if (!json) {
        printf("Error parsing JSON\n");
        return -1;
    }

    cJSON *broker = cJSON_GetObjectItem(json, "mqttUrl");
    cJSON *broker_BKP = cJSON_GetObjectItem(json, "mqttUrl_BKP");
    cJSON *username = cJSON_GetObjectItem(json, "username");
    cJSON *password = cJSON_GetObjectItem(json, "password");

    if (!broker || !username || !password) {
        printf("Missing broker/username/password in JSON\n");
        cJSON_Delete(json);
        return -1;
    }

    snprintf(MQTT_MAIN_HOST, sizeof(MQTT_MAIN_HOST), "%s", broker->valuestring);
    /* Fix #4: broker_BKP may be missing — default to main host */
    if (broker_BKP && broker_BKP->valuestring) {
        snprintf(MQTT_BACKUP_HOST, sizeof(MQTT_BACKUP_HOST), "%s", broker_BKP->valuestring);
    } else {
        snprintf(MQTT_BACKUP_HOST, sizeof(MQTT_BACKUP_HOST), "%s", broker->valuestring);
        printf("Warning: mqttUrl_BKP not found in config, using main broker as backup\n");
    }
    snprintf(USERNAME, sizeof(USERNAME), "%s", username->valuestring);
    snprintf(PASSWORD, sizeof(PASSWORD), "%s", password->valuestring);

    cJSON_Delete(json);
    return 0;
}

int init_device(char *sn, size_t size) {
    if (execute_command("cat /mny/mtd/ipc/BurnUID", sn, size) != 0) {
        printf("Failed to execute command to get NK_SNnumber\n");
        return -1;
    }

    // FIX: remove newline from sn (actual buffer)
    size_t len = strlen(sn);
    if (len > 0 && sn[len - 1] == '\n') {
        sn[len - 1] = '\0';
    }

    snprintf(MQTT_CLIENT_ID, sizeof(MQTT_CLIENT_ID), "%s", sn);
    snprintf(MQTT_TOPIC_RECEIVE, sizeof(MQTT_TOPIC_RECEIVE), "torque/rx/%s/", sn);
    snprintf(MQTT_TOPIC_SEND, sizeof(MQTT_TOPIC_SEND), "torque/tx/%s/", sn);

    if (load_mqtt_config(STREM_CONFIG) != 0) {
        printf("Failed to load MQTT config.\n");
        return -1;
    }

    return 0;
}

void send_websocket_frame(int sockfd, unsigned char *data, size_t length) {
    unsigned char frame[1024];
    int frame_length = 0;
    
    // WebSocket frame structure
    frame[frame_length++] = 0x82; // Final frame, text frame
    if (length <= 125) {
        frame[frame_length++] = length;  // Payload length (7-bit field)
    } else {
        // Handling larger payload length (we will ignore the 16-bit and 64-bit extension for simplicity)
        frame[frame_length++] = 126;
        frame[frame_length++] = (length >> 8) & 0xFF;
        frame[frame_length++] = length & 0xFF;
    }

    /* Fix #8: Bounds check before memcpy to prevent stack overflow */
    if ((size_t)frame_length + length > sizeof(frame)) {
        fprintf(stderr, "WebSocket frame too large (%zu bytes), dropping\n", length);
        return;
    }

    // Copy the data to the frame buffer
    memcpy(frame + frame_length, data, length);
    frame_length += length;

    // Send the WebSocket frame
    if (send(sockfd, frame, frame_length, 0) < 0) {
        perror("Failed to send WebSocket frame");
    }
}

void stream_mqtt_audio(int sockfd, unsigned char *audio_data, size_t length) {
    if (length > 0) {
        send_websocket_frame(sockfd, audio_data, length);
        printf("Sent real-time MQTT audio chunk of size %zu bytes\n", length);
    }
}

int perform_handshake(int sockfd) {
    const char *handshake_request = 
        "GET /cgi-bin/Chat HTTP/1.1\r\n"
        "Host: 192.168.1.70\r\n"
        "Connection: Upgrade\r\n"
        "Upgrade: websocket\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n";

    char response[ABUFFER_SIZE] = {0};

    // Send handshake request
    if (send(sockfd, handshake_request, strlen(handshake_request), 0) < 0) {
        perror("Failed to send handshake request");
        return -1;
    }

    // Receive handshake response
    if (recv(sockfd, response, sizeof(response) - 1, 0) < 0) {
        perror("Failed to receive handshake response");
        return -1;
    }

    // Validate response (simple validation here, more checks may be needed)
    if (strstr(response, "101 Switching Protocols") == NULL) {
        fprintf(stderr, "Handshake failed: %s\n", response);
        return -1;
    }

    printf("Handshake successful:\n%s\n", response);
    return 0;
}

int P2P_Kill() {
    FILE *fpipe;
    const char *command = "ps | grep P2Pambicam | grep -v grep";  // Exclude grep process itself
    char line[256];  // Buffer to store each line of output
    char pid[10];    // Buffer to store the PID

    // Open the pipe to execute the command
    if (NULL == (fpipe = popen(command, "r"))) {
        perror("popen() failed.");
        return -1;
    }

    // Read the output of the command line by line
    while (fgets(line, sizeof(line), fpipe)) {
        printf("Process Found: %s", line);  // Print the line for debugging

        // Extract the PID (assuming PID is the first value on the line)
        sscanf(line, "%9s", pid);  /* Fix #18: Limit to pid buffer size */
        printf("Killing PID: %s\n", pid);  // Print the PID for debugging

        // Formulate the kill command with the PID
        char kill_command[20];
        snprintf(kill_command, sizeof(kill_command), "kill -9 %s", pid);

        // Execute the kill command
        system(kill_command);
        printf("Process with PID %s killed.\n", pid);
    }

    pclose(fpipe);  // Close the pipe
    return 0;
}

void send_PTZ_request(const char *act) {
    int sockfd;
    struct sockaddr_in server_addr;
    char buffer[1024] = {0};
    char http_request[2048] = {0};

    // Step 1: Create a socket
    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Socket creation failed");
        return;
    }  

    // Step 2: Define the server address
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(80);
    if (inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr) <= 0) {
        perror("Invalid address or address not supported");
        close(sockfd);
        return;
    }

    // Step 3: Connect to the server
    if (connect(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Connection failed");
        close(sockfd);
        return;
    }

    // Step 4: Formulate the dynamic HTTP PUT request with dynamic auth
    char auth_header[256];
    get_cam_auth_basic(auth_header, sizeof(auth_header));
    snprintf(http_request, sizeof(http_request),
        "PUT /cgi-bin/hi3510/ptzctrl.cgi?-step=0&-act=%s&-speed=6&-presetNUM=1 HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "User-Agent: Mozilla/5.0 (X11; Ubuntu; Linux x86_64; rv:132.0) Gecko/20100101 Firefox/132.0\r\n"
        "Accept: */*\r\n"
        "Authorization: %s\r\n"
        "X-Requested-With: XMLHttpRequest\r\n"
        "Origin: http://127.0.0.1\r\n"
        "Connection: keep-alive\r\n"
        "Referer: http://127.0.0.1/view.html\r\n"
        "Content-Length: 0\r\n\r\n", act, auth_header);

    // Step 5: Send the request
    if (send(sockfd, http_request, strlen(http_request), 0) < 0) {
        perror("Send failed");
        close(sockfd);
        return;
    }

    // Step 6: Receive the response
    int n = recv(sockfd, buffer, sizeof(buffer) - 1, 0);
    if (n > 0) {
        buffer[n] = '\0';  // Null-terminate the response
        printf("Server Response:\n%s\n", buffer);
    } else {
        perror("Receive failed");
    }

    // Step 7: Close the socket
    close(sockfd);

}

float get_cpu_usage() {
    pthread_mutex_lock(&cpu_mutex);  // Protect shared variables
    FILE *fp;
    char buffer[256];
    unsigned long user, nice, system, idle, iowait, irq, softirq, steal;
    static unsigned long prev_idle = 0, prev_total = 0;
   
    fp = fopen("/proc/stat", "r");
    if (fp == NULL) {
        perror("Failed to open /proc/stat");
        pthread_mutex_unlock(&cpu_mutex);
        return -1;
    }
   
    if (fgets(buffer, sizeof(buffer), fp) == NULL) {
        perror("Failed to read CPU data");
        fclose(fp);
        pthread_mutex_unlock(&cpu_mutex);
        return -1;
    }
    fclose(fp);

    sscanf(buffer, "cpu %lu %lu %lu %lu %lu %lu %lu %lu",
           &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal);

    unsigned long idle_time = idle + iowait;
    unsigned long total_time = user + nice + system + idle + iowait + irq + softirq + steal;

    /* Fix #10: Prevent division by zero on first call or rapid calls */
    if (total_time - prev_total == 0) {
        prev_idle = idle_time;
        prev_total = total_time;
        pthread_mutex_unlock(&cpu_mutex);
        return 0.0f;
    }

    float cpu_usage = (1.0f - (float)(idle_time - prev_idle) / (total_time - prev_total)) * 100.0;
   
    prev_idle = idle_time;
    prev_total = total_time;
   
    pthread_mutex_unlock(&cpu_mutex);
    return cpu_usage;
}

void send_https_request_case40(const char *ip, int port, const char *url, const char *method, const char *data) {
    int sockfd;
    struct sockaddr_in server_addr;
    char request[BUFFER_SIZE];
    int bytes_sent, bytes_received;

    // 1. Create socket
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("Socket creation failed");
        return;
    }

    // 2. Configure server address
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &server_addr.sin_addr) <= 0) {
        perror("Invalid IP address");
        close(sockfd);
        return;
    }

    // 3. Connect to the server
    if (connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Connection failed");
        close(sockfd);
        return;
    }

    // 4. Construct HTTP request with dynamic auth
    char auth_hdr[256];
    get_cam_auth_basic(auth_hdr, sizeof(auth_hdr));
    snprintf(request, sizeof(request),
             "%s %s HTTP/1.1\r\n"
             "Host: %s\r\n"
             "User-Agent: CustomClient/1.0\r\n"
             "Content-Type: application/x-www-form-urlencoded\r\n"
             "Authorization: %s\r\n"
             "Content-Length: %lu\r\n"
             "Connection: close\r\n"
             "\r\n"
             "%s",
             method, url, ip, auth_hdr, (unsigned long int)strlen(data), data);

    printf("Sending HTTP request:\n%s\n", request);

    // 5. Send the request
    bytes_sent = send(sockfd, request, strlen(request), 0);
    if (bytes_sent < 0) {
        perror("Send failed");
        close(sockfd);
        return;
    }

    // 6. Receive the response
    memset(response, 0, sizeof(response));
    bytes_received = recv(sockfd, response, sizeof(response) - 1, 0);
    if (bytes_received < 0) {
        perror("Receive failed");
    } else {
        printf("Response received:\n%s\n", response);
    }

    // 7. Close the socket
    close(sockfd);
}

void handle_case_0() {
    printf("Case 0: Publishing message to broker every 30 seconds...\n");
    // Get system information in JSON format
    const char* system_info = get_system_info();
    if (system_info == NULL) {
        printf("Failed to retrieve system info\n");
        return;
    }
    printf("Publishing system info: %s\n", system_info);
    // Your message payload logic
    const char* case_0_message = "Keep Alive!";
    printf("Publishing: %s\n", case_0_message);
    publish_mqtt_message(client, response, 0);
}

void handle_case_2() {
    send_https_request("/netsdk/video/encode/channel/101", "GET", "", response, sizeof(response));
    // Ensure the response is properly null-terminated
    response[sizeof(response) - 1] = '\0';
    sanitize_response(response);
    printf("JSON Response: %s\n", response);
    publish_mqtt_message(client, response, 2);
}

void handle_case_6(){
    printf("FUNCTION:- %s\n\n",__func__);
    send_https_request("/netsdk/video/input/channel/1", "GET", "", response, sizeof(response));
    response[sizeof(response) - 1] = '\0';
    printf("Raw HTTP Response: %s\n", response);
    sanitize_response(response);
    publish_mqtt_message(client, response, 6);
}

void handle_case_38(){
    printf("FUNCTION:- %s\n\n",__func__);
    send_https_request("/netsdk/image", "GET", "", response, sizeof(response));
    response[sizeof(response) - 1] = '\0';
    printf("Raw HTTP Response: %s\n", response);
    sanitize_response(response);
    publish_mqtt_message(client, response, 38);
}

void handle_case_39(){
    printf("FUNCTION:- %s\n\n",__func__);
    send_https_request("/netsdk/video/encode/channel/102", "GET", "", response, sizeof(response));
    response[sizeof(response) - 1] = '\0';
    printf("Raw HTTP Response: %s\n", response);
    sanitize_response(response);
    publish_mqtt_message(client, response, 39);
}

void handle_case_23(){
    printf("FUNCTION:- %s\n\n",__func__);
    send_https_request("/netsdk/system/time/ntp", "PUT", "{\"ntpEnabled\": true, \"ntpServerDomain\": \"pool.ntp.org\"}", response, sizeof(response));
    response[sizeof(response) - 1] = '\0';
    printf("Raw HTTP Response: %s\n", response);
    sanitize_response(response);
    publish_mqtt_message(client, response, 23);
}

const char* get_system_info() {
    const char *Tempreature = "/sys/class/thermal/thermal_zone0/temp";
    char buffer[20];
    static char result_buffer[256];  // Buffer for storing the JSON output

    // Open and read the temperature file
    FILE *fp = fopen(Tempreature, "r");
    if (fp == NULL) {
        perror("Failed to open temperature file");
        return NULL;
    }

    if (fgets(buffer, sizeof(buffer), fp) == NULL) {
        perror("Failed to read temperature");
        fclose(fp);
        return NULL;
    }
    fclose(fp);

    int temp_millicelsius = atoi(buffer);
    float temperature = (float)(temp_millicelsius / 1000.0);
    printf("Temperature: %.2f°C\n", temperature);

    // Get SD card statistics
    const char *sd_path = "/mnt/tf";
    struct statvfs sd_stat;
    if (statvfs(sd_path, &sd_stat) != 0) {
        perror("Failed to get SD card filesystem statistics");
        return NULL;
    }

    // Calculate SD card usage (sizes in MB)
    unsigned long total_size = (sd_stat.f_blocks * sd_stat.f_frsize) / (1024 * 1024);  // Total size in MB
    unsigned long free_size = (sd_stat.f_bfree * sd_stat.f_frsize) / (1024 * 1024);    // Free size in MB
    unsigned long used_size = total_size - free_size;                                  // Used size in MB
    double used_percentage = (100.0 * used_size) / total_size;                         // Used percentage

    // Get CPU usage
    float cpu_usage = get_cpu_usage();
    if (cpu_usage < 0) {
        return NULL;
    }

    // Create a cJSON object for system information
    cJSON *root_obj = cJSON_CreateObject();

    // Add temperature to the JSON object
    cJSON_AddNumberToObject(root_obj, "Temperature_C", temperature);

    // Create and add SD_Card information as a nested object
    cJSON *sd_card_obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(sd_card_obj, "Total", total_size);
    cJSON_AddNumberToObject(sd_card_obj, "Used", used_size);
    cJSON_AddNumberToObject(sd_card_obj, "Free", free_size);
    cJSON_AddNumberToObject(sd_card_obj, "Used_Percentage", used_percentage);

    // Add the SD_Card object to the root cJSON object
    cJSON_AddItemToObject(root_obj, "SD_Card", sd_card_obj);

    // Add CPU usage to the root cJSON object
    cJSON_AddNumberToObject(root_obj, "CPU", cpu_usage);

    /* Fix #3: cJSON_PrintUnformatted returns malloc'd memory — must free it */
    char *json_str = cJSON_PrintUnformatted(root_obj);
    if (json_str) {
        snprintf(result_buffer, sizeof(result_buffer), "%s", json_str);
        free(json_str);
    } else {
        snprintf(result_buffer, sizeof(result_buffer), "{}");
    }

    // Free the cJSON object memory
    cJSON_Delete(root_obj);

    // Return the result buffer containing the JSON string
    return result_buffer;
}

const char* load_config() {
    FILE *file = fopen(STREM_CONFIG, "r");
    if (file == NULL) {
        printf("Unable to open config.json\n");
        return NULL;
    }

    fseek(file, 0, SEEK_END);
    long fsize = ftell(file);
    fseek(file, 0, SEEK_SET);

    //char *config_data = malloc(fsize + 1);
    char *config_data = malloc((size_t)fsize + 1);
    if (config_data == NULL) {
        printf("Memory allocation failed\n");
        fclose(file);
        return NULL;
    }

    //fread(config_data, 1, fsize, file);
    fread(config_data, 1, (size_t)fsize, file);
    fclose(file);

    config_data[fsize] = '\0';
    return config_data;
}

void sanitize_response(char *response) {
    size_t len = strlen(response);
    for (size_t i = 0; i < len; i++) {
        // Replace non-printable characters with a space
        if ((unsigned char)response[i] < 32 || (unsigned char)response[i] > 126) {
            response[i] = ' ';
        }
    }
}

void save_file_from_message(const char *path, MQTTClient_message *message) {
    if (message == NULL || message->payload == NULL) {
        printf("Error: Invalid message or payload\n");
        return;
    }

    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        printf("Failed to open file: %s\n", path);
        return;
    }

    //size_t written = fwrite(message->payload, 1, message->payloadlen, file);
    size_t written = fwrite(message->payload, 1, (size_t)message->payloadlen, file);
    if (written != (size_t)message->payloadlen) {
        printf("Failed to write entire payload to file\n");
    } else {
        printf("Received and saved file: %s\n", path);
    }

    fclose(file);
}

void publish_mqtt_message(MQTTClient client, const char* message, int caseValue) {
    if (!client || !message) {
        printf("Error: Invalid client or message in publish_mqtt_message\n");
        return;
    }
   
    MQTTClient_message pubmsg = MQTTClient_message_initializer;
    pubmsg.payload = (void *)message;
    pubmsg.payloadlen = (int)strlen(message);
    pubmsg.qos = 0;
    pubmsg.retained = 0;

    char publishTopic[256];
    snprintf(publishTopic, sizeof(publishTopic), "%s%d", MQTT_TOPIC_SEND, caseValue);

    pthread_mutex_lock(&mqtt_mutex);
    int rc = MQTTClient_publishMessage(client, publishTopic, &pubmsg, NULL);
    pthread_mutex_unlock(&mqtt_mutex);
   
    if (rc != MQTTCLIENT_SUCCESS) {
        printf("Failed to publish message, return code %d\n", rc);
    } else {
        printf("Published message to topic: %s", publishTopic);
    }
}

int messageArrived(void *context, char *topicName, int topicLen, MQTTClient_message *message) {
    char *caseValue = topicName + strlen(MQTT_TOPIC_RECEIVE);
    MQTTClient client = (MQTTClient)context;

    /* Fix #5: Null-terminate the MQTT payload safely.
       MQTT payloads are binary — not guaranteed to have \0.
       Allocate a safe copy for all string operations in this function. */
    char *safePayload = (char *)malloc((size_t)message->payloadlen + 1);
    if (!safePayload) {
        fprintf(stderr, "Failed to allocate memory for payload\n");
        MQTTClient_freeMessage(&message);
        MQTTClient_free(topicName);
        return 1;
    }
    memcpy(safePayload, message->payload, (size_t)message->payloadlen);
    safePayload[message->payloadlen] = '\0';

    printf("Raw topicName: %s\n", topicName);
    printf("Raw payload: %s\n", safePayload);

    if (caseValue && strlen(caseValue) > 0) {
        printf("Received message on %s: %s\n", caseValue, safePayload);
    } else {
        printf("Error: Unable to parse caseValue from topicName.\n");
    }

    switch (atoi(caseValue)) {
        case 0: {
            printf("Handling case 0\n");
            // Determine the current status from the message payload
            const char *current_status = strstr(safePayload, "status: online") ? "online" : "offline";
            printf("current_status: %s\n", current_status);

            /* Fix #15: Use fixed buffer instead of strdup/free — thread safe */
            static char last_status[16] = "";
            static int offline_handled = 0;

            if (strcmp(last_status, current_status) != 0) {
                // Update the last known status
                strncpy(last_status, current_status, sizeof(last_status) - 1);
                last_status[sizeof(last_status) - 1] = '\0';

                if (strcmp(current_status, "offline") == 0 && !offline_handled) {
                    printf("Status changed to offline. Killing P2Pambicam process...\n");

                    offline_handled = 1;

                    //stop(KILL -9 P2Pambicam) p2p process if running......
                    P2P_Kill();
                    sleep(2);

                    get_ini_files();
                    sleep(2);

                    // Restart the P2Pambicam process
                    printf("Restarting P2Pambicam process...\n");
                    char command[256];
                    snprintf(command, sizeof(command), "/mny/mtd/ipc/ambicam/P2Pambicam -c /mny/mtd/ipc/ambicam/P2Pambicam_min.ini -f -d 7 &");

                    sleep (2);
                    int ret = system(command);
                    if (ret == -1) {
                        perror("Error executing restart command");
                    } else if (WIFEXITED(ret) && WEXITSTATUS(ret) != 0) {
                        printf("Restart command exited with non-zero status: %d\n", WEXITSTATUS(ret));
                    } else {
                        printf("P2Pambicam process restarted successfully.\n");
                    }
                } else if (strcmp(current_status, "online") == 0) {
                    offline_handled = 0;
                    printf("Status changed to online. Running P2Pambicam process...\n");
                }
            } else {
                printf("Status unchanged: %s\n", current_status);
            }
            break;  
        }
        case 1:{
            printf("Handling case 1\n");
            save_file_from_message(SAVE_PATH, message);
            inifile_COUNT++;
            if(inifile_COUNT > 1) {
                //P2P_Kill();
                printf("Wait for P2Pambicam to restart!\nmax count :-%d\n",inifile_COUNT);
            }
            break;
        }
        case 2: {
            printf("Handling case 2\n");
            send_https_request("/netsdk/video/encode/channel/101", "GET", "", response, sizeof(response));
            response[sizeof(response) - 1] = '\0';
            printf("Raw HTTP Response: %s\n", response);
            sanitize_response(response);
            publish_mqtt_message(client, response, atoi(caseValue));
            break;
        }
        case 3: {
            printf("Handling case 3\n");  
            char *response2="{\"Data\": \"not supported in your product\"}";
            publish_mqtt_message(client, response2, atoi(caseValue));
            break;
        }
        case 4: {
            printf("Handling case 4\n");
            send_https_request("/netsdk/Network/Interface/1", "GET", "", response, sizeof(response));
            response[sizeof(response) - 1] = '\0';
            printf("Raw HTTP Response: %s\n", response);  
            sanitize_response(response);
            publish_mqtt_message(client, response, atoi(caseValue));
            break;
        }
        case 5: {
            printf("Handling case 5\n");
            send_https_request("/netsdk/Network/Dns", "GET", "", response, sizeof(response));
            response[sizeof(response) - 1] = '\0';
            printf("Raw HTTP Response: %s\n", response);
            sanitize_response(response);
            publish_mqtt_message(client, response, atoi(caseValue));
            break;
        }
        case 6: {
            printf("Handling case 6\n");
            send_https_request("/netsdk/video/input/channel/1", "GET", "", response, sizeof(response));
            response[sizeof(response) - 1] = '\0';
            printf("Raw HTTP Response: %s\n", response);
            sanitize_response(response);
            publish_mqtt_message(client, response, atoi(caseValue));
            break;
        }
        case 7: {
            printf("Handling case 7\n");
            send_https_request("/netsdk/System/deviceInfo/macAddress", "GET", "", response, sizeof(response));
            response[sizeof(response) - 1] = '\0';
            printf("Raw HTTP Response: %s\n", response);
            sanitize_response(response);
            publish_mqtt_message(client, response, atoi(caseValue));
            break;
        }
        case 8: {
            printf("Handling case 8\n");
            send_https_request("/netsdk/system/deviceinfo", "GET", "", response, sizeof(response));
            response[sizeof(response) - 1] = '\0';
            printf("Raw HTTP Response: %s\n", response);
            sanitize_response(response);
            publish_mqtt_message(client, response, atoi(caseValue));
            break;
        }
        case 9: {
            printf("Handling case 9\n");
            send_https_request("/NetSDK/System/time/localTime", "GET", "", response, sizeof(response));
            response[sizeof(response) - 1] = '\0';
            printf("Raw HTTP Response: %s\n", response);
            sanitize_response(response);
            publish_mqtt_message(client, response, atoi(caseValue));
            break;
        }
        case 10: {
            printf("Handling case 10\n");
            send_https_request("/NetSDK/Video/motionDetection/channel/1", "GET", "", response, sizeof(response));
            response[sizeof(response) - 1] = '\0';
            printf("Raw HTTP Response: %s\n", response);
            sanitize_response(response);
            publish_mqtt_message(client, response, atoi(caseValue));
            break;
        }
        case 11: {
            printf("Handling case 11\n");
            send_https_request("/NetSdk/V2/Alarm", "GET", "", response, sizeof(response));
            response[sizeof(response) - 1] = '\0';
            printf("Raw HTTP Response: %s\n", response);
            sanitize_response(response);
            publish_mqtt_message(client, response, atoi(caseValue));
            break;
        }
        case 12: {
            printf("Handling case 3\n");  
            char *response2="{\"Data\": \"not supported in your product\"}";
            publish_mqtt_message(client, response2, atoi(caseValue));
            break;
        }
        case 13: {
            printf("Handling case 3\n");  
            char *response2="{\"Data\": \"not supported in your product\"}";
            publish_mqtt_message(client, response2, atoi(caseValue));
            break;
        }
        case 14:{
            printf("Handling case 3\n");  
            char *response2="{\"Data\": \"not supported in your product\"}";
            publish_mqtt_message(client, response2, atoi(caseValue));
            break;
        }
        case 15: {
            printf("Handling case 3\n");  
            char *response2="{\"Data\": \"not supported in your product\"}";
            publish_mqtt_message(client, response2, atoi(caseValue));
            break;
        }
        case 16: {
            printf("Handling case 3\n");  
            char *response2="{\"Data\": \"not supported in your product\"}";
            publish_mqtt_message(client, response2, atoi(caseValue));
            break;
        }
        case 17: {
            printf("Handling case 3\n");  
            char *response2="{\"Data\": \"not supported in your product\"}";
            publish_mqtt_message(client, response2, atoi(caseValue));
            break;
        }
        case 18: {
            printf("Handling case 3\n");  
            char *response2="{\"Data\": \"not supported in your product\"}";
            publish_mqtt_message(client, response2, atoi(caseValue));
            break;
        }
        case 19: {
            printf("Handling case 19\n");
            send_https_request("/netsdk/video/encode/channel/101", "PUT", safePayload, response, sizeof(response));
            response[sizeof(response) - 1] = '\0';  
            printf("Raw HTTP Response: %s\n", response);
            sanitize_response(response);
            publish_mqtt_message(client, response, atoi(caseValue));
            break;
        }
        case 20:{
            printf("Handling case 20\n");
            send_https_request("/netsdk/video/input/channel/1", "PUT", safePayload, response, sizeof(response));
            response[sizeof(response) - 1] = '\0';
            printf("Raw HTTP Response: %s\n", response);
            sanitize_response(response);
            publish_mqtt_message(client, response, atoi(caseValue));
            break;
        }
        case 21:{
            printf("Handling case 21\n");
            send_https_request("/netsdk/image", "PUT", safePayload, response, sizeof(response));
            response[sizeof(response) - 1] = '\0';
            printf("Raw HTTP Response: %s\n", response);
            sanitize_response(response);
            publish_mqtt_message(client, response, atoi(caseValue));
            break;
        }
        case 22:{
            printf("Handling case 22\n");
            char *response2="{\"Data\": \"not supported in your product\"}";
            publish_mqtt_message(client, response2, atoi(caseValue));
            break;
        }
        case 23:{
            printf("Handling case 23\n");
            send_https_request("/netsdk/system/time/ntp", "PUT", safePayload, response, sizeof(response));
            response[sizeof(response) - 1] = '\0';
            printf("Raw HTTP Response: %s\n", response);
            sanitize_response(response);
            publish_mqtt_message(client, response, atoi(caseValue));
            break;
        }
        case 24:{
            printf("Handling case 24\n");
            send_https_request("/NetSDK/Video/motionDetection/channel/1", "PUT", safePayload, response, sizeof(response));
            response[sizeof(response) - 1] = '\0';
            printf("Raw HTTP Response: %s\n", response);
            sanitize_response(response);
            publish_mqtt_message(client, response, atoi(caseValue));
            break;
        }
        case 25:{
            printf("Handling case 25\n");
            send_https_request("/NetSdk/V2/Alarm", "PUT", safePayload, response, sizeof(response));
            response[sizeof(response) - 1] = '\0';
            printf("Raw HTTP Response: %s\n", response);
            sanitize_response(response);
            publish_mqtt_message(client, response, atoi(caseValue));
            break;
        }
        case 26:{
            printf("Handling case 26\n");
            send_https_request("/NetSdk/V2/Image/DWDR", "PUT", safePayload, response, sizeof(response));
            response[sizeof(response) - 1] = '\0';
            printf("Raw HTTP Response: %s\n", response);
            sanitize_response(response);
            publish_mqtt_message(client, response, atoi(caseValue));
            break;
        }
        case 27:{
            printf("Handling case 27\n");
            send_https_request("/NetSDK/Video/humanDetect/", "PUT", safePayload, response, sizeof(response));
            response[sizeof(response) - 1] = '\0';
            printf("Raw HTTP Response: %s\n", response);
            sanitize_response(response);
            publish_mqtt_message(client, response, atoi(caseValue));
            break;
        }
        case 28:{
            printf("Handling case 28\n");
            send_https_request("/NetSdk/V2/AI/FaceDetect", "PUT", safePayload, response, sizeof(response));
            response[sizeof(response) - 1] = '\0';
            printf("Raw HTTP Response: %s\n", response);
            sanitize_response(response);
            publish_mqtt_message(client, response, atoi(caseValue));
            break;
        }
        case 29:{
            printf("Handling case 29\n");
            send_https_request("/NetSdk/V2/AI/LineCrossDetect", "PUT", safePayload, response, sizeof(response));
            response[sizeof(response) - 1] = '\0';
            printf("Raw HTTP Response: %s\n", response);
            sanitize_response(response);
            publish_mqtt_message(client, response, atoi(caseValue));
            break;
        }
        case 30:{
            printf("Handling case 30\n");
            send_https_request("/NetSdk/V2/AI/HumanCounter", "PUT", safePayload, response, sizeof(response));
            response[sizeof(response) - 1] = '\0';
            printf("Raw HTTP Response: %s\n", response);
            sanitize_response(response);
            publish_mqtt_message(client, response, atoi(caseValue));
            break;
        }
        case 31:{
            printf("Handling case 31\n");
            send_https_request("/NetSDK/V2/AI/RegionDetect", "PUT", safePayload, response, sizeof(response));
            response[sizeof(response) - 1] = '\0';
            printf("Raw HTTP Response: %s\n", response);
            sanitize_response(response);
            publish_mqtt_message(client, response, atoi(caseValue));
            break;
        }
        case 32: {
            printf("Handling case 32\n");
            send_https_request("/NetSDK/V2/AI/UnattendedObjDetect", "PUT", safePayload, response, sizeof(response));
            response[sizeof(response) - 1] = '\0';
            printf("Raw HTTP Response: %s\n", response);
            sanitize_response(response);
            publish_mqtt_message(client, response, atoi(caseValue));
            break;
        }
        case 33:{
            printf("Handling case 33\n");
            send_https_request("/NetSDK/V2/AI/ObjRemoveDetect", "PUT", safePayload, response, sizeof(response));
            response[sizeof(response) - 1] = '\0';
            printf("Raw HTTP Response: %s\n", response);
            sanitize_response(response);
            publish_mqtt_message(client, response, atoi(caseValue));
            break;
        }

        case 34:{
            printf("Handling case 34\n");
            char *response2="{\"Data\": \"not supported in your product\"}";
            publish_mqtt_message(client, response2, atoi(caseValue));
            break;
        }

        case 35: {
            printf("Handling case 35\n");
            const char* response2 = load_config();
            if (response2 == NULL) {
                printf("Failed to load config.json\n");
                break;  
            }
            publish_mqtt_message(client, response2, atoi(caseValue));
            free((void*)response2);
            break;
        }
        case 36: {
            printf("Handling case 36\n");
            int caseValueInt = atoi(caseValue);
            const char* sdcard_path = "/mnt/tf";  
            if (access(sdcard_path, F_OK) == 0) {
                const char* response = "SD card is available, proceeding with download...";
                publish_mqtt_message(client, response, caseValueInt);
                printf("SD card is available, proceeding with download...\n");
            } else {
                const char* response = "Please insert SD card! SD card needed";
                publish_mqtt_message(client, response, caseValueInt);
                printf("Please insert SD card!\n");
                break;
            }

            char* url = safePayload;
            printf("Received URL: %s\n", url);

            /* Fix #11: Validate OTA URL to prevent command injection */
            if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
                printf("Invalid OTA URL: must start with http:// or https://\n");
                publish_mqtt_message(client, "Invalid OTA URL", caseValueInt);
                break;
            }
            if (strpbrk(url, ";|`$&(){}[]!") != NULL) {
                printf("Invalid OTA URL: contains shell metacharacters\n");
                publish_mqtt_message(client, "Invalid OTA URL", caseValueInt);
                break;
            }
            // =========================
                // 1) DOWNLOAD LIB TAR FIRST because not available in this device that's why we download tar command from cloud for extraction .tar file!!!!
                // =========================
            const char *lib_url = "http://prong.arcisai.io/protected/augentix/lib/tar";
            const char *lib_out = "/tmp/tar";
             if (access(lib_out, F_OK) == 0) {
                printf("Existing lib tar found. Removing %s...\n", lib_out);
                system("rm -f /tmp/tar");
            }
            char lib_wget_cmd[512];
                // Save as /tmp/augentix_lib.tar
                snprintf(lib_wget_cmd, sizeof(lib_wget_cmd), "wget -O %s %s", lib_out, lib_url);

                if (system(lib_wget_cmd) != 0) {
                    publish_mqtt_message(client, "Lib tar download failed", caseValueInt);
                    break;
                }

            printf("Lib tar downloaded successfully to %s\n", lib_out);
            publish_mqtt_message(client, "Lib tar download successful", caseValueInt);


            if (access("/tmp/Augentix.tar.gz", F_OK) == 0) {
                printf("Existing file found. Removing /tmp/Augentix.tar.gz...\n");
                system("rm -f /tmp/Augentix.tar.gz");
            }

            char wget_command[512];
            snprintf(wget_command, sizeof(wget_command), "wget -P /tmp/ %s", url);
            //snprintf(wget_command, sizeof(wget_command), "wget --user=REDACTED --password=REDACTED -P /tmp/ %s", url);
            if (system(wget_command) != 0) {
                const char* response = "Download failed";
                publish_mqtt_message(client, response, caseValueInt);
                break;
            }
           
            printf("File downloaded successfully to /tmp directory\n");

            publish_mqtt_message(client, "Download successful", caseValueInt);

            printf("Creating and executing shell script...\n");
            ////Need to change from here RAHUL../////
            FILE *script_file = fopen("/tmp/OTA.sh", "w");
            if (script_file == NULL) {
                printf("Failed to create shell script file.\n");
                publish_mqtt_message(client, "Shell script creation failed", caseValueInt);
                break;
            }

            // fprintf(script_file,
            //     "#!/bin/sh\n"
            //     "kill -9 /mny/mtd/ipc/ambicam/MQTT_vcamclient_Augentix"
            //     "kill -9 /mny/mtd/ipc/ambicam/P2Pambicam -c /etc/jffs2/ambicam/P2Pambicam_min.ini -f -d 7"
            //     "\n"
            //     "set -e\n"
            //     "\n"
            //     "echo \"[OTA] Start OTA update\"\n"
            //     "\n"
            //     "cd /tmp || exit 1\n"
            //     "\n"
            //     "echo \"[OTA] Extracting firmware\"\n"
            //     "chmod +x tar\n"
            //     "./tar -xvf Augentix.tar.gz > /tmp/tar_output.log 2>&1\n"
            //     "\n"
            //     "sleep 2\n"
            //     "\n"
            //     "echo \"[OTA] Listing /tmp\"\n"
            //     "ls -l /tmp\n"
            //     "\n"
            //     "if [ ! -d /tmp/AugenTix ]; then\n"
            //     "    echo \"[OTA][ERROR] /tmp/AugenTix not found\"\n"
            //     "    exit 1\n"
            //     "fi\n"
            //     "\n"
            //     "echo \"[OTA] Preparing ambicam directory\"\n"
            //     "rm -rf /tmp/ambicam\n"
            //     "mv /tmp/AugenTix /tmp/ambicam\n"
            //     "\n"
            //     "ls -l /tmp\n"
            //     "\n"
            //     "echo \"[OTA] Copy certificates\"\n"
            //     "mkdir -p /etc/jffs2/ambicam\n"
            //     "mv /tmp/ambicam/*.crt /etc/jffs2/ambicam 2>/dev/null || true\n"
            //     "mv /tmp/ambicam/client.key /etc/jffs2/ambicam 2>/dev/null || true\n"
            //     "\n"
            //     "echo \"[OTA] Copy certificates successfully\"\n"
            //     "\n"
            //     "rm -rf /mny/mtd/ipc/ambicam\n"
            //     "rm -f  /mny/mtd/ipc/ambicam.sh\n"
            //     "\n"
            //     "mkdir -p /mny/mtd/ipc/ambicam\n"
            //     "\n"
            //     "mv /tmp/ambicam/* /mny/mtd/ipc/ambicam/\n"
            //     "mv /mny/mtd/ipc/ambicam/ambicam.sh /mny/mtd/ipc/\n"
            //     "\n"
            //     "chmod +x /mny/mtd/ipc/ambicam/* || true\n"
            //     "chmod +x /mny/mtd/ipc/ambicam.sh || true\n"
            //     "\n"
            //     "echo \"[OTA] OTA update completed successfully\"\n"    
            // );

           fprintf(script_file,
                "#!/bin/sh\n"
                "\n"
                "# Stop running services before OTA\n"
                "pidof MQTT_vcamclient_Augentix >/dev/null && "
                "pidof MQTT_vcamclient_Augentix | xargs kill -9 || true\n"
                "\n"
                "pidof P2Pambicam >/dev/null && "
                "pidof P2Pambicam | xargs kill -9 || true\n"
                "\n"
                "sleep 1\n"
                "\n"
                "set -e\n"
                "\n"
                "echo \"[OTA] Start OTA update\"\n"
                "\n"
                "cd /tmp || exit 1\n"
                "\n"
                "echo \"[OTA] Extracting firmware\"\n"
                "chmod +x tar\n"
                "./tar -xvf Augentix.tar.gz > /tmp/tar_output.log 2>&1\n"
                "\n"
                "sleep 2\n"
                "\n"
                "echo \"[OTA] Listing /tmp\"\n"
                "ls -l /tmp\n"
                "\n"
                "if [ ! -d /tmp/AugenTix ]; then\n"
                "    echo \"[OTA][ERROR] /tmp/AugenTix not found\"\n"
                "    exit 1\n"
                "fi\n"
                "\n"
                "echo \"[OTA] Preparing ambicam directory\"\n"
                "rm -rf /tmp/ambicam\n"
                "mv /tmp/AugenTix /tmp/ambicam\n"
                "\n"
                "echo \"[OTA] Copy certificates\"\n"
                "mkdir -p /etc/jffs2/ambicam\n"
                "mv /tmp/ambicam/*.crt /etc/jffs2/ambicam 2>/dev/null || true\n"
                "mv /tmp/ambicam/client.key /etc/jffs2/ambicam 2>/dev/null || true\n"
                "\n"
                "echo \"[OTA] Copy certificates successfully\"\n"
                "\n"
                "rm -rf /mny/mtd/ipc/ambicam\n"
                "rm -f  /mny/mtd/ipc/ambicam.sh\n"
                "\n"
                "mkdir -p /mny/mtd/ipc/ambicam\n"
                "\n"
                "mv /tmp/ambicam/* /mny/mtd/ipc/ambicam/\n"
                "mv /mny/mtd/ipc/ambicam/ambicam.sh /mny/mtd/ipc/\n"
                "\n"
                "chmod +x /mny/mtd/ipc/ambicam/* || true\n"
                "chmod +x /mny/mtd/ipc/ambicam.sh || true\n"
                "\n"
                "echo \"[OTA] OTA update completed successfully\"\n"
                "reboot\n"
            );

            fclose(script_file);

            if (system("chmod +x /tmp/OTA.sh") != 0) {
                printf("Failed to make shell script executable.\n");
                publish_mqtt_message(client, "Shell script chmod failed", caseValueInt);
                break;
            }

            if (system("/tmp/OTA.sh") != 0) {
                publish_mqtt_message(client, "Shell script execution failed", caseValueInt);
                break;
            }
           
            printf("Shell script executed successfully.\n");

            printf("Rebooting the system...\n");
            if (system("reboot") != 0) {
                publish_mqtt_message(client, "Reboot failed", caseValueInt);
            }

            break;
        }
        case 37: {
            printf("Handling case 37\n");
            const char* response = get_system_info();
            if (response != NULL) {
                publish_mqtt_message(client, response, atoi(caseValue));
                printf("System info:\n%s\n", response);
            } else {
                printf("Failed to fetch system info.\n");
            }
            break;
        }
        case 38: {
            printf("Handling case 38\n");
            send_https_request("/netsdk/image", "GET", "", response, sizeof(response));
            response[sizeof(response) - 1] = '\0';
            printf("Raw HTTP Response: %s\n", response);
            sanitize_response(response);
            publish_mqtt_message(client, response, atoi(caseValue));
            break;
        }
        case 39: {
            printf("Handling case 39\n");
            send_https_request("/netsdk/video/encode/channel/102", "GET", "", response, sizeof(response));
            response[sizeof(response) - 1] = '\0';
            printf("Raw HTTP Response: %s\n", response);
            sanitize_response(response);
            publish_mqtt_message(client, response, atoi(caseValue));
            break;
        }
        case 40: {
            printf("Handling case 40\n");
            const char *ip = "127.0.0.1";
            int port = 80;
            const char *url = "/netsdk/video/encode/channel/102";
            const char *method = "PUT";

            send_https_request_case40(ip, port, url, method, safePayload);

            response[sizeof(response) - 1] = '\0';
            printf("Raw HTTP Response: %s\n", response);
            sanitize_response(response);
            publish_mqtt_message(client, response, atoi(caseValue));
            break;
        }
        case 41:{
            printf("Handling case 41\n");
            send_https_request("/netsdk/system/operation/reboot", "PUT", safePayload, response, sizeof(response));
            response[sizeof(response) - 1] = '\0';
            sanitize_response(response);
            printf("Raw HTTP Response: %s\n", response);
            publish_mqtt_message(client, response, atoi(caseValue));
            break;
        }
        case 42:{
            printf("Handling case 42\n");
            send_https_request("/netsdk/audio/encode/channel/101", "GET", safePayload, response, sizeof(response));
            response[sizeof(response) - 1] = '\0';
            printf("Raw HTTP Response: %s\n", response);
            sanitize_response(response);
            publish_mqtt_message(client, response, atoi(caseValue));
            break;
        }
        case 43:{
            printf("Handling case 43\n");
            send_https_request("/netsdk/audio/encode/channel/101", "PUT", safePayload, response, sizeof(response));
            response[sizeof(response) - 1] = '\0';
            sanitize_response(response);
            publish_mqtt_message(client, response, atoi(caseValue));
            break;
        }
        case 44:{
            printf("Handling case 44\n");  
            char *response2="{\"Data\": \"not supported in your product\"}";
            publish_mqtt_message(client, response2, atoi(caseValue));
            break;
        }
        case 45:{
            printf("Handling case 43\n");
            send_https_request("/NetSdk/V2/Protocol/CustomEventServer", "PUT", safePayload, response, sizeof(response));
            response[sizeof(response) - 1] = '\0';
            printf("Raw HTTP Response: %s\n", response);
            sanitize_response(response);
            publish_mqtt_message(client, response, atoi(caseValue));
            break;
        }
        case 46:{
            printf("Handling case 43\n");
            send_https_request("/NetSdk/V2/Protocol/CustomEventServer", "GET", safePayload, response, sizeof(response));
            response[sizeof(response) - 1] = '\0';
            printf("Raw HTTP Response: %s\n", response);
            sanitize_response(response);
            publish_mqtt_message(client, response, atoi(caseValue));
            break;
        }
        case 47:{
            printf("Handling case 43\n");
            send_https_request("/NetSdk/V2/Protocol/CustomEventServer", "PUT", safePayload, response, sizeof(response));
            response[sizeof(response) - 1] = '\0';
            printf("Raw HTTP Response: %s\n", response);
            sanitize_response(response);
            publish_mqtt_message(client, response, atoi(caseValue));
            break;
        }
        case 48:{
            printf("Handling case 48\n");

            size_t payloadSize = (size_t)message->payloadlen + 1;
            char *payloadBuffer = (char *)malloc(payloadSize);
            if (!payloadBuffer) {
                fprintf(stderr, "Failed to allocate memory for payload\n");
                break;
            }

            strncpy(payloadBuffer, safePayload, payloadSize - 1);
            payloadBuffer[payloadSize - 1] = '\0';
            send_https_request("/netsdk/R.SearchRecord", "PUT", payloadBuffer, response, sizeof(response));

            response[sizeof(response) - 1] = '\0';
            printf("Raw HTTP Response: %s\n", response);
            sanitize_response(response);
            publish_mqtt_message(client, response, atoi(caseValue));
            free(payloadBuffer);
            break;
        }
        case 49: {
            printf("Handling case 49\n");

            size_t payloadSize = (size_t)message->payloadlen + 1;
            char *payloadBuffer = (char *)malloc(payloadSize);
            if (!payloadBuffer) {
                fprintf(stderr, "Failed to allocate memory for payload\n");
                break;
            }

            strncpy(payloadBuffer, safePayload, payloadSize - 1);
            payloadBuffer[payloadSize - 1] = '\0';

            char username[50] = {0};
            char password[50] = {0};
            /* Fix #12: Limit sscanf field width to prevent buffer overflow */
            sscanf(payloadBuffer, "{\"username\":\"%49[^\"]\",\"password\":\"%49[^\"]\"}", username, password);

            free(payloadBuffer);

            char url[512] = {0};
            char cam_user[64], cam_pass[64];
            get_cam_credentials(cam_user, sizeof(cam_user), cam_pass, sizeof(cam_pass));
            snprintf(url, sizeof(url),
                    "/user/add_user.xml?username=%s&password=%s&content="
                    "<user><add_user name=\"%s\" password=\"%s\" admin=\"\" premit_live=\"yes\" premit_setting=\"\" premit_playback=\"\"/></user>&_=1730018160286",
                    cam_user, cam_pass, username, password);

            const char *response2 = "{\"Data\": \"User creat successfully!\"}";

            int httpResult = send_https_request(url, "GET", safePayload, response, sizeof(response));
            if (httpResult != 0) {
                fprintf(stderr, "HTTP request failed\n");
                break;
            }
            response[sizeof(response) - 1] = '\0';
            printf("Raw HTTP Response: %s\n", response);
            sanitize_response(response);
            publish_mqtt_message(client, response2, atoi(caseValue));
            break;
        }
        case 50: {
            printf("Handling case 50\n");

            size_t payloadSize = (size_t)message->payloadlen + 1;
            char *payloadBuffer = (char *)malloc(payloadSize);
            if (!payloadBuffer) {
                fprintf(stderr, "Failed to allocate memory for payload\n");
                break;
            }

            strncpy(payloadBuffer, safePayload, payloadSize - 1);
            payloadBuffer[payloadSize - 1] = '\0';

            char username[50] = {0};
            sscanf(payloadBuffer, "{\"username\":\"%49[^\"]\"}", username);

            free(payloadBuffer);

            char url[512] = {0};
            char cam_user[64], cam_pass[64];
            get_cam_credentials(cam_user, sizeof(cam_user), cam_pass, sizeof(cam_pass));
            snprintf(url, sizeof(url),
             "/user/del_user.xml?username=%s&password=%s&content="
             "<user><del_user name=\"%s\"/></user>&_=1730018160286",
             cam_user, cam_pass, username);

            char response[1024] = {0};

            int httpResult = send_https_request(url, "GET", safePayload, response, sizeof(response));
            if (httpResult != 0) {
                fprintf(stderr, "HTTP request failed\n");
                break;
            }

            const char *response2 = "{\"Data\": \"User Deleted Successfully!\"}";

            response[sizeof(response) - 1] = '\0';
            printf("Raw HTTP Response: %s\n", response);

            sanitize_response(response);

            publish_mqtt_message(client, response2, atoi(caseValue));
            break;
        }
        case 51: {
            printf("Handling case 51\n");
            size_t payloadSize = (size_t)message->payloadlen + 1;
            char *payloadBuffer = (char *)malloc(payloadSize);
            if (!payloadBuffer) {
                fprintf(stderr, "Failed to allocate memory for payload\n");
                break;
            }

            strncpy(payloadBuffer, safePayload, payloadSize - 1);
            payloadBuffer[payloadSize - 1] = '\0';
           
            if (send_HTTPSPTZ_request(payloadBuffer, response, sizeof(response)) == 0) {
                printf("Server Response:\n%s\n", response);
            } else {
                printf("Failed to send PTZ request.\n");
            }

            response[sizeof(response) - 1] = '\0';
            printf("Raw HTTP Response: %s\n", response);
            usleep(500000);  /* 0.5 seconds — slight movement then stop */
            send_PTZ_request("stop");
            sanitize_response(response);
            publish_mqtt_message(client, response, atoi(caseValue));
            free(payloadBuffer);
            break;
        }
        case 52:{
            printf("Handling case 52\n");
            send_https_request("/NetSDK/PTZ/channel/1/setup", "GET", "", response, sizeof(response));
            response[sizeof(response) - 1] = '\0';
            printf("Raw HTTP Response: %s\n", response);
            sanitize_response(response);
            publish_mqtt_message(client, response, atoi(caseValue));
            break;
        }
        case 53: {
            printf("Handling case 53\n");

            size_t payloadSize = (size_t)message->payloadlen + 1;
            char *payloadBuffer = (char *)malloc(payloadSize);
            if (!payloadBuffer) {
                fprintf(stderr, "Failed to allocate memory for payload\n");
                break;
            }

            strncpy(payloadBuffer, safePayload, payloadSize - 1);
            payloadBuffer[payloadSize - 1] = '\0';

            cJSON *root = cJSON_Parse(payloadBuffer);
            if (!root) {
                fprintf(stderr, "Error parsing JSON: %s\n", cJSON_GetErrorPtr());
                free(payloadBuffer);
                break;
            }

            cJSON *cruiseModeItem = cJSON_GetObjectItem(root, "cruiseMode");
            if (cJSON_IsString(cruiseModeItem)) {
                char cruiseMode[50];
                strncpy(cruiseMode, cruiseModeItem->valuestring, sizeof(cruiseMode) - 1);
                cruiseMode[sizeof(cruiseMode) - 1] = '\0';

                printf("Extracted cruiseMode: %s\n", cruiseMode);

                char secondUrl[200];
                snprintf(secondUrl, sizeof(secondUrl), "/NetSDK/PTZ/channel/1/Control?command=%s", cruiseMode);

                char response[1024];
                send_https_request("/NetSDK/PTZ/channel/1/setup", "PUT", payloadBuffer, response, sizeof(response));
                response[sizeof(response) - 1] = '\0';
                printf("First API Response: %s\n", response);

                publish_mqtt_message(client, response, atoi(caseValue));

                char secondApiResponse[1024];
                send_https_request(secondUrl, "PUT", "", secondApiResponse, sizeof(secondApiResponse));
                secondApiResponse[sizeof(secondApiResponse) - 1] = '\0';
                printf("Second API Response: %s\n", secondApiResponse);

            } else {
                fprintf(stderr, "cruiseMode is not a valid string or missing\n");
            }

            /* Fix #13: Always free root, not just in if branch */
            cJSON_Delete(root);
            free(payloadBuffer);
            break;
        }
        case 54:{
            printf("Handling case 54\n");
            send_https_request("/NetSdk/V2/Alarm", "GET", "", response, sizeof(response));
            response[sizeof(response) - 1] = '\0';
            printf("Raw HTTP Response: %s\n", response);

            sanitize_response(response);

            publish_mqtt_message(client, response, atoi(caseValue));
            break;
        }
        case 55:{
            printf("Handling case 55\n");

            size_t payloadSize = (size_t)message->payloadlen + 1;
            char *payloadBuffer = (char *)malloc(payloadSize);
            if (!payloadBuffer) {
                fprintf(stderr, "Failed to allocate memory for payload\n");
                break;
            }

            strncpy(payloadBuffer, safePayload, payloadSize - 1);
            payloadBuffer[payloadSize - 1] = '\0';
            send_https_request("/NetSdk/V2/Alarm", "PUT", safePayload, response, sizeof(response));
            response[sizeof(response) - 1] = '\0';
            printf("Raw HTTP Response: %s\n", response);

            sanitize_response(response);

            publish_mqtt_message(client, response, atoi(caseValue));
            free(payloadBuffer);  /* Fix #7: Free leaked payloadBuffer */
            break;
        }
        case 56: {
            static int sockfd = -1;

            // Open WebSocket connection if not already open
            if (sockfd == -1) {
                struct sockaddr_in server_addr;
                sockfd = socket(AF_INET, SOCK_STREAM, 0);
                if (sockfd < 0) {
                    perror("Socket creation failed");
                    break;
                }

                memset(&server_addr, 0, sizeof(server_addr));
                server_addr.sin_family = AF_INET;
                server_addr.sin_port = htons(SERVER_PORT);

                if (inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr) <= 0) {
                    perror("Invalid address");
                    close(sockfd);
                    sockfd = -1;
                    break;
                }

                if (connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
                    perror("Connection failed");
                    close(sockfd);
                    sockfd = -1;
                    break;
                }

                printf("WebSocket connection established.\n");

                if (perform_handshake(sockfd) < 0) {
                    close(sockfd);
                    sockfd = -1;
                    break;
                }
            }

            // Send received MQTT audio data directly to WebSocket
            send_websocket_frame(sockfd, (unsigned char *)message->payload, message->payloadlen);
            printf("Sent MQTT audio to WebSocket\n");

            // If stream ends, close WebSocket
            /* Fix #19: Use memcmp with length check for binary audio payload */
            if (message->payloadlen == 9 && memcmp(message->payload, "Streamend", 9) == 0) {
                close(sockfd);
                sockfd = -1;
                printf("Audio stream ended. WebSocket connection closed.\n");
            }
            break;
        }                            
        case 57: {        
            //copy the buzers sound in to the received_audio.wav
            system("cp /etc/jffs2/ambicam/alarm.wav /etc/jffs2/ambicam/received_audio.wav");
            //create audio commnad!
            const char *command = "/mny/mtd/ipc/ambicam/AU_TWOWAY";
            int ret = system(command);
            sleep(1);
            if (ret == -1) {    
                perror("Error executing command");
                break;
            } else {        
                printf("Command executed with return value: %d\n", ret);
            }
            break;
        }
        case 58:{
            printf("Handling case 58\n");
            send_https_request("/netsdk/system/time/ntp", "GET", "", response, sizeof(response));
            response[sizeof(response) - 1] = '\0';
            printf("Raw HTTP Response: %s\n", response);
            sanitize_response(response);
            publish_mqtt_message(client, response, atoi(caseValue));
            break;
        }
        case 59: {
            printf("Handling case 59\n");
            send_https_request("/NetSDK/System/time/timeZone", "PUT", safePayload, response, sizeof(response));
            response[sizeof(response) - 1] = '\0';  
            printf("Raw HTTP Response: %s\n", response);
            sanitize_response(response);
            publish_mqtt_message(client, response, atoi(caseValue));
            break;
        }
        case 60: {
            printf("Handling case 60\n");
            send_https_request("/NetSDK/System/time/localTime", "PUT", safePayload, response, sizeof(response));
            response[sizeof(response) - 1] = '\0';  
            printf("Raw HTTP Response: %s\n", response);
            sanitize_response(response);
            publish_mqtt_message(client, response, atoi(caseValue));
            break;
        }
        case 61: {
            printf("Handling case 61\n");
            send_https_request("/NetSDK/System/time/timeZone", "GET", safePayload, response, sizeof(response));
            response[sizeof(response) - 1] = '\0';  
            printf("Raw HTTP Response: %s\n", response);
            sanitize_response(response);
            publish_mqtt_message(client, response, atoi(caseValue));
            break;
        }
        case 62: {
            printf("Handling case 62\n");
            send_https_request("/NetAPI/SmartDetect/Human", "GET", "", response, sizeof(response));
            response[sizeof(response) - 1] = '\0';
            printf("Raw HTTP Response: %s\n", response);
            sanitize_response(response);
            publish_mqtt_message(client, response, atoi(caseValue));
            break;
        }
        case 63: {
            printf("Handling case 63\n");
            send_https_request("/NetAPI/SmartDetect/Human", "PUT", safePayload, response, sizeof(response));
            response[sizeof(response) - 1] = '\0';  
            printf("Raw HTTP Response: %s\n", response);
            sanitize_response(response);
            publish_mqtt_message(client, response, atoi(caseValue));
            break;
        }
        case 64: {
            printf("Handling case 64\n");
            send_https_request("/NetAPI/SmartDetect/Motion", "GET", "", response, sizeof(response));
            response[sizeof(response) - 1] = '\0';
            printf("Raw HTTP Response: %s\n", response);
            sanitize_response(response);
            publish_mqtt_message(client, response, atoi(caseValue));
            break;
        }
        case 65: {
            printf("Handling case 65\n");
            send_https_request("/NetAPI/SmartDetect/Motion", "PUT", safePayload, response, sizeof(response));
            response[sizeof(response) - 1] = '\0';  
            printf("Raw HTTP Response: %s\n", response);
            sanitize_response(response);
            publish_mqtt_message(client, response, atoi(caseValue));
            break;
        }
        case 66: {
            printf("Handling case 66\n");
            send_https_request("/NetAPI/SmartDetect/Tamper", "GET", "", response, sizeof(response));
            response[sizeof(response) - 1] = '\0';
            printf("Raw HTTP Response: %s\n", response);
            sanitize_response(response);
            publish_mqtt_message(client, response, atoi(caseValue));
            break;
        }
        case 67: {
            printf("Handling case 67\n");
            int Flag = send_https_request("/NetSDK/SDCard/format", "PUT", safePayload, response, sizeof(response));
            response[sizeof(response) - 1] = '\0';
            printf("Raw HTTP Response: %s\n", response);
            sanitize_response(response);
            if(Flag == 0){
                publish_mqtt_message(client, "{\"Data\": \"Format Successfully!\"}", atoi(caseValue));
            }
            break;
        }
        case 68: {
            printf("Handling case 68\n");
            send_https_request("/NetAPI/User/List", "GET", "", response, sizeof(response));
            response[sizeof(response) - 1] = '\0';
            printf("Raw HTTP Response: %s\n", response);
            sanitize_response(response);
            publish_mqtt_message(client, response, atoi(caseValue));
            break;
        }
        case 69: {
            printf("Handling case 69\n");
            send_https_request("/NetSDK/SDCard/status2", "GET", "", response, sizeof(response));
            response[sizeof(response) - 1] = '\0';
            printf("Raw HTTP Response: %s\n", response);
            sanitize_response(response);
            publish_mqtt_message(client, response, atoi(caseValue));
            break;
        }
        case 70: {
            printf("Handling case 70\n");

            cJSON *root = cJSON_Parse(safePayload);
            if (!root) {
                printf("JSON parse error\n");
                break;
            }

            cJSON *status = cJSON_GetObjectItem(root, "status");
            cJSON *act = cJSON_GetObjectItem(root, "act");
            cJSON *number = cJSON_GetObjectItem(root, "number");

            if (!cJSON_IsNumber(status) || !cJSON_IsString(act) || !cJSON_IsNumber(number)) {
                printf("Invalid JSON format\n");
                cJSON_Delete(root);
                break;
            }

            char url[256];

            snprintf(url, sizeof(url),
            "/cgi-bin/hi3510/preset.cgi?-status=%d&-act=%s&-number=%d",
            status->valueint,
            act->valuestring,
            number->valueint);

            printf("Request URL: %s\n", url);

            send_https_request(url, "PUT", "", response, sizeof(response));

            response[sizeof(response) - 1] = '\0';

            printf("Raw HTTP Response: %s\n", response);

            sanitize_response(response);

            publish_mqtt_message(client, response, atoi(caseValue));

            cJSON_Delete(root);

            break;
        }
        case 71:
        {
            printf("Handling case 71 : change password\n");

            size_t payloadSize = message->payloadlen + 1;
            char *payloadBuffer = malloc(payloadSize);
            if (!payloadBuffer) {
                printf("Memory allocation failed\n");
                break;
            }

            memcpy(payloadBuffer, message->payload, message->payloadlen);
            payloadBuffer[message->payloadlen] = '\0';

            cJSON *root = cJSON_Parse(payloadBuffer);
            if (!root) {
                printf("JSON parse error\n");
                free(payloadBuffer);
                break;
            }

            cJSON *oldPass = cJSON_GetObjectItem(root, "old_pass");
            cJSON *newPass = cJSON_GetObjectItem(root, "new_pass");

            if (!cJSON_IsString(oldPass) || !cJSON_IsString(newPass)) {
                printf("Invalid password format\n");
                cJSON_Delete(root);
                free(payloadBuffer);
                break;
            }

            char old_password[64];
            char new_password[64];

            strncpy(old_password, oldPass->valuestring, sizeof(old_password) - 1);
            strncpy(new_password, newPass->valuestring, sizeof(new_password) - 1);

            old_password[sizeof(old_password) - 1] = '\0';
            new_password[sizeof(new_password) - 1] = '\0';

            char xml_content[512];
            snprintf(xml_content, sizeof(xml_content),
                    "<user><set_pass old_pass=\"%s\" new_pass=\"%s\"/></user>",
                    old_password, new_password);

            CURL *curl = curl_easy_init();
            if (!curl) {
                printf("curl init failed\n");
                cJSON_Delete(root);
                free(payloadBuffer);
                break;
            }

            char *encoded_content = curl_easy_escape(curl, xml_content, 0);
            if (!encoded_content) {
                printf("URL encoding failed\n");
                curl_easy_cleanup(curl);
                cJSON_Delete(root);
                free(payloadBuffer);
                break;
            }

            char endpoint[1024];
            char cam_user[64], cam_pass[64];
            get_cam_credentials(cam_user, sizeof(cam_user), cam_pass, sizeof(cam_pass));
            snprintf(endpoint, sizeof(endpoint),
                    "/user/set_pass.xml?username=%s&password=%s&content=%s",
                    cam_user, cam_pass, encoded_content);

            curl_free(encoded_content);
            curl_easy_cleanup(curl);

            printf("API Endpoint: %s\n", endpoint);

            char response[1024] = {0};
            int ret = send_https_request(endpoint, "PUT", "", response, sizeof(response));

            printf("API Response: %s\n", response);

            if (ret == 0 && is_password_change_success(response)) {
                if (update_password(new_password) == 0) {
                    printf("Runtime password updated successfully\n");
                } else {
                    printf("Camera password changed, but local password update failed\n");
                }
            } else {
                printf("Password change failed on camera, local runtime password not updated\n");
            }

            publish_mqtt_message(client, response, atoi(caseValue));

            usleep(3000000); 
            cJSON_Delete(root);
            free(payloadBuffer);
            break;
        }
        case 72: {
            printf("Handling case 72\n");
            send_https_request("/NetAPI/SmartDetect/Tamper", "PUT", safePayload, response, sizeof(response));
            response[sizeof(response) - 1] = '\0';  
            printf("Raw HTTP Response: %s\n", response);
            sanitize_response(response);
            publish_mqtt_message(client, response, atoi(caseValue));
            break;
        }
        case 73: {
            printf("Handling case 73\n");
            send_https_request("/NetAPI/System", "GET", "", response, sizeof(response));
            response[sizeof(response) - 1] = '\0';
            printf("Raw HTTP Response: %s\n", response);
            sanitize_response(response);
            publish_mqtt_message(client, response, atoi(caseValue));
            break;
        }
        case 74: {
            printf("Handling case 74\n");
            send_https_request("/NetAPI/System", "PUT", safePayload, response, sizeof(response));
            response[sizeof(response) - 1] = '\0';  
            printf("Raw HTTP Response: %s\n", response);
            sanitize_response(response);
            publish_mqtt_message(client, response, atoi(caseValue));
            break;
        }
        case 75: {
            printf("Handling case 75\n");
            send_https_request("/NetSdk/Rtmp", "GET", "", response, sizeof(response));
            response[sizeof(response) - 1] = '\0';
            printf("Raw HTTP Response: %s\n", response);
            sanitize_response(response);
            publish_mqtt_message(client, response, atoi(caseValue));
            break;
        }
        case 76: {
            printf("Handling case 76\n");
            send_https_request("/NetSdk/Rtmp", "PUT", safePayload, response, sizeof(response));
            response[sizeof(response) - 1] = '\0';  
            printf("Raw HTTP Response: %s\n", response);
            sanitize_response(response);
            publish_mqtt_message(client, response, atoi(caseValue));
            break;
        }
        case 77: {
            printf("Handling case 77\n");
            send_https_request("/NetAPI/Protocol/RTSPServer", "GET", "", response, sizeof(response));
            response[sizeof(response) - 1] = '\0';
            printf("Raw HTTP Response: %s\n", response);
            sanitize_response(response);
            publish_mqtt_message(client, response, atoi(caseValue));
            break;
        }
        case 78: {
            printf("Handling case 78\n");
            send_https_request("/NetAPI/Protocol/RTSPServer", "PUT", safePayload, response, sizeof(response));
            response[sizeof(response) - 1] = '\0';  
            printf("Raw HTTP Response: %s\n", response);
            sanitize_response(response);
            publish_mqtt_message(client, response, atoi(caseValue));
            break;
        }
        case 80: {
            printf("Handling case 80: credential sync from server\n");
            cJSON *root = cJSON_Parse(safePayload);
            if (!root) {
                printf("Case 80: JSON parse error\n");
                break;
            }

            cJSON *user = cJSON_GetObjectItem(root, "username");
            cJSON *pass = cJSON_GetObjectItem(root, "password");

            pthread_mutex_lock(&cred_mutex);
            if (cJSON_IsString(user) && user->valuestring) {
                snprintf(CAM_USERNAME, sizeof(CAM_USERNAME), "%s", user->valuestring);
            }
            if (cJSON_IsString(pass) && pass->valuestring) {
                snprintf(CAM_PASSWORD, sizeof(CAM_PASSWORD), "%s", pass->valuestring);
            }
            rebuild_basic_auth();
            pthread_mutex_unlock(&cred_mutex);

            printf("Camera credentials synced: user=%s\n", CAM_USERNAME);
            cJSON_Delete(root);
            break;
        }
        case 81: {  // v1.13 — provider_srt.conf push from broker (see ops/mqtt-provisioning.md)
            printf("Handling case 81 (provider_srt.conf push)\n");
            save_file_from_message("/mny/mtd/ipc/ambicam/provider_srt.conf", message);
            /* Signal ambicam.sh that the conf landed.  Watchdog there picks
             * up the change and restarts provider_srt within MONITOR_INTERVAL. */
            system("touch /tmp/provider_srt_conf_pushed");
            break;
        }
        default:
            printf("Unhandled case %d\n", atoi(caseValue));
            break;
    }
    free(safePayload);  /* Fix #5: Free the safe payload copy */
    MQTTClient_freeMessage(&message);
    MQTTClient_free(topicName);
    return 1;
}

/* Fix #6: cause can be NULL from paho */
void onConnectionLost(void *context, char *cause) {
    printf("Connection lost: %s\n", cause ? cause : "unknown");
    // Attempt to reconnect and resubscribe only if reconnect is successful
    if (reconnect_and_subscribe() == 0) {
        printf("Reconnect failed, trying again later.\n");
    }
}

int reconnect_mqtt(void) {
    const char* uris[] = { MQTT_MAIN_HOST, MQTT_BACKUP_HOST };
    const int uri_count = 2;
    int rc;

    printf("Reconnecting process started...\n");

    for (;;) {  // keep trying until connected, then run loop until drop
        for (int i = 0; i < uri_count; ++i) {
            if (client != NULL) {
                MQTTClient_destroy(&client);
                client = NULL;
            }

            if (MQTTClient_create(&client, uris[i], MQTT_CLIENT_ID,
                                  MQTTCLIENT_PERSISTENCE_NONE, NULL) != MQTTCLIENT_SUCCESS) {
                printf("Failed to create client for %s\n", uris[i]);
                continue;
            }

            MQTTClient_connectOptions conn_opts = MQTTClient_connectOptions_initializer;
            conn_opts.username = USERNAME;
            conn_opts.password = PASSWORD;
            conn_opts.keepAliveInterval = 60;
            conn_opts.cleansession = 1;

            MQTTClient_setCallbacks(client, client, onConnectionLost, messageArrived, NULL);

            int fail_count = 0;  // <-- reset for this URI
            while ((rc = MQTTClient_connect(client, &conn_opts)) != MQTTCLIENT_SUCCESS) {
                printf("Failed to reconnect to %s, rc=%d. Retrying in %d seconds... (attempt %d/%d)\n",
                       uris[i], rc, RECONNECT_INTERVAL, fail_count + 1, MAX_RETRY);

                fail_count++;
                if (fail_count >= MAX_RETRY) {
                    printf("Max attempts reached for %s. Trying next broker...\n", uris[i]);
                    break;  // leave while; rc != SUCCESS -> try next URI
                }
                sleep(RECONNECT_INTERVAL);
            }

            if (rc == MQTTCLIENT_SUCCESS) {
                printf("Connected to broker: %s\n", uris[i]);

                 // mark where we are and keep CURRENT_HOST in sync
                using_backup = (strcmp(uris[i], MQTT_BACKUP_HOST) == 0) ? 1 : 0;
                strncpy(CURRENT_HOST, uris[i], sizeof(CURRENT_HOST) - 1);
                CURRENT_HOST[sizeof(CURRENT_HOST) - 1] = '\0';

                // re-subscribe *every* successful connect
                resubscribe_topics();

                get_ini_files();

                run_main_loop(); // returns when connection drops
                printf("Connection dropped from: %s. Reconnecting...\n", uris[i]);
                break; // break inner for-loop; outer for(;;) will restart attempts
            } else {
                // we broke out due to MAX_RETRY; try next URI
                printf("Connect to %s not successful. Switching to next broker...\n", uris[i]);
                // tiny pause to avoid hot-spinning
                sleep(1);
            }
        }
    }
    // never reached
    return 0;
}

int reconnect_and_subscribe(void) {
    if (reconnect_mqtt() == 0) {
        resubscribe_topics();
        return 1;
    }
    return 0;
}

int execute_command(const char *command, char *output_buffer, size_t buffer_size) {
    FILE *fp;
    size_t bytes_read;

    // Execute the command
    fp = popen(command, "r");
    if (fp == NULL) {
        return -1;  // Return error code if command execution fails
    }

    // Read the output into the buffer
    bytes_read = fread(output_buffer, 1, buffer_size - 1, fp);
    if (ferror(fp)) {
        pclose(fp);
        return -1;  // Return error code if reading fails
    }

    output_buffer[bytes_read] = '\0';  // Ensure null termination
    pclose(fp);
    return 0;  // Return success
}

void handle_case_34() {
    printf("Entering handle_case_34\n");
    // Send HTTP request
    int http_result = send_https_request("/Netsdk/Vmukti/DeviceInfo", "GET", " ", response, sizeof(response));
    if (http_result != 0) {
        printf("HTTP request failed with code: %d\n", http_result);
        return;
    }
    response[sizeof(response) - 1] = '\0';  // Ensure null-termination
    printf("Raw HTTP Response: %s\n", response);
    sanitize_response(response);  // Sanitize if needed
    publish_mqtt_message(client, response, 34);
}

void cameraname_Change(){
    printf("Function Name:- %s\n", __FUNCTION__);
    const char channle[100] = "{\"channelName\": \"ARCIS AI\"}";
    send_https_request("/netsdk/video/encode/channel/101", "PUT", channle, response, sizeof(response));
    response[sizeof(response) - 1] = '\0';  
    printf("Raw HTTP Response: %s\n", response);
    sanitize_response(response);
}

void get_ini_files(void){
    publish_mqtt_message(client, response, 1);
    system("cat /mny/mtd/ipc/ambicam/P2Pambicam_min.ini");
}

void run_startup_cases(void) {
    get_ini_files();
    cameraname_Change();
    handle_case_6();
    sleep(5);
    handle_case_38();
    sleep(5);
    handle_case_39();
    sleep(5);
    handle_case_2();
    sleep(5);
}

void subscribe_topics(const char *sn) {
    char subscribeTopic[256];
    snprintf(subscribeTopic, sizeof(subscribeTopic), "torque/rx/%s/#", sn);

    if (MQTTClient_subscribe(client, subscribeTopic, QOS) != MQTTCLIENT_SUCCESS) {
        printf("Failed to subscribe to topic: %s\n", subscribeTopic);
    }

    const char *global_topic = "torque/rx/all/#";
    if (MQTTClient_subscribe(client, global_topic, QOS) != MQTTCLIENT_SUCCESS) {
        printf("Failed to subscribe to global topic: %s\n", global_topic);
    } else {
        printf("Subscribed to global topic: %s\n", global_topic);
    }
}

int connect_broker(const char *host) {
    int rc, retries = 0;

    while ((rc = MQTTClient_connect(client, &conn_opts)) != MQTTCLIENT_SUCCESS) {
        printf("Failed to connect to %s, rc=%d (retry %d/%d)\n",
               host, rc, retries+1, MAX_RETRY);
        retries++;
        if (retries >= MAX_RETRY) {
            return -1; // fail after max retries
        }
        sleep(RECONNECT_INTERVAL);
    }

    printf("Connected to broker: %s\n", host);
    sleep(2);
    get_ini_files();
    return 0;
}

void resubscribe_topics(void) {
    printf("%s\n",__FUNCTION__);
    char subscribeTopic[256];
    snprintf(subscribeTopic, sizeof(subscribeTopic), "%s#", MQTT_TOPIC_RECEIVE);

    if (MQTTClient_subscribe(client, subscribeTopic, QOS) == MQTTCLIENT_SUCCESS) {
        printf("Subscribed to: %s\n", subscribeTopic);
    } else {
        printf("Failed to subscribe: %s\n", subscribeTopic);
    }

    const char *global_topic = "torque/rx/all/#";
    if (MQTTClient_subscribe(client, global_topic, QOS) == MQTTCLIENT_SUCCESS) {
        printf("Subscribed to: %s\n", global_topic);
    } else {
        printf("Failed to subscribe: %s\n", global_topic);
    }
}  

int init_mqtt_with_failover() {
    int rc;

    snprintf(CURRENT_HOST, sizeof(CURRENT_HOST), "%s", MQTT_MAIN_HOST);
    printf("Trying main broker: %s\n", CURRENT_HOST);

    // Create client
    MQTTClient_create(&client, CURRENT_HOST, MQTT_CLIENT_ID,
                      MQTTCLIENT_PERSISTENCE_NONE, NULL);

    conn_opts.username = USERNAME;
    conn_opts.password = PASSWORD;
    conn_opts.keepAliveInterval = 60;
    conn_opts.cleansession = 1;
    MQTTClient_setCallbacks(client, client, onConnectionLost, messageArrived, NULL);

    rc = connect_broker(CURRENT_HOST);

    if (rc != 0) {
        // Switch to backup
        printf("Main broker failed after %d retries. Switching to backup broker.\n", MAX_RETRY);
        MQTTClient_destroy(&client);

        snprintf(CURRENT_HOST, sizeof(CURRENT_HOST), "%s", MQTT_BACKUP_HOST);
        using_backup = 1;

        MQTTClient_create(&client, CURRENT_HOST, MQTT_CLIENT_ID,
                          MQTTCLIENT_PERSISTENCE_NONE, NULL);
        MQTTClient_setCallbacks(client, client, onConnectionLost, messageArrived, NULL);

        rc = connect_broker(CURRENT_HOST);
        if (rc != 0) {
            printf("Backup broker also failed.\n");
            return -1;
        }
    }

    return 0;
}

void cleanup_resources(void) {
    printf("Cleaning up resources...\n");
   
    // Disconnect MQTT
    if (client != NULL) {
        MQTTClient_disconnect(client, TIMEOUT);
        MQTTClient_destroy(&client);
        client = NULL;
    }
   
    // Destroy mutex
    pthread_mutex_destroy(&cpu_mutex);
}

void *monitor_main_broker(void *arg) {
    printf("%s\n",__FUNCTION__);
    while (1) {
        sleep(30);  // check every 30s

        if (using_backup) {
            printf("Currently on backup broker. Checking if main broker is available...\n");

            MQTTClient temp_client;
            MQTTClient_connectOptions temp_opts = MQTTClient_connectOptions_initializer;
            temp_opts.username = USERNAME;
            temp_opts.password = PASSWORD;
            temp_opts.keepAliveInterval = 30;
            temp_opts.cleansession = 1;

            // Try connect to main broker in temporary session
            if (MQTTClient_create(&temp_client, MQTT_MAIN_HOST,
                                  "temp_client", MQTTCLIENT_PERSISTENCE_NONE, NULL) == MQTTCLIENT_SUCCESS) {
                if (MQTTClient_connect(temp_client, &temp_opts) == MQTTCLIENT_SUCCESS) {
                    printf("Main broker is back online. Switching...\n");

                    // Disconnect backup
                    MQTTClient_disconnect(client, TIMEOUT);
                    MQTTClient_destroy(&client);

                    /* Fix #9 & #17: Use snprintf + remove duplicate connect_broker */
                    snprintf(CURRENT_HOST, sizeof(CURRENT_HOST), "%s", MQTT_MAIN_HOST);

                    using_backup = 0;
                    MQTTClient_create(&client, CURRENT_HOST, MQTT_CLIENT_ID,
                                      MQTTCLIENT_PERSISTENCE_NONE, NULL);
                    MQTTClient_setCallbacks(client, client, onConnectionLost, messageArrived, NULL);

                    // Connect to MAIN
                    if (connect_broker(CURRENT_HOST) == 0) {
                        // <<< CRITICAL: re-subscribe on the new connection >>>
                        resubscribe_topics();
                        sleep(2);
                        get_ini_files();
                    } else {
                        // If main failed right now, mark we’re still on backup
                        using_backup = 1;
                    }

                    MQTTClient_disconnect(temp_client, TIMEOUT);
                }
                MQTTClient_destroy(&temp_client);
            }
        }
    }
    return NULL;
}

void run_main_loop(void) {
    while (!shutdown_requested) {
        handle_case_34();
        sleep(60);
        handle_case_0();
        sleep(5);
    }
}

int main() {
    char NK_SNnumber[20];
    struct sigaction sa;
    // Setup signal handlers
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Fix #2: Initialize curl once globally — not per-request */
    curl_global_init(CURL_GLOBAL_DEFAULT);

    // 1. Init device and load config (SN, MQTT URLs, creds, etc.)
    if (init_device(NK_SNnumber, sizeof(NK_SNnumber)) != 0) {
        return EXIT_FAILURE;
    }

    // 2. Connect with failover logic (main → backup)
    if (init_mqtt_with_failover() != 0) {
        printf("Failed to connect to both brokers. Exiting.\n");
        return EXIT_FAILURE;
    }

    // 3. Subscribe topics
    subscribe_topics(NK_SNnumber);

    // 4. Start monitor thread (to check if main broker comes back)
    pthread_t monitor_tid;
    if (pthread_create(&monitor_tid, NULL, monitor_main_broker, NULL) != 0) {
        perror("Failed to create monitor thread");
        return EXIT_FAILURE;
    }

    // 5. Run startup sequence
    run_startup_cases();

    // 6. Enter main loop
    run_main_loop();

    // ==== Cleanup ====
    printf("Disconnecting...\n");
    MQTTClient_disconnect(client, TIMEOUT);
    MQTTClient_destroy(&client);
    curl_global_cleanup();  /* Fix #2: Cleanup curl once at exit */

    return EXIT_SUCCESS;
}
