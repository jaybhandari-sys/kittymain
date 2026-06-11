#!/bin/sh
# view-feed.sh — one-shot live viewer for the Augentix dev camera.
#
# NOTE: The ARCISAI_API_TOKEN / ARCISAI_VERIFY_TOKEN defaults below are
# appropriate for the dev camera on GCP staging (see
# ops/dev-integration-guide.md).  Production deployments use different,
# rotated credentials — override via env vars in that case.
#
# Does (in order):
#   1. Kill any leftover augentix_view.py tester (each viewer needs a
#      *fresh* SRT pump — otherwise ffplay/ffmpeg sees the "prefix replay
#      then jump to live" boundary, which produces "Packet mismatch" +
#      "DTS discontinuity" errors and an unwatchable picture).
#   2. Spawn a fresh augentix_view.py in the background.  Its log lands
#      at /tmp/augview.log.
#   3. Wait until the SRT handshake completes (log line "[srt] connected").
#   4. ffplay the local HTTP-FLV endpoint with the safe low-latency flags.
#      ~2 s glass-to-glass.
#
# Required env (defaults pulled from the on-camera provider_srt.conf):
#   ARCISAI_API_TOKEN    — signaling-server registration token
#   ARCISAI_VERIFY_TOKEN — local HTTP-FLV verify=... query token
#   SERVICE_ID           — camera serial (default ATPL-200007-TESTA)
#   CHANNEL_PATH         — /flv/live_ch0_0.flv (main) or _1.flv (sub).
#                          The .139 LAN device tends to camp on _1, so we
#                          default to _0 to avoid contention.
#
# Usage:
#   sh view-feed.sh                       # default: ~2-sec safe latency
#   sh view-feed.sh raw                   # no probe limits (compat)
#   CHANNEL_PATH=/flv/live_ch0_1.flv sh view-feed.sh
#   URL=http://127.0.0.1:8080/live.flv sh view-feed.sh   # legacy mode:
#                                          # use a pre-launched augview
#                                          # (sets SKIP_RESTART implicitly)
set -e

URL="${URL:-http://127.0.0.1:8080/live.flv}"

# ---- legacy "URL=… view-feed.sh" path: assume tester already running ----
# Old behaviour for users who already started augview manually.
if [ -n "${SKIP_RESTART:-}" ] || [ "${URL%:8080/live.flv}" != "http://127.0.0.1" ]; then
    case "$1" in
      raw) exec ffplay -hide_banner -loglevel info "$URL" ;;
      *)   exec ffplay -hide_banner -loglevel info \
                       -fflags nobuffer -flags low_delay -framedrop \
                       "$URL" ;;
    esac
fi

# ---- normal path: kill old pump, spawn fresh, wait, ffplay ----
SERVICE_ID="${SERVICE_ID:-ATPL-200007-TESTA}"
CHANNEL_PATH="${CHANNEL_PATH:-/flv/live_ch0_0.flv}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TESTER="$SCRIPT_DIR/augentix_view.py"

ARCISAI_API_TOKEN="${ARCISAI_API_TOKEN:-p2p-server-api-token-change-me}"
ARCISAI_VERIFY_TOKEN="${ARCISAI_VERIFY_TOKEN:-a%2Fb4Znt%2BOFGrYtmHw0T16Q%3D%3D}"
echo "[view-feed] using staging defaults for ARCISAI_API_TOKEN / ARCISAI_VERIFY_TOKEN; override via env vars if you need different values"

echo "[view-feed] killing any prior augentix_view.py..."
pkill -f "augentix_view.py" 2>/dev/null || true
# Wait for the listening socket to free (macOS holds it briefly).
for _ in 1 2 3 4 5 6 7 8 9 10; do
    if ! lsof -i :8080 -P -n 2>/dev/null | grep -q LISTEN; then
        break
    fi
    sleep 0.5
done

echo "[view-feed] spawning fresh tester (service=$SERVICE_ID channel=$CHANNEL_PATH)..."
: > /tmp/augview.log
( cd "$SCRIPT_DIR/.." && \
  ARCISAI_API_TOKEN="$ARCISAI_API_TOKEN" \
  ARCISAI_VERIFY_TOKEN="$ARCISAI_VERIFY_TOKEN" \
  nohup python3 "$TESTER" \
      --service-id "$SERVICE_ID" \
      --channel-path "$CHANNEL_PATH" \
      > /tmp/augview.log 2>&1 < /dev/null &
)
AUGVIEW_PID=$!
echo "[view-feed] tester pid=$AUGVIEW_PID; waiting for SRT handshake..."

# Wait up to 20 s for "first FLV bytes" (means SRT is up AND camera responded
# with FLV header — the actual prerequisite for ffplay).
WAITED=0
while [ "$WAITED" -lt 40 ]; do
    if grep -q "first FLV bytes" /tmp/augview.log 2>/dev/null; then
        break
    fi
    sleep 0.5
    WAITED=$((WAITED + 1))
done
if ! grep -q "first FLV bytes" /tmp/augview.log 2>/dev/null; then
    echo "[view-feed] FLV bytes never arrived from camera; tester log:"
    cat /tmp/augview.log
    exit 2
fi

echo "[view-feed] SRT up, FLV flowing.  Launching ffplay..."
case "$1" in
  raw)
    exec ffplay -hide_banner -loglevel info "$URL"
    ;;
  *)
    # nobuffer + low_delay + framedrop = ~2 s glass-to-glass.  Adding
    # -infbuf or -sync ext made latency WORSE in earlier experiments
    # (the buffer grew unbounded).
    exec ffplay -hide_banner -loglevel info \
                -fflags nobuffer -flags low_delay -framedrop \
                "$URL"
    ;;
esac
