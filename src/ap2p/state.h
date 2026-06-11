/*
 * ap2p — state.h
 *
 * v2.0 single-binary scaffold.  Shared state between mqtt_thread and srt_thread.
 *
 * RUNTIME CONFIG MODEL (v2.0):
 *   The runtime config (CTRL_*, EDGE_*, RELAY_*, SRC_*, LATENCY_MS, VERBOSE)
 *   is NOT persisted on the camera filesystem.  It arrives over MQTT as a
 *   retained message on topic torque/rx/<NODE_ID>/81; the broker auto-delivers
 *   it on subscribe.  apply_ap2p_conf_payload() applies it to the in-memory
 *   ap2p_state_t and signals state_ready_cv.  The SRT thread blocks on
 *   ap2p_wait_state_ready() until the first payload lands, then enters its
 *   main loop.
 *
 * Bootstrap config (MQTT broker URL + creds + NODE_ID) comes from
 * /mny/mtd/ipc/ambicam/config.json — same as today's MQTT_vcamclient — and
 * is loaded by main.c into the state struct BEFORE the threads are spawned.
 *
 * Hot reload: when MQTT delivers a fresh case-81 payload (operator updated
 * the retained message), apply_ap2p_conf_payload() sets srt_reload_pending
 * so the SRT thread tears down and rebuilds with the new values.
 */
#ifndef AP2P_STATE_H
#define AP2P_STATE_H

#include <pthread.h>
#include <signal.h>
#include <stddef.h>

typedef struct ap2p_state {
    pthread_mutex_t lock;

    /* Shutdown coordination — set from signal handlers (SIGTERM/SIGINT/SIGHUP)
     * and observed by both worker threads to break out of their loops. */
    volatile sig_atomic_t shutdown_flag;

    /* State-ready coordination.  The SRT thread blocks on state_ready_cv until
     * the MQTT thread has applied a case-81 payload populating ctrl_host etc.
     * On every subsequent fresh payload, srt_reload_pending is set so the
     * SRT thread tears down and rebuilds with the new values. */
    pthread_cond_t state_ready_cv;
    volatile sig_atomic_t state_ready;            /* 1 once first payload applied */
    volatile sig_atomic_t srt_reload_pending;     /* set on every subsequent push */

    /* Bootstrap (from config.json on flash — never delivered over MQTT). */
    char node_id[64];
    char bootstrap_mqtt_url[256];      /* mqttUrl     — primary broker  */
    char bootstrap_mqtt_url_bkp[256];  /* mqttUrl_BKP — failover broker (empty if not set) */
    char bootstrap_mqtt_user[64];
    char bootstrap_mqtt_pass[64];

    /* Runtime configuration (delivered as MQTT retained payload, KEY=value).
     * Empty until the first payload lands; SRT thread waits.  Sizes mirror
     * existing provider_srt buffers — keep small for flash. */
    char ctrl_host[128];
    int  ctrl_port;
    char ctrl_scheme[8];
    char ctrl_token[128];
    char edge_host[128];
    int  edge_port;
    char relay_host[128];
    int  relay_port;
    char relay_user[64];
    char relay_pass[64];
    char src_host[64];
    int  src_port;
    char src_path[256];
    char src_auth[128];
    int  latency_ms;
    int  verbose;

    /* MQTT client handle exposed to other threads (SRT thread uses it to
     * publish the one-shot boot-timing summary after signaling REGISTER
     * succeeds).  Set by mqtt_thread once MQTTClient_create has returned;
     * NULL before that.  paho-mqtt's publish path is thread-safe. */
    void *mqtt_client_handle;
} ap2p_state_t;

/* Lifecycle */
int  ap2p_state_init(ap2p_state_t *state);
void ap2p_state_destroy(ap2p_state_t *state);

/* Bootstrap loader: reads /mny/mtd/ipc/ambicam/config.json (JSON) and
 * populates node_id + bootstrap_mqtt_*.  Returns 0 on success, -1 otherwise.
 * Called from main() BEFORE the threads start. */
int  ap2p_state_load_bootstrap(ap2p_state_t *state, const char *config_json_path);

/* Apply an in-memory runtime-config payload (KEY=value flat format, matches
 * the v2.0 17-key schema).  Called by mqtt_thread on every case-81 delivery
 * — initial retained payload AND subsequent updates.  Internally takes the
 * lock, parses, and on success signals state_ready_cv + (if not first payload)
 * sets srt_reload_pending.  Returns 0 on success, -1 if payload was malformed
 * or didn't contain the minimum required keys (ctrl_host). */
int  apply_ap2p_conf_payload(ap2p_state_t *state, const char *payload, size_t len);

/* Reload-flag helpers — SRT thread consumes (test-and-clears) on each
 * iteration; MQTT thread sets via apply_ap2p_conf_payload(). */
int  ap2p_consume_srt_reload(ap2p_state_t *state);

/* State-ready wait — SRT thread calls this once at the top of its main loop;
 * blocks until apply_ap2p_conf_payload() has landed at least one valid
 * payload.  timeout_ms < 0 means wait forever.  Returns 0 if state became
 * ready, -1 on timeout, -2 if shutdown was requested while waiting. */
int  ap2p_wait_state_ready(ap2p_state_t *state, int timeout_ms);

/* Worker thread entry points (each implemented in its own .c). */
void *mqtt_thread_main(void *state_arg);
void *srt_thread_main(void *state_arg);

#endif /* AP2P_STATE_H */
