# MQTT-driven configuration of camera daemons

## How `P2Pambicam_min.ini` is provisioned today (works correctly)

The camera-side MQTT client (`MQTT_vcamclient_Augentix`, built from `src/mqtt-vcamclient/`) subscribes to `torque/rx/<serial>/#`.  On every message landing on `torque/rx/<serial>/1`, it writes the raw payload byte-for-byte to `/etc/jffs2/ambicam/P2Pambicam_min.ini`.

The broker is the source of truth — it generates the per-device ini including TURN credentials, signaling host, service ID, etc., and pushes it as the MQTT message payload.  No hardcoded TURN/STUN credentials on the camera.

```
[broker]  →  publishes provider INI  →  topic torque/rx/<sn>/1
[MQTT_vcamclient on camera]  →  receives  →  fwrite(/etc/jffs2/ambicam/P2Pambicam_min.ini)
[ambicam.sh]  →  detects file, starts P2Pambicam  (legacy libjuice provider)
```

This is the **existing automation for the legacy libjuice provider** (`P2Pambicam`).  It works today.

## Why this is currently broken for `provider_srt` (the new SRT daemon)

`provider_srt` reads `/mny/mtd/ipc/ambicam/provider_srt.conf`, **not** `/etc/jffs2/ambicam/P2Pambicam_min.ini`.  Today that file is **hand-deployed** on each camera with hardcoded values (we found `TURN_PASSWORD=turnpassword123` baked into the file on the dev camera).  The MQTT client does not push `provider_srt.conf` content from the broker.  Therefore TURN/STUN/signaling credentials for the SRT path are presently in cleartext on each camera's flash and are not centrally rotated.

## What needs to happen for end-to-end automation

| Step | Where | Owner | Status |
|---|---|---|---|
| 1. Add a new case in `src/mqtt-vcamclient/HTTP.c` that writes its payload to `/mny/mtd/ipc/ambicam/provider_srt.conf` | Camera-side code | **us** (PR follows) | TODO |
| 2. Update `deploy/ambicam.sh` to wait for `/mny/mtd/ipc/ambicam/provider_srt.conf` (alongside the existing `P2Pambicam_min.ini` wait) before starting `provider_srt` | Camera-side code | **us** | TODO |
| 3. On the broker, generate the per-device `provider_srt.conf` content (filling in `SERVICE_ID`, `TURN_*`, `STUN_*`, `SIGNALING_*`, and — once we flip on mTLS — also pointing at the device cert/key paths) | Broker-side automation | **you** | TODO |
| 4. Publish that content to `torque/rx/<sn>/<new-case>` once per camera at provisioning time, and on every credential rotation | Broker-side automation | **you** | TODO |
| 5. Remove hardcoded TURN/STUN/API_TOKEN values from `/mny/mtd/ipc/ambicam/provider_srt.conf` on every camera (force-deliver new file via MQTT) | Fleet-wide OTA | **you** + **us** | TODO after (1)-(4) |

## Proposed topic numbering

Existing cases in `HTTP.c` go up to 24.  To keep things obvious:

| Topic | Payload | Target file |
|---|---|---|
| `torque/rx/<sn>/1` | `P2Pambicam_min.ini` (raw text) | `/etc/jffs2/ambicam/P2Pambicam_min.ini` (existing) |
| `torque/rx/<sn>/81` | `provider_srt.conf` (raw text) | `/mny/mtd/ipc/ambicam/provider_srt.conf` (NEW in v1.13) |

Topic `81` was picked because cases `0`–`80` are already taken in the existing `HTTP.c` switch.

(Case `25` is suggested; pick any free slot you prefer on the broker side.)

## Code patch sketch (for our PR)

```c
// In HTTP.c messageArrived() switch:
case 25: {  // NEW: provider_srt.conf push
    printf("Handling case 25 (provider_srt.conf)\n");
    save_file_from_message("/mny/mtd/ipc/ambicam/provider_srt.conf", message);
    // Signal ambicam.sh that the config landed — touch a sentinel.  Watchdog
    // in ambicam.sh restarts provider_srt to pick up the new conf.
    system("touch /tmp/provider_srt_conf_pushed");
    break;
}
```

And in `deploy/ambicam.sh`, wait for `/mny/mtd/ipc/ambicam/provider_srt.conf` alongside the existing config-wait loop, and SIGHUP / restart `provider_srt` when `/tmp/provider_srt_conf_pushed` is fresh.

## Rotation properties this gets us

- TURN/STUN/API-token rotation = broker publishes new content + cameras restart provider_srt.  No firmware change needed.
- Credential leak response = same flow, minutes-scale rollout.
- Per-device customisation (different TURN tier for different camera classes, latency tuning per camera location) = trivial.

## What stays out of scope (next-next version)

- Encrypted MQTT payloads.  Today the payload is plaintext — anyone with MQTT broker creds can see the TURN password.  Bumping the broker to MQTT over TLS (8883) addresses transit; for end-to-end secret protection we'd encrypt the INI content with a per-device key.  Not a v1.13 concern.
