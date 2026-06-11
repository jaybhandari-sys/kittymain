/*
 * ap2p — stream_timing.c
 *
 * Per-viewer-session timing.  See stream_timing.h for design.
 */
#include "stream_timing.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "MQTTClient.h"

#define ST_PEER_ID_LEN 64

static pthread_mutex_t  g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct timespec  g_marks[ST__COUNT];     /* zeroed = unset */
static char             g_peer_id[ST_PEER_ID_LEN];
static int              g_active     = 0;        /* 1 = session in progress */
static int              g_published  = 0;        /* one-shot guard          */
static unsigned         g_session_id = 0;

static const char *g_names[ST__COUNT] = {
    "peer_requested",       "srt_handshake_begin",  "srt_handshake_done",
    "http_get_open",        "http_first_byte",      "first_flv_pushed",
};

static double ts_to_sec(const struct timespec *ts) {
    return ts->tv_sec + ts->tv_nsec / 1e9;
}

static void clock_now(struct timespec *out) {
    clock_gettime(CLOCK_MONOTONIC, out);
}

void st_session_begin(const char *peer_id) {
    pthread_mutex_lock(&g_lock);
    memset(g_marks, 0, sizeof(g_marks));
    g_active    = 1;
    g_published = 0;
    g_session_id++;
    if (peer_id && peer_id[0]) {
        strncpy(g_peer_id, peer_id, sizeof(g_peer_id) - 1);
        g_peer_id[sizeof(g_peer_id) - 1] = '\0';
    } else {
        g_peer_id[0] = '\0';
    }
    /* ST_PEER_REQUESTED is implicit-on-begin. */
    clock_now(&g_marks[ST_PEER_REQUESTED]);
    pthread_mutex_unlock(&g_lock);
}

void st_mark(st_milestone_t m) {
    if (m < 0 || m >= ST__COUNT) return;
    pthread_mutex_lock(&g_lock);
    if (g_active && g_marks[m].tv_sec == 0 && g_marks[m].tv_nsec == 0) {
        clock_now(&g_marks[m]);
    }
    pthread_mutex_unlock(&g_lock);
}

size_t st_format_summary(char *buf, size_t buflen, const char *node_id) {
    if (!buf || buflen < 64) return 0;
    if (!node_id) node_id = "";

    pthread_mutex_lock(&g_lock);
    double t0_sec = ts_to_sec(&g_marks[ST_PEER_REQUESTED]);

    int n = snprintf(buf, buflen,
        "{\"node_id\":\"%s\","
         "\"peer_id\":\"%s\","
         "\"session_id\":%u,"
         "\"version\":\"2.0.6\","
         "\"absolute_sec\":{",
        node_id, g_peer_id, g_session_id);

    int first = 1;
    for (int i = 0; i < ST__COUNT && n < (int)buflen - 48; i++) {
        double v = (g_marks[i].tv_sec || g_marks[i].tv_nsec)
                       ? ts_to_sec(&g_marks[i]) : -1.0;
        n += snprintf(buf + n, buflen - n, "%s\"%s\":%.6f",
                      first ? "" : ",", g_names[i], v);
        first = 0;
    }
    n += snprintf(buf + n, buflen - n,
                  "},\"since_peer_requested_sec\":{");
    first = 1;
    for (int i = 0; i < ST__COUNT && n < (int)buflen - 48; i++) {
        double v = (g_marks[i].tv_sec || g_marks[i].tv_nsec)
                       ? ts_to_sec(&g_marks[i]) : -1.0;
        double d = v < 0 ? -1.0 : v - t0_sec;
        n += snprintf(buf + n, buflen - n, "%s\"%s\":%.6f",
                      first ? "" : ",", g_names[i], d);
        first = 0;
    }
    n += snprintf(buf + n, buflen - n, "}}");
    pthread_mutex_unlock(&g_lock);

    return (size_t)n;
}

void st_publish_summary(void *mqtt_client, const char *node_id) {
    pthread_mutex_lock(&g_lock);
    if (!g_active || g_published) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    g_published = 1;
    pthread_mutex_unlock(&g_lock);

    char json[2048];
    size_t n = st_format_summary(json, sizeof(json), node_id);

    fprintf(stderr, "[ap2p:stream-timing] %.*s\n", (int)n, json);
    fflush(stderr);

    if (!mqtt_client || !node_id || !node_id[0] || n == 0) return;

    char topic[160];
    snprintf(topic, sizeof(topic), "torque/tx/%s/stream", node_id);

    MQTTClient_message msg = MQTTClient_message_initializer;
    msg.payload    = json;
    msg.payloadlen = (int)n;
    msg.qos        = 1;
    msg.retained   = 1;

    MQTTClient_deliveryToken token;
    int rc = MQTTClient_publishMessage((MQTTClient)mqtt_client, topic, &msg, &token);
    if (rc != MQTTCLIENT_SUCCESS) {
        fprintf(stderr, "[ap2p:stream-timing] publish rc=%d\n", rc);
        fflush(stderr);
    }
}
