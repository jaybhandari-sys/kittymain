/*
 * ap2p — mqtt_thread.c
 *
 * Phase A port of src/mqtt-vcamclient/HTTP.c into the v2.0 single-binary
 * scaffold.  Replaces HTTP.c's main() with mqtt_thread_main(state_arg).
 *
 * Responsibilities:
 *   1. Connect to the MQTT broker (bootstrap_mqtt_url / user / pass loaded
 *      by main.c from config.json BEFORE this thread starts).
 *   2. Subscribe to torque/rx/<NODE_ID>/# — the broker auto-delivers the
 *      retained case-81 payload immediately on subscribe.
 *   3. On every case-81 delivery, call apply_ap2p_conf_payload(state, ...)
 *      which parses KEY=value, populates the SRT-side fields, and signals
 *      the SRT thread to start (first time) or to rebuild (subsequent).
 *   4. Other broker-side cases (1/6/25/34/38/39/...) — these handlers are
 *      not yet ported in this phase; see the Phase A residual TODO list in
 *      the report.  They are non-critical to the ap2p P2P pipeline and can
 *      be merged across in a follow-up.
 *
 * Brand-leak guard:  no SIGNALING/STUN/TURN/provider_srt literals; the only
 * conf access is via state->* fields; no sentinel files written.  The old
 * `system("touch /tmp/provider_srt_conf_pushed")` line is gone — in-process
 * signaling replaces it.
 */

#define _GNU_SOURCE
#include "state.h"
#include "boot_timing.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>

#include "MQTTClient.h"
#include "legacy_cases.h"

#define RECONNECT_INTERVAL 5      /* steady-state retry sleep (seconds) — initial */
#define RECONNECT_BACKOFF_CAP 60  /* v2.0.7-rc4 — exponential backoff cap (s)    */
#define TIMEOUT_MS         10000L
#define QOS                0
#define MAX_RETRY          5

/* v2.0.5 fast-boot path: during the first FAST_BOOT_WINDOW_SEC seconds of
 * process lifetime, retry MQTT connect every FAST_BOOT_RETRY_MS milliseconds
 * instead of waiting RECONNECT_INTERVAL seconds.  Critical because the
 * camera's wlan0/DHCP frequently isn't routable yet when ap2p first runs;
 * v2.0.4 measurement showed 3–4 failed connects burning ~17 s of dead air.
 * Tight retry collapses that to <1 s in steady state. */
#define FAST_BOOT_WINDOW_SEC 30
#define FAST_BOOT_RETRY_MS   500

/* MQTT state lives at file scope (the C API of paho is callback-based and
 * needs stable pointers).  Each callback receives the shared ap2p_state_t
 * via the `context` arg passed at setCallbacks time. */
static MQTTClient g_client = NULL;

/* v2.0.7-rc4 — set by connection_lost callback (runs on paho's internal
 * thread); polled by the main-loop's idle wait so a broker disconnect is
 * detected within ~1 s instead of waiting for the next MQTTClient_isConnected
 * poll (which can lag the actual TCP-RST by tens of seconds).  Reset to 0
 * after each successful reconnect. */
static volatile int g_disconnect_signalled = 0;
static char g_topic_recv[80];    /* torque/rx/<NODE_ID>/ — case num appended on subscribe */
static char g_topic_send[80];    /* torque/tx/<NODE_ID>/ (reserved for case-0/6/... publishers) */

/* ------------------------------------------------------------------------ */
/*  Callbacks                                                               */
/* ------------------------------------------------------------------------ */

static int message_arrived(void *context, char *topicName, int topicLen,
                           MQTTClient_message *message) {
    (void)topicLen;
    ap2p_state_t *state = (ap2p_state_t *)context;

    /* Extract the case number — topic format is "<g_topic_recv><N>" e.g.
     * torque/rx/SN123/81 → "81".  Bail out on malformed topics. */
    size_t prefix_len = strlen(g_topic_recv);
    const char *case_str = (strncmp(topicName, g_topic_recv, prefix_len) == 0)
                         ? topicName + prefix_len : NULL;
    int case_num = case_str ? atoi(case_str) : -1;

    /* Defensive null-termination of the payload — MQTT payloads are binary
     * and not guaranteed to carry a trailing NUL. */
    char *payload_copy = (char *)malloc((size_t)message->payloadlen + 1);
    if (!payload_copy) {
        fprintf(stderr, "mqtt: payload alloc failed\n");
        MQTTClient_freeMessage(&message);
        MQTTClient_free(topicName);
        return 1;
    }
    memcpy(payload_copy, message->payload, (size_t)message->payloadlen);
    payload_copy[message->payloadlen] = '\0';

    switch (case_num) {
        case 90: {
            /* v2.0.7+ — runtime-config push moved here.  Previously was case 81,
             * but the ArcisAI Kitty QC Tool reserves case 81 for tampering
             * detect (matches v1.13.x semantics + the Augentix Update sheet).
             * Migrating retained config to a new dedicated case avoids the
             * collision.  Operators publish to torque/rx/<sn>/90 going forward;
             * ops/fleet-provision.sh + broker-provisioning-runbook.md updated.
             *
             * v1.x used to save_file_from_message() this to disk and `touch`
             * a sentinel to signal ambicam.sh.  v2.0 keeps everything in-
             * process: parse the bytes into the shared state struct; the
             * in-memory parser broadcasts state_ready_cv (first time) or
             * sets srt_reload_pending (subsequent).  No disk, sentinel,
             * subprocess. */
            printf("mqtt: case 90 (runtime config push, %d bytes)\n",
                   message->payloadlen);
            int rc = apply_ap2p_conf_payload(state, payload_copy,
                                             (size_t)message->payloadlen);
            if (rc != 0) {
                fprintf(stderr, "mqtt: case 90 payload rejected (missing CTRL_HOST?)\n");
            }
            break;
        }
        /* All non-90 cases route to the legacy handler bundle (verbatim copy
         * of v1.13.x HTTP.c — preserves 80+ case handlers).  The legacy
         * function does NOT free the message/topicName — caller owns them. */
        default:
            legacy_message_arrived(g_client, topicName, topicLen, message);
            break;
    }

    free(payload_copy);
    MQTTClient_freeMessage(&message);
    MQTTClient_free(topicName);
    return 1;
}

static void connection_lost(void *context, char *cause) {
    (void)context;
    fprintf(stderr, "mqtt: connection LOST: %s — signalling main loop to reconnect\n",
            cause ? cause : "unknown");
    fflush(stderr);
    /* v2.0.7-rc4 — set a flag that the main-loop poll picks up on its NEXT
     * tick (it polls every 1 s now, was 5 s).  We can't break the inner
     * loop directly from this callback (paho calls it on its own thread). */
    __atomic_store_n(&g_disconnect_signalled, 1, __ATOMIC_SEQ_CST);
}

/* ------------------------------------------------------------------------ */
/*  Subscribe helper                                                        */
/* ------------------------------------------------------------------------ */

static int subscribe_topics(void) {
    char wild[96];
    snprintf(wild, sizeof(wild), "%s#", g_topic_recv);
    int rc = MQTTClient_subscribe(g_client, wild, QOS);
    if (rc != MQTTCLIENT_SUCCESS) {
        fprintf(stderr, "mqtt: subscribe %s failed rc=%d\n", wild, rc);
        return -1;
    }
    bt_mark(BT_SUBSCRIBED);
    printf("mqtt: subscribed to %s\n", wild);
    /* Global broadcast topic — kept for parity with v1.x client. */
    const char *global = "torque/rx/all/#";
    if (MQTTClient_subscribe(g_client, global, QOS) == MQTTCLIENT_SUCCESS) {
        printf("mqtt: subscribed to %s\n", global);
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/*  Single connect attempt against a broker URI.  Returns 0 on success.     */
/* ------------------------------------------------------------------------ */

static int connect_once(ap2p_state_t *state, const char *uri) {
    if (g_client) {
        MQTTClient_destroy(&g_client);
        g_client = NULL;
    }
    if (MQTTClient_create(&g_client, uri, state->node_id,
                          MQTTCLIENT_PERSISTENCE_NONE, NULL) != MQTTCLIENT_SUCCESS) {
        fprintf(stderr, "mqtt: create client failed for %s\n", uri);
        return -1;
    }

    MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer;
    opts.username = state->bootstrap_mqtt_user;
    opts.password = state->bootstrap_mqtt_pass;
    /* v2.0.7-rc4 — 30 s keepalive (was 60).  paho detects a dead broker
     * after ~1.5 * keepAliveInterval; 30 s → disconnect noticed in ~45 s
     * (was 90 s).  Faster recovery from billing/network outages where
     * detection time was the dominant component of "camera looks offline". */
    opts.keepAliveInterval = 30;
    opts.cleansession = 1;
    /* Explicit connect timeout so a hung-half-open TCP doesn't stall us
     * for the libpaho default (30 s).  10 s is plenty on any reachable
     * network; if it fails we want to fall through to backoff fast. */
    opts.connectTimeout = 10;

    MQTTClient_setCallbacks(g_client, state,
                            connection_lost, message_arrived, NULL);

    bt_mark(BT_MQTT_CONNECTING);
    /* v2.0.5: timeline-based retry instead of attempt-count.
     *   - For first FAST_BOOT_WINDOW_SEC seconds since this call started,
     *     retry every FAST_BOOT_RETRY_MS ms (typically 0.5 s) — collapses
     *     the boot-time "broker not routable yet" stall to <1 s.
     *   - After the window expires, fall back to 5 s sleeps + 5-attempt
     *     cap so a genuinely-unreachable broker doesn't pin the CPU.
     * The fast window only runs ONCE per process: subsequent reconnects
     * (e.g., after a long-lived disconnect) use the slow cadence. */
    static int g_fast_window_used = 0;
    struct timespec t_start;
    clock_gettime(CLOCK_MONOTONIC, &t_start);
    int rc, attempt = 0;
    /* v2.0.7-rc4 — exponential backoff (was: fixed 5 s) + INFINITE retries
     * (was: MAX_RETRY=5 then return -1).  The previous code would give up
     * after ~25 s in steady mode; the outer loop would still retry, but
     * the unnecessary churn slowed reconnect AND if the OUTER loop hit
     * a bug we'd genuinely lose the camera forever.  Now we keep trying
     * with backoff capped at RECONNECT_BACKOFF_CAP (60 s) so a long
     * server outage results in steady retries at the cap, not a flood. */
    int backoff_s = 1;
    while ((rc = MQTTClient_connect(g_client, &opts)) != MQTTCLIENT_SUCCESS) {
        attempt++;
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (now.tv_sec - t_start.tv_sec) +
                         (now.tv_nsec - t_start.tv_nsec) / 1e9;
        int in_fast = (!g_fast_window_used && elapsed < FAST_BOOT_WINDOW_SEC);
        fprintf(stderr, "mqtt: connect %s failed rc=%d (attempt %d, t=%.1fs, %s)\n",
                uri, rc, attempt, elapsed, in_fast ? "fast" : "steady");
        if (state->shutdown_flag) return -1;
        if (in_fast) {
            struct timespec ts = { FAST_BOOT_RETRY_MS / 1000,
                                   (FAST_BOOT_RETRY_MS % 1000) * 1000000L };
            nanosleep(&ts, NULL);
        } else {
            /* Exponential backoff: 1, 2, 4, 8, 16, 32, 60, 60, ...  Caller
             * (mqtt_thread_main) detects success when MQTTClient_connect
             * returns SUCCESS; no upper bound on attempts. */
            sleep(backoff_s);
            backoff_s = (backoff_s * 2 > RECONNECT_BACKOFF_CAP)
                      ? RECONNECT_BACKOFF_CAP : backoff_s * 2;
        }
    }
    g_fast_window_used = 1;       /* burn the one-shot window on success */
    bt_mark(BT_MQTT_CONNECTED);
    printf("mqtt: connected to %s as %s\n", uri, state->node_id);
    /* Expose the client handle to other threads (boot_timing publishes from
     * SRT thread on first signaling-registered).  paho-mqtt-c is thread-safe
     * for concurrent publish/subscribe/connect calls on the same client. */
    state->mqtt_client_handle = (void *)g_client;
    /* Hand the live client + connection params to the legacy handler bundle
     * so its publish helpers (used by cases 0/1/2/6/34/38/39/...) can post
     * back via the same connection. */
    ap2p_legacy_init(g_client, state->node_id, uri,
                     state->bootstrap_mqtt_user, state->bootstrap_mqtt_pass);
    return 0;
}

/* ------------------------------------------------------------------------ */
/*  mqtt_thread entry point                                                 */
/* ------------------------------------------------------------------------ */

void *mqtt_thread_main(void *state_arg) {
    fprintf(stderr, "[ap2p:trace] mqtt_thread_main: entered\n"); fflush(stderr);
    ap2p_state_t *state = (ap2p_state_t *)state_arg;
    if (!state) return NULL;

    /* Bootstrap fields MUST have been populated by main.c before we spawn. */
    if (!state->bootstrap_mqtt_url[0] || !state->node_id[0]) {
        fprintf(stderr, "mqtt: missing bootstrap (mqtt_url / node_id) — exiting thread\n");
        return NULL;
    }

    snprintf(g_topic_recv, sizeof(g_topic_recv), "torque/rx/%s/", state->node_id);
    snprintf(g_topic_send, sizeof(g_topic_send), "torque/tx/%s/", state->node_id);

    /* Outer loop: keep trying the broker forever — every disconnect triggers
     * a fresh connect attempt.  Failover policy: prefer main; if main fails
     * (connect_once exhausted retries), try the backup URL if configured.
     * On the next iteration after a successful connection drops, start with
     * whichever URL last succeeded so a single bad broker doesn't bounce us
     * between hosts.  v3.0 will extend this with "drain back to main" once
     * we have observability for which broker is healthy. */
    const char *last_ok_url = NULL;          /* whichever URL last connected */
    while (!state->shutdown_flag) {
        const char *url_primary = last_ok_url ? last_ok_url
                                              : state->bootstrap_mqtt_url;
        const char *url_secondary =
            state->bootstrap_mqtt_url_bkp[0] && state->bootstrap_mqtt_url_bkp[0] != '\0'
                ? state->bootstrap_mqtt_url_bkp
                : NULL;
        /* If url_primary IS the backup (because that's what last worked),
         * url_secondary becomes the main; either way we alternate. */
        if (url_secondary && strcmp(url_secondary, url_primary) == 0) {
            url_secondary = state->bootstrap_mqtt_url;
        }

        fprintf(stderr, "mqtt: trying primary broker %s\n", url_primary);
        if (connect_once(state, url_primary) == 0) {
            last_ok_url = url_primary;
        } else if (url_secondary) {
            fprintf(stderr, "mqtt: primary failed; failing over to backup %s\n",
                    url_secondary);
            if (connect_once(state, url_secondary) != 0) {
                fprintf(stderr, "mqtt: both brokers failed; sleeping before retry\n");
                sleep(RECONNECT_INTERVAL);
                continue;
            }
            last_ok_url = url_secondary;
        } else {
            fprintf(stderr, "mqtt: connect exhausted retries; sleeping before retry\n");
            sleep(RECONNECT_INTERVAL);
            continue;
        }
        if (subscribe_topics() != 0) {
            MQTTClient_disconnect(g_client, TIMEOUT_MS);
            sleep(RECONNECT_INTERVAL);
            continue;
        }
        /* v2.0.7 — fire the legacy init pulse ONCE per process, after we've
         * subscribed and given the broker a moment to deliver retained config
         * (case 90).  This restores v1.13.x first-init behaviour that v2.x
         * had silently dropped: get_ini_files(), cameraname_Change(),
         * handle_case_6/38/39/2 (config publishes), handle_case_8 (device
         * info), handle_case_23 (NTP set), handle_case_59_ist (TZ set).
         * Detached thread so the main MQTT idle loop isn't blocked by the
         * inter-case sleeps. */
        static int g_startup_fired = 0;
        if (!g_startup_fired) {
            g_startup_fired = 1;
            pthread_t startup_tid;
            if (pthread_create(&startup_tid, NULL,
                               ap2p_legacy_startup_thread, state) == 0) {
                pthread_detach(startup_tid);
            } else {
                fprintf(stderr, "mqtt: WARN — startup thread create failed\n");
            }
            /* v2.0.7-rc3 — also spawn the periodic case-0 keepalive that
             * v1.13.x had via run_main_loop().  Restores the 30-s "Keep
             * Alive!" publish to torque/tx/<sn>/0 that every IoT-side
             * alerting + QC tool uses to tell whether a camera is online
             * RIGHT NOW (as opposed to "boot publish was retained at some
             * point earlier").  Detached; loops until shutdown_flag. */
            pthread_t keepalive_tid;
            if (pthread_create(&keepalive_tid, NULL,
                               ap2p_legacy_keepalive_thread, state) == 0) {
                pthread_detach(keepalive_tid);
                fprintf(stderr, "mqtt: case-0 keepalive thread spawned (30s cadence)\n");
            } else {
                fprintf(stderr, "mqtt: WARN — keepalive thread create failed\n");
            }
        }
        /* v2.0.7-rc4 — reset the disconnect-signalled flag on every
         * successful (re)connect so the inner loop starts clean. */
        __atomic_store_n(&g_disconnect_signalled, 0, __ATOMIC_SEQ_CST);

        /* Idle loop — paho dispatches incoming messages on its own
         * internal thread (via setCallbacks).  We just wait here for
         * disconnect or shutdown.
         *
         * Two ways to detect disconnect:
         *   1) connection_lost callback sets g_disconnect_signalled
         *      atomically — fires within milliseconds of paho noticing.
         *   2) MQTTClient_isConnected periodic poll — fallback in case
         *      the callback didn't fire (e.g. half-open TCP).
         * Poll cadence is 1 s (was 5 s) so the fallback path is also
         * snappy; the actual cost is minimal (one atomic load + one
         * libpaho call per second). */
        while (!state->shutdown_flag) {
            if (__atomic_load_n(&g_disconnect_signalled, __ATOMIC_SEQ_CST)) {
                fprintf(stderr, "mqtt: connection_lost callback fired — reconnecting\n");
                break;
            }
            if (!MQTTClient_isConnected(g_client)) {
                fprintf(stderr, "mqtt: isConnected==false — reconnecting\n");
                break;
            }
            sleep(1);
        }
        if (g_client) {
            /* Bound the disconnect call so a wedged TCP doesn't make us
             * sit here for 30+ s before retrying — 2 s is enough for a
             * clean DISCONNECT; abort after that and reconnect anyway. */
            MQTTClient_disconnect(g_client, 2000);
        }
    }

    if (g_client) {
        MQTTClient_destroy(&g_client);
        g_client = NULL;
    }
    /* g_topic_send is populated above for the future broker-publish helpers
     * (cases 0/1/6/34/...).  Reference it once here so -Wunused-but-set is
     * silent while those handlers land in a follow-up Phase A iteration. */
    (void)g_topic_send;
    return NULL;
}
