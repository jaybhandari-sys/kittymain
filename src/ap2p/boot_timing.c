/*
 * ap2p — boot_timing.c
 *
 * See boot_timing.h for the model.  This file is intentionally tiny —
 * a static array of milestone timestamps plus one publish-summary call.
 */
#include "boot_timing.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "MQTTClient.h"

static pthread_mutex_t g_lock        = PTHREAD_MUTEX_INITIALIZER;
static double          g_marks[BT__COUNT] = {0};     /* uptime seconds */
static double          g_ambicam_t0  = -1.0;          /* lazy-loaded */
static int             g_published   = 0;             /* one-shot guard */

static const char *g_names[BT__COUNT] = {
    "ap2p_main",        "config_loaded",   "mqtt_connecting",
    "mqtt_connected",   "subscribed",      "case81_rx",
    "state_ready",      "srt_started",     "edge_srflx",
    "ctrl_registered",
};

/* Read /proc/uptime — first field is kernel uptime in seconds (float). */
double bt_uptime_now_sec(void)
{
    FILE *fp = fopen("/proc/uptime", "r");
    if (!fp) return 0.0;
    double up = 0.0;
    if (fscanf(fp, "%lf", &up) != 1) up = 0.0;
    fclose(fp);
    return up;
}

/* Read /tmp/boot_t0 (written by ambicam.sh).  Sets g_ambicam_t0 in seconds
 * (kernel uptime at launcher start) or leaves it at -1 if the file is
 * unavailable / unparseable. */
static void load_ambicam_t0_unlocked(void)
{
    if (g_ambicam_t0 >= 0.0) return;
    FILE *fp = fopen("/tmp/boot_t0", "r");
    if (!fp) { g_ambicam_t0 = 0.0; return; }
    double v = 0.0;
    if (fscanf(fp, "%lf", &v) != 1) v = 0.0;
    fclose(fp);
    g_ambicam_t0 = v;
}

void bt_mark(bt_milestone_t m)
{
    if (m < 0 || m >= BT__COUNT) return;
    pthread_mutex_lock(&g_lock);
    load_ambicam_t0_unlocked();
    if (g_marks[m] == 0.0) {
        g_marks[m] = bt_uptime_now_sec();
    }
    pthread_mutex_unlock(&g_lock);
}

/* Read /mny/mtd/ipc/ambicam/VERSION (set by CI per-tag) so the boot JSON
 * reports the ACTUAL kitty version on this camera, not a stale literal.
 * Falls back to "unknown" if the file is missing or unreadable.
 *
 * Static cache: read once on first call, reuse on subsequent boot-summary
 * publishes (the VERSION file doesn't change without a reboot). */
static const char *bt_version_string(void)
{
    static char cached[32] = "";
    if (cached[0]) return cached;
    FILE *fp = fopen("/mny/mtd/ipc/ambicam/VERSION", "r");
    if (!fp) { strncpy(cached, "unknown", sizeof(cached) - 1); return cached; }
    if (fgets(cached, sizeof(cached), fp) == NULL) {
        strncpy(cached, "unknown", sizeof(cached) - 1);
    } else {
        /* Strip trailing newline/whitespace. */
        size_t L = strlen(cached);
        while (L > 0 && (cached[L-1] == '\n' || cached[L-1] == '\r' ||
                          cached[L-1] == ' '  || cached[L-1] == '\t')) {
            cached[--L] = '\0';
        }
        if (L == 0) strncpy(cached, "unknown", sizeof(cached) - 1);
    }
    fclose(fp);
    return cached;
}

/* Render summary. */
size_t bt_format_summary(char *buf, size_t buflen, const char *node_id)
{
    if (!buf || buflen < 64) return 0;
    if (!node_id) node_id = "";

    pthread_mutex_lock(&g_lock);
    load_ambicam_t0_unlocked();
    double t0 = g_ambicam_t0 > 0.0 ? g_ambicam_t0 : 0.0;

    int n = snprintf(buf, buflen,
        "{\"node_id\":\"%s\","
         "\"version\":\"%s\","
         "\"ambicam_start_uptime_sec\":%.3f,"
         "\"now_uptime_sec\":%.3f,"
         "\"since_ambicam_start_sec\":{",
        node_id, bt_version_string(), t0, bt_uptime_now_sec());

    int first = 1;
    for (int i = 0; i < BT__COUNT && n < (int)buflen - 32; i++) {
        double v = g_marks[i];
        double d = (v > 0.0 && t0 > 0.0) ? (v - t0) : -1.0;
        n += snprintf(buf + n, buflen - n, "%s\"%s\":%.3f",
                      first ? "" : ",", g_names[i], d);
        first = 0;
    }
    n += snprintf(buf + n, buflen - n, "},\"uptime_at_milestone_sec\":{");
    first = 1;
    for (int i = 0; i < BT__COUNT && n < (int)buflen - 32; i++) {
        n += snprintf(buf + n, buflen - n, "%s\"%s\":%.3f",
                      first ? "" : ",", g_names[i], g_marks[i]);
        first = 0;
    }
    n += snprintf(buf + n, buflen - n, "}}");
    pthread_mutex_unlock(&g_lock);

    return (size_t)n;
}

void bt_publish_summary(void *mqtt_client, const char *node_id)
{
    pthread_mutex_lock(&g_lock);
    if (g_published) { pthread_mutex_unlock(&g_lock); return; }
    g_published = 1;
    pthread_mutex_unlock(&g_lock);

    char json[2048];
    size_t n = bt_format_summary(json, sizeof(json), node_id);

    /* Log to stderr regardless — gives an on-camera trace via ap2p.log
     * even when broker connectivity is flaky and the publish drops. */
    fprintf(stderr, "[ap2p:boot-timing] %.*s\n", (int)n, json);
    fflush(stderr);

    if (!mqtt_client || !node_id || !node_id[0] || n == 0) return;

    char topic[160];
    snprintf(topic, sizeof(topic), "torque/tx/%s/boot", node_id);

    MQTTClient_message msg = MQTTClient_message_initializer;
    msg.payload    = json;
    msg.payloadlen = (int)n;
    msg.qos        = 1;
    msg.retained   = 1;

    MQTTClient_deliveryToken token;
    int rc = MQTTClient_publishMessage((MQTTClient)mqtt_client, topic, &msg, &token);
    if (rc != MQTTCLIENT_SUCCESS) {
        fprintf(stderr, "[ap2p:boot-timing] publish rc=%d\n", rc);
        fflush(stderr);
    }
}
