#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

#define BASE_URL "http://localhost"
#define CA_CERT_FILE "camera_cert.crt"
#define SERVER_CRT_FILE "server_cert.crt"

/* Dynamic camera credentials from HTTP.c (received via MQTT case 80) */
extern char CAM_USERNAME[64];
extern char CAM_PASSWORD[64];
extern void get_cam_credentials(char *user, size_t user_size, char *pass, size_t pass_size);
#define API_1 "/netsdk/System/deviceInfo/macAddress"
#define API_2 "/Netsdk/Vmukti/DeviceInfo"

/* Fix #1: Safe write callback with bounds checking */
typedef struct {
    char *buffer;
    size_t size;    /* total buffer capacity */
    size_t used;    /* bytes already written */
} write_ctx_t;

size_t write_callback_1(void *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t total_size = size * nmemb;
    write_ctx_t *ctx = (write_ctx_t *)userdata;

    /* Calculate remaining space (leave room for null terminator) */
    size_t remaining = ctx->size - ctx->used - 1;
    size_t to_copy = (total_size < remaining) ? total_size : remaining;

    if (to_copy > 0) {
        memcpy(ctx->buffer + ctx->used, ptr, to_copy);
        ctx->used += to_copy;
        ctx->buffer[ctx->used] = '\0';
    }

    /* Return total_size to tell curl all data was consumed */
    return total_size;
}

/* Fix #2: Removed curl_global_init/cleanup from here — call once in main() */
int send_https_request(const char *endpoint, const char *method,
                       const char *data, char *response, size_t response_size)
{
    CURL *curl;
    CURLcode res;
    char url[256];

    snprintf(url, sizeof(url), "%s%s", BASE_URL, endpoint);
    printf("URL: %s\n", url);

    curl = curl_easy_init();
    if (!curl) {
        printf("curl init failed\n");
        return -1;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);

    /* Use dynamic camera credentials (thread-safe copy) */
    char local_user[64], local_pass[64];
    get_cam_credentials(local_user, sizeof(local_user), local_pass, sizeof(local_pass));
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_ANY);
    curl_easy_setopt(curl, CURLOPT_USERNAME, local_user);
    curl_easy_setopt(curl, CURLOPT_PASSWORD, local_pass);

    if (strcmp(method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data);
    } else if (strcmp(method, "PUT") == 0) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data);
    } else {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    }

    /* Set up safe write context */
    memset(response, 0, response_size);
    write_ctx_t ctx = { .buffer = response, .size = response_size, .used = 0 };
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback_1);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);

    curl_easy_setopt(curl, CURLOPT_VERBOSE, 0L);

    res = curl_easy_perform(curl);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    printf("HTTP Status Code: %ld\n", http_code);

    curl_easy_cleanup(curl);

    return (res == CURLE_OK) ? 0 : -1;
}

int send_HTTPSPTZ_request(const char *act, char *response, size_t response_size) {
    char endpoint[256];

    /* step=0 + timed stop. speed=6 for responsive movement */
    snprintf(endpoint, sizeof(endpoint),
             "/cgi-bin/hi3510/ptzctrl.cgi?-step=0&-act=%s&-speed=5&-presetNUM=1", act);

    return send_https_request(endpoint, "PUT", "", response, response_size);
}
