/*
 * ap2p — stream_timing.h
 *
 * Per-viewer-session instrumentation.  Mirrors boot_timing but resets
 * for every new pump-request (= every new viewer connecting).
 *
 * Goal: characterize the ~2 s gap between "SRT up, FLV flowing" log
 * line on the host viewer and the first decoded frame appearing in
 * ffplay.  See docs/STREAM_LATENCY_MEASUREMENT.md for the full design.
 *
 * Each session publishes ONE retained JSON to torque/tx/<NODE_ID>/stream
 * once the first FLV byte makes it onto the SRT socket.  Subsequent
 * sessions overwrite the retained payload.  Host-side tooling can
 * subscribe + log every session by clearing retained on each receive.
 *
 * All timestamps captured with CLOCK_MONOTONIC, sub-millisecond
 * precision.  Total session-start latency should be well under 2 s in
 * the optimized case; today (v2.0.5) we expect ~1-2 s with the
 * dominant cost concentrated in one stage.
 *
 * Thread-safety: a single global "current session" guarded by a mutex.
 * The pump worker is single-threaded so this is sufficient — control
 * thread posts new sessions, pump worker marks milestones, publish
 * happens from pump worker after first FLV byte ships.
 */
#ifndef AP2P_STREAM_TIMING_H
#define AP2P_STREAM_TIMING_H

#include <stddef.h>

typedef enum {
    ST_PEER_REQUESTED = 0, /* post_pump_request called by ctrl thread */
    ST_SRT_HANDSHAKE_BEGIN,/* srt_create_socket / srt_bind start       */
    ST_SRT_HANDSHAKE_DONE, /* srt_connect rendezvous returned OK       */
    ST_HTTP_GET_OPEN,      /* TCP connect to camera HTTP-FLV server OK */
    ST_HTTP_FIRST_BYTE,    /* first body byte received from HTTP server*/
    ST_FIRST_FLV_PUSHED,   /* first srt_send completed (frame on wire) */
    ST__COUNT
} st_milestone_t;

/* Begin a new session.  Resets all marks; copies peer_id for the JSON
 * payload.  Caller passes the most descriptive identifier available
 * — usually "<peer_ip>:<peer_port>" or the signaling-assigned peer SN. */
void st_session_begin(const char *peer_id);

/* Mark a milestone.  Idempotent — first call per milestone wins per
 * session.  No-op outside an active session. */
void st_mark(st_milestone_t m);

/* Render a JSON summary of the current session into buf; returns bytes
 * written.  Includes peer_id, all six milestones in seconds (or -1 if
 * unreached), plus deltas from ST_PEER_REQUESTED. */
size_t st_format_summary(char *buf, size_t buflen, const char *node_id);

/* Publish a retained JSON summary to torque/tx/<node_id>/stream.  Calls
 * st_format_summary internally.  One-shot per session — guarded so we
 * don't double-publish if multiple call sites fire it. */
void st_publish_summary(void *mqtt_client, const char *node_id);

#endif /* AP2P_STREAM_TIMING_H */
