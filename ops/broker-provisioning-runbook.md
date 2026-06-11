# Runbook — provisioning camera runtime config over MQTT (broker side)

**Audience**: whoever administers the MQTT broker that the cameras subscribe to (`mqtt-staging.devices.arcisai.io:443` for the staging fleet, or the production broker URL for production fleet).

**Goal**: deliver per-camera runtime config (signaling host, STUN/TURN, source path, etc.) over MQTT so credentials never live on the camera flash long-term, AND so config rotation is one `mosquitto_pub` against a topic — no per-device deploy work needed.

> **v2.0.7 BREAKING CHANGE — retained-config topic moved from `/81` to `/90`.**
> The ArcisAI Kitty QC Tool uses case 81 for tampering-detect GET (matches
> v1.13.x semantics).  v2.0.x's repurposing of `/81` for retained-config
> caused the QC tool's tampering tests to time out.  v2.0.7+ cameras
> listen on `/90` for retained config; `/81` is now the tampering-detect
> handler.  `ops/fleet-provision.sh` and `ops/cutover-v2.0.sh` publish
> to `/90` AND clear `/81` (zero-byte retained) in the same run so old
> retained payloads don't trigger the new tampering handler.

---

## How it works in v2.0.x (current)

1. `ap2p` (the single camera daemon) reads `config.json` at startup for the MQTT broker URL + creds + (optional) serviceId.  That's the *only* file it reads from disk.
2. `ap2p` connects to the broker, subscribes to `torque/rx/<NODE_ID>/#`.
3. Operator has previously published a **retained** MQTT message on `torque/rx/<NODE_ID>/81` carrying the brand-neutral 17-key payload (`NODE_ID` / `CTRL_*` / `EDGE_*` / `RELAY_*` / `SRC_*` / `LATENCY_MS` / `VERBOSE`).
4. Broker auto-delivers the retained payload on subscribe.  `ap2p` parses the bytes into in-memory `ap2p_state_t`, signals its internal SRT thread to start, and the SRT thread does STUN + control-plane register + waits for peers.
5. To rotate config later, operator just re-publishes with `--retain`.  Every connected camera receives within seconds (case-81 hot reload, no daemon restart).  Camera restarts also pick up the latest retained payload automatically.

**No on-camera persistent config file in v2.0.x.**  No camera reboot needed.  No firmware update needed.  Just publish to the topic.

## How it worked in v1.13.x (legacy — for reference)

(Cameras on v1.13.x are still in production; this section documents that path.  When all cameras have moved to v2.x, this section can be deleted.)

1. `MQTT_vcamclient_Augentix` subscribes to `torque/rx/<serial>/#`.
2. When a message arrives on `torque/rx/<serial>/81`, `HTTP.c:messageArrived()` case 81 writes the message payload byte-for-byte to `/mny/mtd/ipc/ambicam/provider_srt.conf`, then `touch /tmp/provider_srt_conf_pushed`.
3. `ambicam.sh` monitor loop sees the sentinel within `MONITOR_INTERVAL` (10 s), `killall -TERM`s `provider_srt`, and respawns it — which re-reads the new conf and reconnects.

---

## What you need to do, step by step

### 1. Confirm the camera is on v1.13.0

```sh
# On the camera (via telnet, or your fleet-mgmt tool):
cat /mny/mtd/ipc/ambicam/VERSION                 # → 1.13.0
ls -la /mny/mtd/ipc/ambicam/provider_srt          # → exists, ~616 KB, ELF ARM
ls -la /mny/mtd/ipc/ambicam/MQTT_vcamclient_Augentix  # → exists, v1.13.0 build
```

If v1.12.x or earlier — the MQTT case 81 handler doesn't exist on the camera, broker messages to `/81` are silently dropped.  Flash v1.13.0 first.

### 2. Look up the camera's serial number

The MQTT subscription is `torque/rx/<serial>/#`, where `<serial>` matches what the camera advertises as `SERVICE_ID` in its `config.json` (or, if you go by hardware, the BurnUID).

For example, the test camera at `192.168.12.129` registers as `ATPL-200007-TESTA`.

### 3. Compose the `provider_srt.conf` payload

Plain text, one per device.  The file you publish is what ends up on the camera verbatim.  Use this template (filling in the **bold** values per device):

```ini
LOCAL_HOST=127.0.0.1
LOCAL_PORT=80
LOCAL_HTTP_PATH=/flv/live_ch0_1.flv?verify=**<per-device verify token>**
LOCAL_HTTP_AUTH=Basic YWRtaW46

SERVICE_ID=**<device serial, eg ATPL-200007-TESTA>**

# Plain-TCP signaling (current; will become TLS in v2 with mTLS)
SIGNALING_HOST=142.93.223.221
SIGNALING_PORT=8888
SIGNALING_SCHEME=plain

API_TOKEN=**<signaling-server API token; same per service tier>**

STUN_HOST=turn.devices.arcisai.io
STUN_PORT=5349

TURN_HOST=turn.devices.arcisai.io
TURN_PORT=5349
TURN_USERNAME=**<TURN user, eg arcisai>**
TURN_PASSWORD=**<TURN password>**

LATENCY_MS=300
VERBOSE=1
```

Place this file at, say, `/tmp/cam-<serial>.conf` on the broker host.

### 4. Publish it to the camera

```sh
# From the broker host (or any host with MQTT-publish rights):
mosquitto_pub \
    -h prong.arcisai.io -p 1883 \
    -u Torque -P <MQTT broker password> \
    -t "torque/rx/<serial>/81" \
    -q 1 \
    -f /tmp/cam-<serial>.conf
```

Key flags:
- `-q 1` — at-least-once delivery.  Don't use `-q 0` here; the conf push is rare and a single missed delivery means the camera keeps the old conf.
- `-f file` — read payload from file.  Don't `-m "$(cat file)"` because that mangles whitespace.
- **For v2.x cameras: add `--retain`.**  The broker stores the payload forever (or until you push a zero-byte retained payload to clear it).  Every time the camera subscribes — boot, reconnect, launcher respawn — the broker auto-delivers the latest retained payload.  This is the whole point of the v2 model.

For a fleet, wrap this in a loop reading from a per-device CSV/JSON table.

### 5. Verify on the camera within 30 seconds

You'll see in the camera's `provider_srt.log`:

```
[INFO ] config: re-read from /mny/mtd/ipc/ambicam/provider_srt.conf
[INFO ] signaling: connect 142.93.223.221:8888 (plain)
[INFO ] signaling: registered as <SERVICE_ID>
```

And in the camera's `ambicam.log`:

```
provider_srt.conf rotated by MQTT — restarting daemon to apply
provider_srt down — restarting (fail count 0, backoff 2s)
starting provider_srt
```

Both come for free because `ambicam.sh` monitors the sentinel and `provider_srt` restart cycle.

### 6. Spot-check no leakage

```sh
# Camera side: confirm the file landed and has the values you sent
cat /mny/mtd/ipc/ambicam/provider_srt.conf
```

If anything looks wrong, just publish again — the broker is the source of truth.

---

## Fleet rollout

The "do this for every camera" version:

```sh
# Pseudo-code; adapt to your broker tooling

for serial in $(awk -F, '/Augentix/{print $1}' fleet.csv); do
    # generate per-device conf from template + per-device variables
    sed -e "s/{{SERVICE_ID}}/$serial/" \
        -e "s/{{VERIFY_TOKEN}}/$(lookup_verify_token $serial)/" \
        -e "s/{{TURN_USER}}/arcisai/" \
        -e "s/{{TURN_PASS}}/$(secret tur_password)/" \
        provider_srt.conf.j2 > /tmp/cam-$serial.conf

    mosquitto_pub -h prong.arcisai.io -p 1883 \
                  -u Torque -P "$MQTT_BROKER_PASSWORD" \
                  -t "torque/rx/$serial/81" \
                  -q 1 -f /tmp/cam-$serial.conf

    rm -f /tmp/cam-$serial.conf   # don't leave secrets on the broker host
done
```

For long-term, this generation script should live in whatever orchestrator already publishes `torque/rx/<sn>/1` (the legacy `P2Pambicam_min.ini`).  Topic 81 is the same shape — same auth, same QoS, just different filename target on the camera.

## Credential rotation

The whole point of this automation: rotating TURN/STUN/API-token credentials is now just:

1. Change the value on the relevant server (coTURN, signaling).
2. Edit your `provider_srt.conf.j2` template with the new value.
3. Re-run the loop above.

Cameras pick up the new conf and reconnect within ~30 s.  No firmware update.  No reboot.

## What to do if MQTT delivery is unreliable

The cameras don't currently ack at the application layer — at-least-once QoS 1 is the safety net.  If you suspect a camera missed an update:

- `mosquitto_sub -h prong.arcisai.io -p 1883 -u Torque -P ... -t "torque/tx/<serial>/#"` and look for the daemon's heartbeat publishes.  If the camera is publishing on its `/tx/` topic with the new SERVICE_ID, the conf landed.

## When TLS+mTLS rolls out (Phase 2, NOT yet)

The payload template will switch to the TLS block (`CTRL_HOST=signaling.devices.arcisai.io`, `CTRL_PORT=443`, `CTRL_SCHEME=tls`, plus the per-device cert paths).  Same MQTT case 81, same topic, same delivery flow.  Only the conf content changes.

---

## v2.0.x payload schema (brand-neutral keys)

For cameras running v2.0.x — the broker pushes the **brand-neutral 17-key schema** with `--retain`:

```ini
NODE_ID = <camera serial, e.g. ATPL-200007-TESTA>

# Control plane (was SIGNALING_*)
CTRL_HOST   = signaling.devices.arcisai.io
CTRL_PORT   = 80                           # 443 + tls in v2.1
CTRL_SCHEME = plain                        # plain | tls
CTRL_TOKEN  = <opaque auth token>

# Edge discovery (was STUN_*)
EDGE_HOST = turn.devices.arcisai.io
EDGE_PORT = 5349

# Relay fallback (was TURN_*)
RELAY_HOST = turn.devices.arcisai.io
RELAY_PORT = 5349
RELAY_USER = arcisai
RELAY_PASS = <opaque>

# Local media source on the camera
SRC_HOST = 127.0.0.1
SRC_PORT = 80
SRC_PATH = /flv/live_ch0_1.flv?verify=<token>
SRC_AUTH = Basic YWRtaW46

LATENCY_MS = 300
VERBOSE = 1
```

Publish with:
```sh
mosquitto_pub -h mqtt-staging.devices.arcisai.io -p 443 \
              -u Torque -P 'Raptor@0' \
              -t "torque/rx/<NODE_ID>/81" \
              -q 1 --retain \
              -f /tmp/v2-config-<NODE_ID>.txt
```

The retained flag is **mandatory** for v2 — without it the camera won't receive the payload on subscribe.

See `ops/cutover-v2.0.sh` for the operator-side helper that translates a v1-format conf into the v2 schema + publishes it.

For v1.13.x cameras, the legacy SIGNALING_HOST/STUN_*/TURN_*/SERVICE_ID/LOCAL_* schema (documented earlier in this file) continues to work; `--retain` is optional but recommended for the same boot-time auto-delivery benefit.
