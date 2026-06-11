# Secrets handling — kitty-augentix-camera

## The rule

**No production secrets in source control, ever.**  This applies to:

- TURN passwords, STUN credentials
- MQTT broker passwords, API tokens, dashboard auth
- HTTP-FLV `?verify=` tokens
- Telnet root passwords on cameras
- Device private keys

Even though this repository is private, secrets in source control:
- Survive in git history forever (deleting a file doesn't remove it from `git log`)
- Get replicated to every developer's machine via `git clone`
- Are visible in CI logs if anything echoes a file containing them
- Become part of the STQC compliance scope as soon as they touch a certified box

## How real values get in

| Secret | Where the value actually lives | How it gets onto a camera |
|---|---|---|
| MQTT broker password (`config.json` `password`) | Provisioned at manufacturing time, burned into `/mny/mtd/ipc/ambicam/config.json` | Manufacturing-line script |
| **v2.0.x:** signaling host/port, TURN/STUN host+port+creds, source-path verify token, latency, verbose | **NEVER persisted on the camera.**  Broker holds the runtime config as an MQTT **retained** message on `torque/rx/<NODE_ID>/81`; `ap2p` parses it into in-memory state on every subscribe.  See `ops/broker-provisioning-runbook.md`. | MQTT retained payload, broker side only |
| v1.13.x: TURN/STUN/SIGNALING/API_TOKEN (`provider_srt.conf`) | Broker pushes `provider_srt.conf` content over MQTT case 81 — see `ops/mqtt-provisioning.md`.  `MQTT_vcamclient_Augentix` writes the payload to disk; `ambicam.sh` respawns `provider_srt` to reload it. | MQTT `torque/rx/<sn>/81` (non-retained in v1.x) |
| Signaling API token | Broker holds it server-side; cameras receive it in the case-81 payload (v2 retained; v1 written to provider_srt.conf) | server-side only after v1.13 |
| HTTP-FLV `?verify=` token | Per-device; in v2.0.x it's in the case-81 payload's `SRC_PATH` field.  In v1.13.x it's in `provider_srt.conf`'s `LOCAL_HTTP_PATH`. | broker-pushed |
| Device cert + private key (mTLS to signaling — v2.1 only) | Issued by `ArcisAI IoT Intermediate CA HSM` per device serial | Manufacturing-line script, dropped at `/etc/jffs2/ambicam/{kitty.crt, client.key}` |
| Telnet root password | Per-device, set at manufacturing time | Manufacturing-line script |

## How to write values into the repo (without writing actual values)

Use `REPLACE_WITH_*` placeholders in every `.example` file.  Tools that need a real value read it from:

1. Environment variables — `ARCISAI_API_TOKEN`, `ARCISAI_VERIFY_TOKEN`, `CAMERA_ROOT_PASSWORD`.
2. GitHub Actions secrets — for CI runs.
3. GCP Secret Manager — for any service running in GCP.
4. The on-camera filesystem — for daemons running on the camera itself.

The `tools/` Python scripts already read tokens from env vars; see `tools/augentix_view.py` and `tools/diagnostics/deploy_to_camera_telnet.py`.

### Why v2.0.x is structurally safer than v1.13.x

- The camera filesystem has **no persistent runtime config file** (no `provider_srt.conf`, no `ap2p.conf`).  Anyone who pulls a v2.0.x camera apart and inspects flash sees `config.json` (MQTT broker bootstrap only — no STUN/TURN/signaling secrets) and nothing else of interest.
- Secrets that DO need to reach the camera (TURN password, signaling API token, verify token) live ONLY in MQTT broker memory as a retained message.  Compromising one camera doesn't expose the fleet's secrets — each camera receives only its own per-device payload, and the payload is in memory only, not on disk.
- Brand-leak gate (`branding/verify.sh`) hard-fails CI on any `v2.x` tag if the `ap2p` binary contains identifiable upstream-library banners or legacy config-key vocabulary — defense-in-depth against `strings` reconnaissance.

## What we cleaned up in this commit

The first import from `Eco-Series-Kitty` accidentally pulled in the actual production values for:

- `Raptor@0` — MQTT broker password
- `Camera@Secure2024` — TURN password
- `p2p-server-api-token-change-me` — signaling API token
- `rtrt` — signaling dashboard auth
- `a/b4Znt+OFGrYtmHw0T16Q==` — HTTP-FLV verify token
- `adiance@999@arcisai` — camera telnet root password

All replaced with `REPLACE_WITH_*` placeholders or env-var reads.  These are still in **git history of the upstream `Eco-Series-Kitty` repo** — that's a separate cleanup the upstream owner should do (rotate the secret + force-push history rewrite, or accept the leak and rotate the secret).

## What you should rotate

Given the values were in plain text in `hardiksa/Eco-Series-Kitty` (a private GitHub repo that's been accessible to anyone with clone rights since `85cdef2` in May 2026), the prudent move is:

| Secret | Rotation effort | Priority |
|---|---|---|
| MQTT broker password `Raptor@0` | Change on broker + push new `config.json` to fleet via OTA | High |
| TURN password `Camera@Secure2024` (DO TURN at 134.209.155.47) | Change on coTURN + push new `provider_srt.conf` to fleet | High |
| Signaling API token | Change on signaling server + push new `provider_srt.conf` + redeploy our `kitty-signaling-server-del` | High |
| Dashboard auth `rtrt`/`rtrt` | Change on signaling server | High — also `rtrt` is trivially guessable |
| HTTP-FLV `?verify=` token | Per-device; re-issue at next manufacturing batch | Medium |
| Telnet root password | Per-device; cameras already deployed don't easily change this without OTA | Low (telnet is LAN-only and disabled-by-default on most deployments) |
