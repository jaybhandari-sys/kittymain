/*
 * ap2p — legacy_cases.h
 *
 * Glue header exposing the legacy MQTT case-dispatch (cases 0..80 + 82+).
 *
 * The implementation lives in `legacy_cases.c`, which is a near-verbatim
 * copy of the v1.13.x `src/mqtt-vcamclient/HTTP.c`.  The verbatim copy
 * preserves dozens of handlers (handle_case_0/2/6/23/34/38/39/... plus the
 * 75 inline case bodies) that would otherwise need a per-case re-port.
 *
 * Surgical changes vs the original:
 *   - main() is wrapped in `#if 0` (ap2p's main is in src/ap2p/main.c).
 *   - messageArrived → legacy_message_arrived; the MQTTClient_freeMessage()
 *     + MQTTClient_free(topicName) calls are removed (caller owns them).
 *   - case 81 in the switch is a no-op — runtime-config rotation is
 *     handled in-process by src/ap2p/mqtt_thread.c → apply_ap2p_conf_payload.
 *
 * mqtt_thread.c invokes legacy_message_arrived() for case_num != 81 so the
 * legacy handlers run with the existing MQTT client + topic prefixes that
 * the legacy code already knows how to set up (it has its own client global
 * that gets populated from config.json on first call — see load_mqtt_config
 * + init_device inside legacy_cases.c).
 */
#ifndef AP2P_LEGACY_CASES_H
#define AP2P_LEGACY_CASES_H

#include "MQTTClient.h"

/* Run the legacy dispatcher against a single MQTT delivery.  Same signature
 * as paho's message_arrived callback — but the caller, NOT this function,
 * owns the message + topicName lifetime. */
int legacy_message_arrived(void *context, char *topicName, int topicLen,
                           MQTTClient_message *message);

/* Bootstrap the legacy module from ap2p's already-loaded config.  Called
 * once from mqtt_thread.c after connect_once() succeeds.  Wires the
 * legacy globals (`client`, `MQTT_TOPIC_RECEIVE`, `MQTT_TOPIC_SEND`,
 * `USERNAME`, `PASSWORD`, `MQTT_HOST`) so the legacy handlers can
 * publish responses back via the existing connection. */
void ap2p_legacy_init(MQTTClient client,
                      const char *node_id,
                      const char *mqtt_url,
                      const char *username,
                      const char *password);

/* v2.0.7 — startup thread entry.  Spawned (detached) from mqtt_thread.c
 * after subscribe + initial retained-config land.  Calls
 * run_startup_cases() which publishes NTP/TZ/deviceInfo + per-channel
 * config on every camera boot. */
void *ap2p_legacy_startup_thread(void *arg);

/* v2.0.7-rc3 — periodic keepalive thread.  Spawned (detached) from
 * mqtt_thread.c after subscribe.  Publishes torque/tx/<sn>/0 every 30 s
 * with the v1.13.x "Keep Alive!" payload (handle_case_0 → temperature +
 * SD stats).  This is what every IoT-side alerting rule depends on to
 * decide "camera is alive".  The arg is &ap2p_state so the thread can
 * observe shutdown_flag for SIGTERM-clean exit. */
void *ap2p_legacy_keepalive_thread(void *arg);

#endif /* AP2P_LEGACY_CASES_H */
