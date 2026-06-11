# Reducing glass-to-glass latency

**Status**: research note.  Current measured E2E delay is **~2 s**.  Realistic floor with the existing camera firmware is **~500 ms**; reaching <300 ms requires camera-firmware changes outside our control.

## Budget today

| Term | Current | Floor reachable today | How to lower |
|---|---|---|---|
| Encoder + 1-frame delay | 67 ms @ 15 fps | 33 ms @ 30 fps | Camera UI / MQTT `frameRate=30` (CPU concern — load already 2.7 / 1 core) |
| GOP / first-keyframe wait (avg ½ GOP) | 1000 ms | 250 ms | Camera UI / MQTT `gop=0.5` (`keyFrameInterval=7`) |
| HTTP-FLV local read (loopback) | <5 ms | 5 ms | — |
| SRT send (camera side, FILE mode) | ~0 ms | 0 ms | TSBPD disabled in FILE → no buffer |
| Internet RTT/2 | ~25 ms | ~25 ms | Physics |
| Consumer SRT `RCVLATENCY` | 300 ms | 50 ms | `tools/augentix_view.py --latency-ms 50` |
| **Consumer FlvFanout prefix replay** | up to 2 MB (~250 s at sub-stream rate, drained via framedrop) | ~few KB (FLV header + onMetaData + HEVC config only) | Implement FLV-tag-aware prefix in `augentix_view.py` |
| ffplay decode + display | ~100 ms | ~80 ms | `-fflags nobuffer -flags low_delay -framedrop` (already set) |
| **Total** | **~2 s** | **~500 ms** | |

## Specific code-side fixes

### 1. `provider_srt.c`: wire `LATENCY_MS` through to libsrt

The daemon reads `LATENCY_MS=` from `provider_srt.conf` into `cfg->latency_ms` (line 157) but **never calls `srt_setsockopt(SRTO_LATENCY, …)`**.  One-line fix in `set_srt_options_caller()`:

```c
// After SRTO_PEERIDLETIMEO setsockopt (around line 346), add:
int latency = g_cfg.latency_ms;
if (srt_setsockopt(s, 0, SRTO_LATENCY,     &latency, sizeof(latency)) != 0) return -1;
if (srt_setsockopt(s, 0, SRTO_RCVLATENCY,  &latency, sizeof(latency)) != 0) return -1;
if (srt_setsockopt(s, 0, SRTO_PEERLATENCY, &latency, sizeof(latency)) != 0) return -1;
```

Won't directly lower latency (FILE mode already has TSBPD off → 0 ms) but exposes a real knob for tuning vs. loss-tolerance.

### 2. `tools/augentix_view.py`: FLV-tag-aware prefix cache

Currently caches the first **2 MB** of stream and replays it verbatim to each new HTTP-FLV consumer.  At sub-stream bitrate (62 kbps) this is 250 s of stale bytes.  Player catches up via framedrop but it costs the user ~1-2 s of confused playback.

Replace with a parser that captures:

1. FLV file header (`46 4C 56 01 …` — 13 bytes including the leading PrevTagSize0)
2. First **script tag** (`type=0x12`) — the `onMetaData` AMF object with width/height/codec/fps
3. First **video sequence header** (`type=0x09`, `frame_type=1`, `avc_packet_type=0`) — the HEVCDecoderConfigurationRecord with SPS+PPS

Total cache ≈ 500 bytes.  Live tail attaches right after, no stale-bytes confusion.  Implementation sketch in `tools/augentix_view.py` — the FLV tag format is documented in Adobe's `flv_file_format_spec_v10_1.pdf`.

### 3. Camera GOP via MQTT

The MQTT vcamclient log shows the cloud sends the encoder config:

```json
{"id":102, "keyFrameInterval":30, "gop":2, "frameRate":15, ...}
```

Pushing `{"keyFrameInterval":7,"gop":0.5}` halves the keyframe-wait latency.  This is a camera firmware concern — the encoder respects the value but the CPU cost grows.  Test on a single camera first; if CPU stays under load 2.0 / single-core, roll fleet-wide.

## Things that won't help

- Reducing `provider_srt`'s `LOCAL_BUFFER_SIZE` (currently 256 KB).  `recv()` returns as soon as data arrives — the buffer is a maximum, not a latency floor.
- Disabling SRT retransmission.  In FILE mode it's already minimal; turning it off causes desync mid-tag.
- Switching from SRT to WebRTC.  100-200 ms lower in theory but requires a full transport-layer rewrite.  Defer.

## Measurement protocol

Use the camera's OSD timestamp (burned into every frame) against the Mac's wall clock:

```
1. Open ffplay against the local proxy.
2. Take a phone-photo of the Mac screen with a stopwatch-app phone in
   frame, so the photo captures BOTH the OSD clock in the video AND the
   phone clock.
3. Difference = true glass-to-glass latency.
```

Repeat across 5 attaches to get a distribution — the GOP wait is uniformly distributed in [0, GOP] so single-shot measurements vary by up to a full GOP interval.
