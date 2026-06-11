# Dev integration guide — Augentix kitty on GCP staging

**Audience:** the engineer integrating the Android (or any) consumer
against the GCP staging stack.  This is the *handover* doc — every
endpoint, credential, code path, and test command you need to render
live video from the dev camera through the new infrastructure.

> **Android engineers:** read [`ops/android-integration-guide.md`](android-integration-guide.md)
> **first** — it covers the Android-specific MQTT timeout failure modes
> (cleartext block on API 28+, wrong URI scheme, subscribe-after-publish
> race) that account for >80% of bench reports.  This generic guide
> stays valid for the protocol layer; the Android guide adds the
> platform-specific recipe + a working Paho-Android example.

**Status as of camera firmware v2.0.x:**

* Dev camera `ATPL-200007-TESTA` @ `192.168.12.129` is **fully on GCP
  staging**, running the v2.0.x single-binary `ap2p` daemon (replaces
  v1.13.x's `provider_srt` + `MQTT_vcamclient_Augentix`).  No traffic
  to the DigitalOcean signaling server anymore.
* Control plane → `mqtt-staging.devices.arcisai.io:443` (plain TCP).
* Signaling → `signaling.devices.arcisai.io:80` (plain TCP).
* STUN / TURN unchanged on `turn.devices.arcisai.io:5349` (production
  STQC box; we do not modify it).

**Note for v2.x camera firmware:**  The consumer-facing protocols
(STUN / signaling / SRT / HTTP-FLV) are **identical** to v1.13.x.  Your
Android integration does not change between v1 and v2 — the camera-side
re-architecture (two binaries → one) is invisible on the wire.

---

## 1. Endpoint summary

| Layer                    | Endpoint                                    | Proto                  | Auth                                               |
| ------------------------ | ------------------------------------------- | ---------------------- | -------------------------------------------------- |
| MQTT control plane       | `mqtt-staging.devices.arcisai.io:443`       | plain MQTT over TCP    | `Torque` / `Raptor@0`                              |
| Signaling (rendezvous)   | `signaling.devices.arcisai.io:80`           | plain TCP, length-pref | `API_TOKEN` field in `SRT_REGISTER` / `SRT_REQUEST` |
| STUN                     | `turn.devices.arcisai.io:5349`              | UDP                    | none                                               |
| TURN                     | `turn.devices.arcisai.io:5349`              | UDP                    | `arcisai` / `turnpassword123`                      |
| Local HTTP-FLV on camera | `/flv/live_ch0_0.flv` (main, 2304×1296)     | HTTP/1.1 over SRT      | `Authorization: Basic YWRtaW46` + `?verify=…`      |
|                          | `/flv/live_ch0_1.flv` (sub, 800×448)        | same                   | same                                               |

All endpoints reachable over **port 443 or 80 only** — corporate-firewall
friendly.  The :80 plain-TCP signaling channel is staging-only; v2 with
mTLS lives on :443 (separate cutover).

> **Important:** the dev camera is one device.  Last-viewer-wins on the
> camera, so when more than one consumer connects to the same channel
> they will displace each other (the camera's `provider_srt` cancels the
> older pump when a new peer arrives).  A LAN device at
> `192.168.12.139` continuously hits the **sub** stream — for clean
> testing always request the **main** path (`live_ch0_0.flv`).

---

## 2. Secrets

```
API_TOKEN          = p2p-server-api-token-change-me
TURN_USERNAME      = arcisai
TURN_PASSWORD      = turnpassword123
MQTT_USERNAME      = Torque
MQTT_PASSWORD      = Raptor@0
HTTP_BASIC_AUTH    = Basic YWRtaW46          (≡ admin:  — literal trailing colon)
HTTP_VERIFY_TOKEN  = a%2Fb4Znt%2BOFGrYtmHw0T16Q%3D%3D
                     (URL-encoded form of  a/b4Znt+OFGrYtmHw0T16Q== )
```

> These are **staging** secrets, also still live on the existing
> production DO signaling.  Rotation is pending; see `SECURITY.md`.
> Do **not** publish these to anyone outside the team or check them
> into a public repo.

---

## 3. End-to-end flow your consumer must implement

The protocol is identical to what the existing `Signalling-Server/p2p_libjuice-main` consumer already does — the only change is **which signaling host** it talks to.  Three steps:

### Step 1 — STUN bind to learn your own SRFLX

* Bind a UDP socket on `0.0.0.0:0` (any free port).
* Send an RFC 5389 BindingRequest to `turn.devices.arcisai.io:5349`.
* Read the BindingResponse and extract `XOR-MAPPED-ADDRESS` →
  your `(SRFLX_IP, SRFLX_PORT)`.
* **Keep the UDP socket open** — you'll re-use the same local port for
  the SRT rendezvous so the NAT mapping survives.  Closing and rebinding
  drops the public mapping.

Reference: `tools/augentix_view.py:stun_discover()`.

### Step 2 — TCP signaling REGISTER (consumer side) on GCP

```text
TCP connect      signaling.devices.arcisai.io:80     (NOT 8888, NOT mTLS)
Wire format      length-prefixed frames, body is "k=v\n" pairs
Request 1 send   action=SRT_REQUEST
                 service_id=ATPL-200007-TESTA
                 api_token=p2p-server-api-token-change-me
                 srflx_ip=<your_srflx_ip_from_step_1>
                 srflx_port=<your_srflx_port_from_step_1>
Reply            action=SRT_PROVIDER
                 srflx_ip=<camera's public IP>
                 srflx_port=<camera's public UDP port>
                 [lan_ip, lan_port optional]
```

If the camera-side `provider_srt` is registered (it is, we just verified
it's pinging `signaling.devices.arcisai.io:80` every 10 s) then this
exchange completes in <100 ms.

Reference: `tools/augentix_view.py:signaling_request()`.

### Step 3 — SRT rendezvous on the same UDP port

* Reuse the UDP socket from step 1 — bind libsrt to the same local port.
* Open an SRT socket with:
  * `SRTO_RENDEZVOUS = 1`
  * `SRTO_TRANSTYPE  = SRTT_FILE` (the camera uses stream-API, not msg-API;
    mismatch → "Agent uses MESSAGE API, but Peer declares STREAM API")
  * `SRTO_LATENCY    = 300` (ms; matches camera default)
* `srt_connect()` to the camera's `(SRFLX_IP, SRFLX_PORT)`.
* Handshake completes in ~150 ms over UDP hole-punch.
* Camera's `provider_srt` accepts the rendezvous and waits for the HTTP-FLV GET.

Reference: `tools/augentix_view.py:srt_rendezvous()`.

### Step 4 — HTTP-FLV GET over the SRT tunnel

Inside the SRT byte stream send (CRLF line endings, blank line terminator):

```http
GET /flv/live_ch0_0.flv?verify=a%2Fb4Znt%2BOFGrYtmHw0T16Q%3D%3D HTTP/1.1
Host: 127.0.0.1
Authorization: Basic YWRtaW46
User-Agent: <your client name>/<version>

```

> **Do NOT** send `Connection: close` — it tells the camera's HTTP-FLV
> server to flush+close the SRT after the response, producing the
> "Invalid socket ID" + 12 KB-and-die pattern.  Default HTTP/1.1
> keep-alive is what you want; close from your end when you're done.

The camera responds with `HTTP/1.1 200 OK\r\nContent-Type: video/x-flv\r\n\r\n`
followed by an infinite FLV byte stream.

> **FLV header DataOffset patch** — the camera's libflv emits the FLV
> header with `DataOffset = 0`, which violates the spec (should be 9).
> Some demuxers reject it.  Patch the first 9 bytes on receive:
> bytes 0–4 are `b"FLV\x01\x05"`, then write `\x00\x00\x00\x09` into
> bytes 5–8.  Reference: `tools/augentix_view.py:srt_pump_thread()`
> (the `header_patched` block).

### Step 5 — feed the bytes to your decoder

* Codec is **HEVC Main profile** (camera publishes `H.265` in the channel
  config; ffprobe confirms `codec_name=hevc level=90`).
* SPS/PPS/VPS + IDR_W_RADL keyframe arrives every ~3-5 frames at 15 fps,
  so a decoder that probes the first ~256 KB always finds enough to lock.
* Resolution: 2304×1296 main / 800×448 sub.  Both yuvj420p, bt709.
* Wrap with `ExoPlayer` (Android) using a custom MediaSource feeding raw
  HEVC NALs, or pipe the FLV byte stream through any standard FLV demuxer
  that supports HEVC-in-FLV (the camera's tag IDs follow the de-facto
  CN-IPC convention — videoCodecId = 12).

---

## 4. End-to-end test (do this first)

### 4.1 — confirm MQTT plane is up

```sh
mosquitto_sub -h mqtt-staging.devices.arcisai.io -p 443 \
              -u Torque -P 'Raptor@0' \
              -t 'torque/tx/ATPL-200007-TESTA/#' -v
```

Within seconds you should see lines like:

```
torque/tx/ATPL-200007-TESTA/1  (heartbeat)
torque/tx/ATPL-200007-TESTA/2  {"id":101,..."resolution":"2304x1296"...}
torque/tx/ATPL-200007-TESTA/6  {"id":1,"enabled":true,...}
torque/tx/ATPL-200007-TESTA/38 {"irCutFilter":{...}...}
torque/tx/ATPL-200007-TESTA/39 {"id":102,...}
```

If you see nothing, the camera is offline — check Wi-Fi / power before
debugging signaling.

### 4.2 — confirm signaling rendezvous works (Mac reference consumer)

```sh
# from the kitty repo root:
cd kitty-augentix-camera
brew install srt ffmpeg                              # one-time

ARCISAI_API_TOKEN='p2p-server-api-token-change-me' \
ARCISAI_VERIFY_TOKEN='a%2Fb4Znt%2BOFGrYtmHw0T16Q%3D%3D' \
SERVICE_ID=ATPL-200007-TESTA \
CHANNEL_PATH=/flv/live_ch0_0.flv \
  tools/view-feed.sh
```

`view-feed.sh` does the whole orchestration: kills any stale tester,
spawns a fresh `augentix_view.py` (which talks to the GCP signaling on
:80 if you pass `--signaling-host signaling.devices.arcisai.io
--signaling-port 80`), waits for SRT, then ffplays the local HTTP-FLV
endpoint.  ~2 s glass-to-glass.

To explicitly target the **GCP signaling**:

```sh
ARCISAI_API_TOKEN=… ARCISAI_VERIFY_TOKEN=… \
  python3 tools/augentix_view.py \
    --signaling-host signaling.devices.arcisai.io \
    --signaling-port 80 \
    --service-id ATPL-200007-TESTA \
    --channel-path /flv/live_ch0_0.flv &
sleep 4
ffplay -fflags nobuffer -flags low_delay -framedrop \
       http://127.0.0.1:8080/live.flv
```

Successful run prints:

```
[stun]  SRFLX=…:…
[sig]   provider SRFLX=…:…   ← the camera's public address (proves GCP signaling found it)
[srt]   rendezvous connecting to …:… ...
[srt]   connected (handle=…)
[srt]   sent 176B HTTP-FLV GET over SRT
[srt]   first FLV bytes: header patched (DataOffset 0→9)
```

### 4.3 — port the same flow to your Android consumer

You should already have a working consumer against the DO signaling.
Change exactly two lines:

```diff
- SIGNALING_HOST = "142.93.223.221"
+ SIGNALING_HOST = "signaling.devices.arcisai.io"

- SIGNALING_PORT = 8888
+ SIGNALING_PORT = 80
```

Everything else (STUN, TURN, API_TOKEN, MQTT topics, FLV path, FLV
header patch, decoder setup) is identical.

---

## 5. Reference files in this repo

| File                                          | What it shows                              |
| --------------------------------------------- | ------------------------------------------ |
| `tools/augentix_view.py`                      | Full reference consumer in Python (~600 LoC) |
| `tools/view-feed.sh`                          | One-shot ffplay launcher                   |
| `src/ap2p/srt_thread.c`                       | Camera-side SRT pump (v2; ported from `src/provider-srt/provider_srt.c`) |
| `src/ap2p/mqtt_thread.c` + `legacy_cases.c`   | MQTT loop + 81 case handlers (incl. case 81 = config rotation) |
| `src/ap2p/state.h`                            | Shared state schema (17 brand-neutral keys) |
| `ops/broker-provisioning-runbook.md`          | How an operator rotates camera secrets     |
| `ops/firmware-update-tool.md`                 | `Augentix.tar.gz` contract for the deploy tool |
| `ops/v2.0-cutover-runbook.md`                 | v1.13.x → v2.0 cutover workflow            |

---

## 6. What's *not* in this cutover (still on the original infra)

* **TURN/STUN** are on `turn.devices.arcisai.io` (the production STQC
  box, IP `34.100.143.36`).  No GCP staging copies — we do not modify
  that VM.  Your consumer's STUN/TURN config does not change.
* **Production cameras** still go to the production stack via
  `mqtt.devices.arcisai.io` + `sig.devices.arcisai.io` (same IP
  `34.100.143.36`).  This staging cutover is dev-camera-only.
* **TLS / mTLS** for signaling + MQTT will follow as v2 (Phase 2).
  Same staging VMs, same hostnames, but :443 with mTLS instead of :80
  plain.  Your consumer will need an OpenSSL/BoringSSL layer at that
  point — not now.

---

## 7. Common pitfalls

| Symptom                                   | Likely cause                                                   | Fix |
| ----------------------------------------- | -------------------------------------------------------------- | --- |
| `signaling unexpected reply: ERROR`       | wrong `api_token`, or registering before camera is up           | check camera is on (`mosquitto_sub` for heartbeats); confirm token spelling |
| `signaling: SRT_PROVIDER missing srflx_*` | camera not registered on this signaling server                  | run `Step 4.1` first, then verify camera-side log says `signaling: registered as <service_id>` |
| SRT handshake never completes             | NAT mapping died between STUN and SRT — bind reused?            | bind one UDP socket up-front, **never close it** between STUN and SRT; libsrt `bind` to same local port |
| "Agent uses MESSAGE API…" SRT error       | `SRTO_TRANSTYPE` not set to `SRTT_FILE`                         | set the socket option **before** `srt_connect()` |
| "Invalid socket ID" 12 KB into stream     | client sent `Connection: close` in the HTTP-FLV GET             | drop that header; HTTP/1.1 keep-alive is the default |
| FLV demuxer rejects header                | camera writes `DataOffset = 0` instead of 9                     | patch bytes 5–8 of the first FLV header on receive (see step 4 note) |
| ffmpeg says `Could not find ref with POC` | normal — started mid-GOP before next IDR, decoder catches up    | wait one keyframe period (~333 ms at this camera's IDR cadence) |
| Stream cuts after a few seconds           | another viewer attached; "latest wins" on the camera             | use the **main** channel (`live_ch0_0.flv`); `192.168.12.139` is constantly camping `live_ch0_1.flv` |

---

## 8. Versions in this stack

* **Current dev camera**: `v2.0.x` (single-binary `ap2p`)
* **Latest release**: [`v2.0.1`](https://github.com/Adiance-Technologies/kitty-augentix-camera/releases/tag/v2.0.1)
  — packaging-only change vs v2.0.0; ships `Augentix.tar.gz` for the legacy firmware update tool

### v2.0.0 binaries

* `ap2p` (single statically-merged ELF):  md5 `d082ef0b31213c76ec8756a746ead0e8`,  663 452 bytes stripped + brand-scrubbed
* `ambicam.sh` (v2 minimal supervisor):   md5 `76708798f336c2d561510da8743f6796`, 404 bytes

### Legacy v1.13.2 (still on production fleet)

* `provider_srt` md5 `d683bb6813851309ca837d0cabbb2823`
* `MQTT_vcamclient_Augentix` md5 `e37ab3c3dfecf7073de557a076a1c02e`
* `ambicam.sh` (full watchdog) md5 `575a82062b376a6e0a1204ba462008c0`

### Wire protocol versions (unchanged between v1.13.x and v2.0.x)

* STUN: RFC 5389 Binding Request, XOR-MAPPED-ADDRESS attribute
* Signaling: TCP, 4-byte BE length prefix + INI-text payload
* SRT: libsrt 1.5.4 in `SRTT_FILE` (stream) mode with rendezvous
* HTTP-FLV: standard FLV over the SRT tunnel; AVCDecoderConfigurationRecord
  carries HEVC VPS/SPS/PPS

**Your Android consumer code does not need to change for the v1→v2 camera-firmware upgrade.**  The single-binary refactor on the camera side is invisible on the wire.
