/*
 * ap2p — state.c
 *
 * Lifecycle + reload/state-ready helpers for the shared ap2p_state_t.
 * Runtime-config parsing lives in config.c; this file is just mutex/cv plumbing.
 */
#include "state.h"

#include <string.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>

int ap2p_state_init(ap2p_state_t *state)
{
    if (!state) return -1;
    memset(state, 0, sizeof(*state));
    if (pthread_mutex_init(&state->lock, NULL) != 0) return -1;
    if (pthread_cond_init(&state->state_ready_cv, NULL) != 0) {
        pthread_mutex_destroy(&state->lock);
        return -1;
    }
    /* Sensible defaults — overridden by apply_ap2p_conf_payload(). */
    state->ctrl_port  = 80;
    state->edge_port  = 0;
    state->relay_port = 0;
    state->src_port   = 80;
    state->latency_ms = 300;
    state->verbose    = 0;
    return 0;
}

void ap2p_state_destroy(ap2p_state_t *state)
{
    if (!state) return;
    pthread_cond_destroy(&state->state_ready_cv);
    pthread_mutex_destroy(&state->lock);
}

int ap2p_consume_srt_reload(ap2p_state_t *state)
{
    if (!state) return 0;
    int was_set = 0;
    pthread_mutex_lock(&state->lock);
    if (state->srt_reload_pending) {
        was_set = 1;
        state->srt_reload_pending = 0;
    }
    pthread_mutex_unlock(&state->lock);
    return was_set;
}

int ap2p_wait_state_ready(ap2p_state_t *state, int timeout_ms)
{
    if (!state) return -1;
    pthread_mutex_lock(&state->lock);
    int rc = 0;
    if (timeout_ms < 0) {
        /* Wait forever, but periodically check shutdown_flag so SIGTERM
         * unblocks us cleanly. */
        while (!state->state_ready && !state->shutdown_flag) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 1;
            pthread_cond_timedwait(&state->state_ready_cv, &state->lock, &ts);
        }
        if (state->shutdown_flag) rc = -2;
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec  += timeout_ms / 1000;
        ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        while (!state->state_ready && !state->shutdown_flag) {
            int w = pthread_cond_timedwait(&state->state_ready_cv, &state->lock, &ts);
            if (w == ETIMEDOUT) { rc = -1; break; }
        }
        if (rc == 0 && state->shutdown_flag) rc = -2;
    }
    pthread_mutex_unlock(&state->lock);
    return rc;
}
