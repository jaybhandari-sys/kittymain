# Stream startup latency — measurement plan for v2.0.6

> Owner: this is a v2.0.6 design doc; instrumentation lands then.

## What we're measuring

User reports ~2 s delay between `view-feed.sh` saying `SRT up, FLV
flowing` and the first decoded frame appearing in `ffplay`.  Goal: find
out which of the 5 stages in the video path owns the 2 s, fix the
dominant one.

## The video path

```
+--------------------+              +-------------------------+              +------------+
|  Augentix encoder  | --(local)--> |  Camera HTTP-FLV server | --(local)--> |   ap2p     |
|  (sensor → ISP)    |              |  /flv/live_ch0_0.flv    |              |  pump+SRT  |
+--------------------+              +-------------------------+              +-----+------+
                                                                                   |
                                                                              SRT (NAT-traversed)
                                                                                   |
                                                                                   v
                                              +--------+                +----------+----------+
                                              | ffplay | <--decode--    |  viewer (Python)    |
                                              | render |  & buffer      |  augentix_view.py   |
                                              +--------+                +---------------------+
```

5 places latency can hide:

1. **Encoder first-IDR** — encoder pauses on idle; resumes on demand.
   1 GOP ≈ 1 s of wait at typical 1 fps I-frame cadence.
2. **HTTP-FLV server first-byte** — camera's HTTP server may wait for
   the next I-frame before sending any FLV body.
3. **ap2p HTTP-source pump** — libcurl `CURLOPT_NOSIGNAL` + TCP-connect
   + HTTP GET; should be 50–100 ms LAN-local.
4. **SRT handshake + first packet** — rendezvous, 4-way; 200–500 ms.
5. **ffplay prebuffer** — defaults to 200–1000 ms.

## Instrumentation (v2.0.6 work)

### Camera side (new in `src/ap2p/stream_timing.{c,h}`)

```c
typedef enum {
    ST_VIEWER_CONNECT_NEW = 0,   /* signaling sends us a new peer  */
    ST_SRT_HANDSHAKE_START,      /* srt_create + srt_connect       */
    ST_SRT_HANDSHAKE_DONE,       /* SRT_CONNECTED state            */
    ST_HTTP_GET_OPEN,            /* TCP connect to local HTTP done */
    ST_HTTP_FIRST_BYTE,          /* first body byte read           */
    ST_FIRST_FLV_PUSHED,         /* first SRT send completes       */
    ST__COUNT
} st_milestone_t;

void st_session_begin(const char *peer_id);
void st_mark(st_milestone_t m);
void st_session_publish(void *mqtt_client, const char *node_id,
                        const char *peer_id);
```

On the camera, every viewer session publishes one JSON to
`torque/tx/<sn>/stream/<peer_id>` with all six milestones (deltas from
`ST_VIEWER_CONNECT_NEW`) plus the camera's `CLOCK_MONOTONIC` at session
start, so we can correlate to host clocks.

### Viewer side (extend `tools/augentix_view.py`)

```python
import time
TIMINGS = {}
def stamp(label):
    TIMINGS[label] = time.monotonic()
# stamp at:
#   't_start'                 — script main()
#   't_sig_connect_ok'        — signaling-server says "found provider"
#   't_srt_handshake_done'    — SRT socket reaches connected state
#   't_first_flv_byte'        — first byte arrives on the recv socket
#   't_ffplay_spawned'        — Popen(ffplay) returned
#   't_first_decoded_frame'   — parse ffplay stderr for "Stream #" line
```

Dump to `/tmp/viewer_session.json` per run.

### NTP-sync

Camera + host must agree on wall clock within ~50 ms before measurement.
Camera runs `ntpdate` (or has chrony) at boot; host is whatever
mac/linux defaults are.  Cross-check via the camera's reported
`CLOCK_MONOTONIC` against a fresh MQTT publish — gives us a stable
offset for the session.

## Acceptance for v2.0.6

- One session's JSON shows where the 2 s goes.
- A fix lands on the dominant stage.
- New baseline: first-decoded-frame ≤ 500 ms after `SRT up`.

## Likely fixes (preview)

If stage 5 (ffplay prebuffer) is the culprit:
- `ffplay -fflags nobuffer -flags low_delay -framedrop -strict experimental`
- Pair with `-probesize 32 -analyzeduration 0` for fastest demux start.

If stage 2 (FLV server) is the culprit:
- Tune `SRC_PATH` to a channel with shorter GOP (live_ch0_1.flv vs
  ch0_0.flv).  Already configurable per-device via the retained MQTT
  payload — no code change needed.
- Or: ask Augentix for an FLV-server option to flush partial GOP.

If stage 4 (SRT handshake) is the culprit:
- Pre-warm the SRT socket: bind + listen in srt_thread BEFORE the
  signaling REGISTER, so when a viewer hits us the handshake is
  half-done already.

If stage 1 (encoder first-IDR) is the culprit:
- Configure encoder to emit I-frame on socket-accept (Augentix-side).
- Or: keep encoder always running, FLV server just gates on next IDR
  (this is closer to what most IPCs do).
