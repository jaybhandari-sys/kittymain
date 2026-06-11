/*
 * ap2p — main.c
 *
 * v2.0 single-binary entry point.  Two pthreads, one process, zero .so's of
 * our own.  See branding/manifest.toml for the brand contract.
 *
 * Runtime model:
 *   1. Read /mny/mtd/ipc/ambicam/config.json — MQTT broker URL + creds + NODE_ID.
 *      This is the ONLY config file ap2p reads from disk.
 *   2. Spawn two pthreads.  The MQTT thread connects to the broker and
 *      subscribes; the broker auto-delivers the retained case-81 payload
 *      with the rest of the runtime config (CTRL_*, EDGE_*, RELAY_*, SRC_*,
 *      LATENCY_MS, VERBOSE).
 *   3. The MQTT thread calls apply_ap2p_conf_payload(state, bytes, len) on
 *      every case-81 delivery.  The SRT thread is blocked on state_ready_cv
 *      until the first payload lands.
 *   4. SIGTERM/SIGINT/SIGHUP set the shutdown flag; both worker threads
 *      observe it and return cleanly.
 */
#include "state.h"
#include "boot_timing.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>

#define DEFAULT_BOOTSTRAP_PATH "/mny/mtd/ipc/ambicam/config.json"
#define DEFAULT_NODE_ID_PATH   "/mny/mtd/ipc/BurnUID"

/* Fallback NODE_ID source.  config.json may or may not carry SERVICE_ID
 * depending on firmware vintage; the SN file is always present on flash.
 * Trims a trailing newline. */
static int load_node_id_from_file(ap2p_state_t *state, const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    if (!fgets(state->node_id, sizeof(state->node_id), fp)) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    size_t n = strlen(state->node_id);
    while (n > 0 && (state->node_id[n - 1] == '\n' ||
                      state->node_id[n - 1] == '\r' ||
                      state->node_id[n - 1] == ' ')) {
        state->node_id[--n] = '\0';
    }
    return state->node_id[0] ? 0 : -1;
}

/* Signal handler reaches the worker threads via this pointer.  Set once in
 * main() before signal handlers are armed; never reassigned. */
static ap2p_state_t *g_state = NULL;

static void on_signal(int signo)
{
    (void)signo;
    if (g_state) g_state->shutdown_flag = 1;
}

static void install_signal_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    /* No SA_RESTART — we want blocking syscalls to return EINTR so the
     * thread loops can re-check shutdown_flag promptly. */
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGHUP,  &sa, NULL);
}

/* Instrumented logger — flush every line to surface where the binary hangs.
 * Goes to stderr because ambicam.sh redirects both stdout AND stderr to
 * ap2p.log, but stderr is unbuffered for line-output by default. */
#define TRACE(...) do { \
    fprintf(stderr, "[ap2p:trace] " __VA_ARGS__); \
    fputc('\n', stderr); \
    fflush(stderr); \
} while (0)

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    /* Boot-timing canary AND first measured milestone. */
    bt_mark(BT_AP2P_MAIN);
    /* This first line is the canary: if it doesn't appear in ap2p.log, the
     * crash/hang is happening BEFORE main() — almost certainly in a C++
     * static constructor or .init_array entry (libsrt is C++; its globals
     * run before main).  If it DOES appear, main is reachable and we
     * narrow further with the TRACE() calls below. */
    TRACE("main() entered argc=%d  uptime=%.3f", argc, bt_uptime_now_sec());

    TRACE("calling ap2p_state_init");
    ap2p_state_t state;
    if (ap2p_state_init(&state) != 0) {
        fprintf(stderr, "ap2p: state init failed\n");
        return 1;
    }
    TRACE("ap2p_state_init OK");

    TRACE("calling ap2p_state_load_bootstrap(%s)", DEFAULT_BOOTSTRAP_PATH);
    if (ap2p_state_load_bootstrap(&state, DEFAULT_BOOTSTRAP_PATH) != 0) {
        fprintf(stderr, "ap2p: cannot load bootstrap from %s\n",
                DEFAULT_BOOTSTRAP_PATH);
        ap2p_state_destroy(&state);
        return 1;
    }
    TRACE("bootstrap loaded node_id='%s' mqtt='%s' user='%s'",
          state.node_id, state.bootstrap_mqtt_url, state.bootstrap_mqtt_user);

    /* Fall back to the camera SN file if config.json didn't carry a
     * service_id / SERVICE_ID key.  Either source must yield a non-empty
     * node_id or we have no MQTT topic to subscribe on. */
    if (!state.node_id[0]) {
        TRACE("node_id empty, falling back to %s", DEFAULT_NODE_ID_PATH);
        if (load_node_id_from_file(&state, DEFAULT_NODE_ID_PATH) != 0) {
            fprintf(stderr, "ap2p: no NODE_ID in config.json and no %s\n",
                    DEFAULT_NODE_ID_PATH);
            ap2p_state_destroy(&state);
            return 1;
        }
        TRACE("BurnUID node_id='%s'", state.node_id);
    }

    g_state = &state;
    TRACE("installing signal handlers");
    install_signal_handlers();
    TRACE("signal handlers installed");

    pthread_t t_mqtt, t_srt;
    TRACE("pthread_create mqtt_thread_main");
    if (pthread_create(&t_mqtt, NULL, mqtt_thread_main, &state) != 0) {
        fprintf(stderr, "ap2p: mqtt pthread_create failed\n");
        ap2p_state_destroy(&state);
        return 1;
    }
    TRACE("mqtt thread spawned");

    TRACE("pthread_create srt_thread_main");
    if (pthread_create(&t_srt, NULL, srt_thread_main, &state) != 0) {
        fprintf(stderr, "ap2p: srt pthread_create failed\n");
        state.shutdown_flag = 1;
        pthread_join(t_mqtt, NULL);
        ap2p_state_destroy(&state);
        return 1;
    }
    TRACE("srt thread spawned");

    TRACE("pthread_join mqtt");
    pthread_join(t_mqtt, NULL);
    TRACE("mqtt joined");
    TRACE("pthread_join srt");
    pthread_join(t_srt,  NULL);
    TRACE("srt joined, exiting");
    ap2p_state_destroy(&state);
    return 0;
}
