#!/bin/sh
# view-feed-fast.sh — fast-path viewer.  Pipes SRT-rendezvous'd FLV bytes
# directly into ffplay's stdin.  No FlvFanout prefix replay, no local HTTP
# server, no port races.
#
# Why this is dramatically faster than view-feed.sh:
#
#   view-feed.sh path:
#     STUN          ~0.1 s
#     signaling     ~0.7 s
#     SRT rdv'd     ~0.5 s
#     first byte    ~0.6 s
#     ─────────────────────  ≈ 1.9 s of network work
#     pkill old, wait for :8080 free, spawn aug_view, wait 0–20 s for
#     "first FLV bytes" log line, then ffplay attaches to HTTP-FLV…
#     ffplay reads 256 KB prefix replay (≈ 5 s of stale stream at this
#     bitrate) BEFORE catching up to live.
#     ─────────────────────  ≈ 5–10 s of viewer-side ceremony
#                              + a noticeable "stale catch-up" period.
#
#   view-feed-fast.sh path:
#     STUN          ~0.1 s    \
#     signaling     ~0.7 s     | total ≈ 1.9 s.
#     SRT rdv'd     ~0.5 s    /
#     first byte    ~0.6 s    /
#     ffplay reads stdin directly — first frame decoded on the spot.
#
# Net cold-start saving: roughly 4–6 seconds vs view-feed.sh.
#
# Usage (CAMERA-AGNOSTIC):
#   view-feed-fast.sh ATPL-405858-AUGEN            # by Device ID
#   view-feed-fast.sh --ip 192.168.12.32           # by camera IP (auto-detects SN)
#   view-feed-fast.sh 192.168.12.32                # same — IPv4 positional
#   SERVICE_ID=ATPL-xxxxx view-feed-fast.sh        # via env
#   CHANNEL_PATH=/flv/live_ch0_1.flv view-feed-fast.sh ATPL-... # main stream
#
# Args + Env:
#   <positional>          either a DeviceID (ATPL-..., VSPL-..., etc.) or IPv4.
#   --ip <addr>           camera IP — script GETs /netsdk/system/deviceinfo
#                         and extracts serialNumber as SERVICE_ID.
#   --service-id <id>     explicit override (skip auto-detect).
#   --channel-path <p>    /flv/live_ch0_0.flv (sub, default) or _1.flv (main).
#   --latency-ms <ms>     SRT TSBPD buffer (default 300).
#   ARCISAI_API_TOKEN     signaling registration token
#   ARCISAI_VERIFY_TOKEN  HTTP-FLV ?verify=... token (URL-encoded)
#   SERVICE_ID            camera SN  (auto-detected if blank)
#   CAMERA_IP             same as --ip
#   CAMERA_HTTP_PASSWORD  HTTP admin pwd (default empty)
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PIPE_SCRIPT="$SCRIPT_DIR/feed-direct.py"

# CAMERA-AGNOSTIC: SERVICE_ID is auto-detected from the camera's vendor
# NetSDK if not provided.  Three ways to specify a target:
#
#   1. Positional Device ID    : view-feed-fast.sh ATPL-405858-AUGEN
#   2. Camera IP   ( --ip X )  : view-feed-fast.sh --ip 192.168.12.32
#   3. Environment             : SERVICE_ID=... or CAMERA_IP=... or both
#
# When only IP is given, we hit /netsdk/system/deviceinfo and pick the
# serialNumber field (matches the format /^[A-Z]{4}-\d+-[A-Z0-9]+$/).
SERVICE_ID="${SERVICE_ID:-}"
CAMERA_IP="${CAMERA_IP:-}"
CAMERA_HTTP_PASSWORD="${CAMERA_HTTP_PASSWORD:-}"

# Parse args.  First positional that looks like ATPL/VSPL/etc. → SERVICE_ID.
# --ip <addr> or positional IPv4 → CAMERA_IP.
while [ $# -gt 0 ]; do
    case "$1" in
        --ip)             CAMERA_IP="$2"; shift 2 ;;
        --service-id)     SERVICE_ID="$2"; shift 2 ;;
        --channel-path)   CHANNEL_PATH="$2"; shift 2 ;;
        --latency-ms)     LATENCY_MS="$2"; shift 2 ;;
        -h|--help)
            sed -n '2,20p' "$0"; exit 0 ;;
        *)
            # Pattern-match: looks like an ID or an IPv4?
            if echo "$1" | grep -qE '^[A-Z]{4}-[0-9]+-[A-Z0-9]+$'; then
                SERVICE_ID="$1"
            elif echo "$1" | grep -qE '^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$'; then
                CAMERA_IP="$1"
            else
                echo "view-feed-fast: unrecognised arg: $1" >&2
                echo "usage: view-feed-fast.sh [<DeviceID> | --ip <ip>]" >&2
                exit 2
            fi
            shift
            ;;
    esac
done

# Auto-detect SERVICE_ID from CAMERA_IP if SERVICE_ID still empty.
# Retries up to 4× because WLAN bench networks (500-ms RTT + packet
# loss) frequently drop the first HTTP request — observed in practice
# on the dev bench where attempt 1 times out and attempts 2–5 all
# succeed.
if [ -z "$SERVICE_ID" ] && [ -n "$CAMERA_IP" ]; then
    for attempt in 1 2 3 4; do
        SERVICE_ID=$(curl -s -u "admin:$CAMERA_HTTP_PASSWORD" \
                        --connect-timeout 8 --max-time 15 \
                        "http://$CAMERA_IP/netsdk/system/deviceinfo" 2>/dev/null \
                     | python3 -c '
import sys, json, re
try:
    j = json.load(sys.stdin)
except Exception:
    sys.exit(0)
pat = re.compile(r"^[A-Z]{4}-\d+-[A-Z0-9]+$")
for k in ("serialNumber", "extSN2"):
    v = (j.get(k) or "").strip()
    if pat.match(v):
        print(v); break
' 2>/dev/null)
        if [ -n "$SERVICE_ID" ]; then
            break
        fi
        if [ "$attempt" -lt 4 ]; then
            sleep 2
        fi
    done
fi

if [ -z "$SERVICE_ID" ]; then
    echo "view-feed-fast: no SERVICE_ID and could not auto-detect." >&2
    echo "  options:" >&2
    echo "    view-feed-fast.sh ATPL-NNNNNN-XXXXX" >&2
    echo "    view-feed-fast.sh --ip 192.168.12.32" >&2
    echo "    SERVICE_ID=ATPL-NNNNNN-XXXXX view-feed-fast.sh" >&2
    exit 2
fi
echo "view-feed-fast: SERVICE_ID=$SERVICE_ID${CAMERA_IP:+  (resolved from $CAMERA_IP)}" >&2

CHANNEL_PATH="${CHANNEL_PATH:-/flv/live_ch0_0.flv}"
LATENCY_MS="${LATENCY_MS:-300}"
ARCISAI_API_TOKEN="${ARCISAI_API_TOKEN:-p2p-server-api-token-change-me}"
ARCISAI_VERIFY_TOKEN="${ARCISAI_VERIFY_TOKEN:-a%2Fb4Znt%2BOFGrYtmHw0T16Q%3D%3D}"

# Kill any prior fast-path or legacy viewer.  The pipe form below makes
# port conflicts impossible (no HTTP server) but a leftover SRT bind on
# the same UDP port would interfere with rendezvous.
pkill -f feed-direct.py 2>/dev/null || true
pkill -f augentix_view.py 2>/dev/null || true
sleep 0.2

export ARCISAI_API_TOKEN ARCISAI_VERIFY_TOKEN

# Strict pipe: feed-direct.py writes FLV bytes to stdout; ffplay reads stdin.
# ffplay options:
#   -fflags nobuffer    — disable input buffering, present frames ASAP
#   -flags low_delay    — H.264/HEVC low-delay decode hint
#   -framedrop          — drop frames if we fall behind (don't compound lag)
#   -probesize 32       — minimal init-probe, since first bytes ARE the init
#   -analyzeduration 0  — same idea
#   -i pipe:0           — read from stdin
# Pipeline architecture: feed-direct.py → ffplay (direct FLV).
#
# Wrapped in an auto-restart loop: when the SRT tunnel drops, the
# signaling server rotates the provider, ap2p restarts on the camera,
# or any other transient closes the pipe, feed-direct.py exits, ffplay
# sees EOF and exits, and the loop respawns the pipeline after a 1-s
# backoff.  Ctrl+C still cleanly exits via the trap.
#
# ffplay options (each one matters — added in response to a specific
# failure mode seen on the bench):
#
#   -fflags +nobuffer+discardcorrupt+genpts
#       nobuffer        present frames ASAP (no input buffering)
#       discardcorrupt  drop bad packets BEFORE they reach the decoder
#       genpts          regenerate PTS so a/v re-sync after drops
#   -flags low_delay    H.264/HEVC low-delay decode hint
#   -framedrop          drop late frames so we don't compound lag
#   -err_detect ignore_err  keep decoding past errors instead of freezing
#   -skip_frame nokey   on HEVC mid-GOP tune-in, skip non-IDR frames
#                       until the first IDR arrives (kills the
#                       "Could not find ref with POC N" flood).
#   -probesize 200000 -analyzeduration 1000000
#       Previously we used `-probesize 32 -analyzeduration 0` to
#       minimise cold-start latency.  Side effect: ffplay couldn't
#       read the FLV onMetaData tag (which carries the framerate=15),
#       guessed 30 fps as default, then with -framedrop dropped 28/30
#       frames → visible playback at ~0.5 fps.  200 KB / 1 s of probe
#       is plenty to find both onMetaData and the AAC + HEVC sequence
#       headers but only adds ~150 ms to cold start.
#   -f flv -i pipe:0    force the input demuxer (don't guess)
#
# What we deliberately do NOT set:
#   -autoexit           used to be on; made ffplay quit on first pipe
#                       close.  Now we let the OUTER while loop restart
#                       the pipeline instead.
#   -vsync 0            ffplay 8.1 rejects this; vsync is muxer-only.

# Ctrl+C / SIGTERM cleanly exits the outer loop.
restart_count=0
running=1
trap 'running=0; kill 0 2>/dev/null; exit 0' INT TERM

while [ "$running" -eq 1 ]; do
    if [ "$restart_count" -gt 0 ]; then
        echo "view-feed-fast: pipeline ended; restart #${restart_count} ..." >&2
    fi
    python3 "$PIPE_SCRIPT" \
            --service-id "$SERVICE_ID" \
            --channel-path "$CHANNEL_PATH" \
            --latency-ms "$LATENCY_MS" \
            2>/tmp/feed-direct.log \
        | ffplay -hide_banner -loglevel warning \
                 -fflags +nobuffer+discardcorrupt+genpts \
                 -flags low_delay -framedrop \
                 -err_detect ignore_err \
                 -skip_frame nokey \
                 -probesize 200000 -analyzeduration 1000000 \
                 -f flv -i pipe:0
    rc=$?
    # If the user closed the ffplay window, rc=0 — don't restart.
    if [ "$rc" -eq 0 ] || [ "$running" -ne 1 ]; then
        break
    fi
    restart_count=$((restart_count + 1))
    sleep 1
done
