/*
 * ap2p — boot_timing.h
 *
 * Per-boot instrumentation.  Each milestone in the camera's
 * boot-to-go-live path captures /proc/uptime when reached; on first
 * successful signaling REGISTER we publish a single retained JSON
 * summary to torque/tx/<NODE_ID>/boot.  Host-side benchmark tooling
 * subscribes to that topic to measure where time is spent.
 *
 * All timestamps are in kernel-relative seconds (from /proc/uptime),
 * so they're directly comparable across reboots.  ambicam.sh writes
 * its own startup uptime to /tmp/boot_t0 before exec'ing ap2p, so
 * the publisher can compute "since launcher start" deltas too.
 *
 * Thread-safety: all functions take an internal mutex; the marker
 * functions are idempotent (first mark wins for each milestone),
 * which lets multiple call-sites along the same code path safely
 * call into them without double-counting.
 */
#ifndef AP2P_BOOT_TIMING_H
#define AP2P_BOOT_TIMING_H

#include <stddef.h>

typedef enum {
    BT_AP2P_MAIN = 0,     /* main() entered                                    */
    BT_CONFIG_LOADED,     /* config.json parsed                                */
    BT_MQTT_CONNECTING,   /* about to call MQTTClient_connect                  */
    BT_MQTT_CONNECTED,    /* MQTT CONNACK received                             */
    BT_SUBSCRIBED,        /* subscribe to torque/rx/<sn>/# completed           */
    BT_CASE81_RX,         /* retained config payload arrived + parsed start    */
    BT_STATE_READY,       /* state_ready_cv broadcast (apply succeeded)        */
    BT_SRT_STARTED,       /* SRT thread main loop entered                      */
    BT_EDGE_SRFLX,        /* STUN edge discovery success                       */
    BT_CTRL_REGISTERED,   /* signaling SRT_REGISTER ACK received  ←  GO-LIVE   */
    BT__COUNT
} bt_milestone_t;

/* Read /proc/uptime once (current kernel uptime, monotonic). */
double bt_uptime_now_sec(void);

/* Mark a milestone.  Idempotent — first call wins.  No-op if m out of range. */
void bt_mark(bt_milestone_t m);

/* Render a JSON summary describing every recorded milestone into `buf`.
 * Format includes both kernel-uptime values and deltas-since-ambicam-start
 * (zero if /tmp/boot_t0 wasn't written).  Returns bytes written. */
size_t bt_format_summary(char *buf, size_t buflen, const char *node_id);

/* Publish the JSON summary to `torque/tx/<node_id>/boot` via the supplied
 * paho-mqtt client handle (opaque to avoid pulling MQTT headers into all
 * call sites — caller passes the same MQTTClient pointer mqtt_thread
 * holds).  Retained so the broker keeps the most recent boot's report
 * available for inspection.  One-shot: subsequent calls are no-ops, so
 * multiple invocations from the registration callback are safe. */
void bt_publish_summary(void *mqtt_client, const char *node_id);

#endif /* AP2P_BOOT_TIMING_H */
