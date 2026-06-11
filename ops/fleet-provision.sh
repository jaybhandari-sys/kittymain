#!/usr/bin/env bash
# =============================================================================
# fleet-provision.sh — push per-device v2 runtime config to N cameras over MQTT
# =============================================================================
#
# OPERATOR-SIDE.  Runs on a laptop / jumpbox with network reach to the MQTT
# broker.  Does NOT touch the cameras directly.  Each retained-message
# publish lands in broker memory; cameras receive their own payload the
# next time they subscribe (boot, reconnect, or right now if they're online).
#
# This is the v2 equivalent of "deploy config.json + provider_srt.conf to
# every camera in the batch" — but instead of touching every camera, you
# touch the broker once per camera.  For a 2500-unit batch this script
# takes ~30 s to run end to end (a few ms per mosquitto_pub).
#
# Usage:
#     fleet-provision.sh <fleet.csv> [--dry-run]
#
# fleet.csv format (header row required):
#     serial,verify_token,latency_ms,verbose
#     ATPL-200001-TESTA,abcd1234...,300,1
#     ATPL-200002-TESTA,efgh5678...,300,1
#     ATPL-200003-TESTA,ijkl9012...,300,0
#     ...
#
# Other fields (CTRL_HOST, EDGE_HOST, RELAY_HOST/USER/PASS, SRC_HOST/PORT,
# SRC_AUTH) are shared across the fleet and read from environment.  Override
# any of them per-environment:
#
#     CTRL_HOST=signaling.devices.arcisai.io \
#     CTRL_PORT=80 \
#     RELAY_PASS='replace-me' \
#     fleet-provision.sh fleet.csv
#
# What happens to each camera:
#   1. ap2p (running) is subscribed to torque/rx/<NODE_ID>/#.
#   2. The instant this script publishes torque/rx/<NODE_ID>/81 with --retain,
#      the broker holds the payload AND forwards a copy to the camera (if it's
#      online).
#   3. ap2p.mqtt_thread.message_arrived() → apply_ap2p_conf_payload().
#   4. State-ready flips; SRT thread comes alive and registers with
#      signaling using the new config.
#   5. Camera reboots? On reconnect to the broker, the retained payload is
#      auto-delivered again.  No re-publish needed.
#
# To clear a camera's config (e.g., decommission a unit), publish a zero-
# byte retained payload:
#     mosquitto_pub … --retain -t "torque/rx/<NODE_ID>/81" -n
# =============================================================================

set -euo pipefail

# ---------------------------------------------------------------------------
# Fleet-wide defaults (override per env)
# ---------------------------------------------------------------------------
BROKER_HOST="${BROKER_HOST:-mqtt-staging.devices.arcisai.io}"
BROKER_PORT="${BROKER_PORT:-443}"
BROKER_USER="${BROKER_USER:-Torque}"
BROKER_PASS="${BROKER_PASS:?BROKER_PASS env var required (do not bake into the script)}"

CTRL_HOST="${CTRL_HOST:-signaling.devices.arcisai.io}"
CTRL_PORT="${CTRL_PORT:-80}"
CTRL_SCHEME="${CTRL_SCHEME:-plain}"
CTRL_TOKEN="${CTRL_TOKEN:?CTRL_TOKEN env var required}"

EDGE_HOST="${EDGE_HOST:-turn.devices.arcisai.io}"
EDGE_PORT="${EDGE_PORT:-5349}"

RELAY_HOST="${RELAY_HOST:-turn.devices.arcisai.io}"
RELAY_PORT="${RELAY_PORT:-5349}"
RELAY_USER="${RELAY_USER:-arcisai}"
RELAY_PASS="${RELAY_PASS:?RELAY_PASS env var required}"

SRC_HOST="${SRC_HOST:-127.0.0.1}"
SRC_PORT="${SRC_PORT:-80}"
SRC_AUTH="${SRC_AUTH:-Basic YWRtaW46}"      # admin: (HTTP Basic)
# SRC_PATH and LATENCY_MS / VERBOSE come from the per-device CSV row.

# ---------------------------------------------------------------------------
# Arg parsing
# ---------------------------------------------------------------------------
CSV=""
DRY=0
for arg in "$@"; do
    case "$arg" in
        -h|--help)
            sed -n '4,/^# =====/p' "$0" | sed -n '/^# ====/q;p' | sed 's/^# \{0,1\}//'
            exit 0 ;;
        --dry-run) DRY=1 ;;
        -*) echo "unknown flag: $arg" >&2; exit 1 ;;
        *)
            if [ -z "$CSV" ]; then CSV="$arg"
            else echo "extra arg: $arg" >&2; exit 1
            fi ;;
    esac
done
[ -n "$CSV" ] || { echo "usage: $0 <fleet.csv> [--dry-run]" >&2; exit 1; }
[ -f "$CSV" ] || { echo "csv not found: $CSV" >&2; exit 1; }

command -v mosquitto_pub >/dev/null || { echo "missing: mosquitto_pub" >&2; exit 1; }

# ---------------------------------------------------------------------------
# Process every row.  CSV header is consumed and validated.
# ---------------------------------------------------------------------------
echo "fleet-provision: broker=$BROKER_HOST:$BROKER_PORT  ctrl=$CTRL_HOST:$CTRL_PORT  csv=$CSV  dry_run=$DRY"
TMP=$(mktemp -t fleet-prov.XXXX.txt)
trap "rm -f $TMP" EXIT

count_ok=0
count_skip=0
count_fail=0
row=0

# Read CSV with a FIXED column order (works on macOS bash 3.2):
#   serial , verify_token , latency_ms , verbose , src_path
# Columns 3-5 are optional; defaults are 300, 0, and live_ch0_1.flv.
{
    IFS=, read -r -a header
    # Strip CRs (some CSVs are CRLF)
    for i in "${!header[@]}"; do header[$i]="${header[$i]%$'\r'}"; done

    # Confirm the first column is 'serial' — that's the only hard requirement.
    [ "${header[0]}" = "serial" ] || {
        echo "csv: first column must be 'serial' (got '${header[0]}')" >&2
        echo "expected header: serial,verify_token,latency_ms,verbose,src_path" >&2
        exit 1
    }

    while IFS=, read -r -a row_data; do
        row=$((row + 1))
        for i in "${!row_data[@]}"; do row_data[$i]="${row_data[$i]%$'\r'}"; done

        serial="${row_data[0]:-}"
        [ -n "$serial" ] || { count_skip=$((count_skip + 1)); continue; }

        verify_token="${row_data[1]:-REPLACE_WITH_VERIFY_TOKEN}"
        latency_ms="${row_data[2]:-300}"
        verbose="${row_data[3]:-0}"
        src_path="${row_data[4]:-/flv/live_ch0_1.flv?verify=${verify_token}}"

        # Compose payload
        cat > "$TMP" <<EOF
NODE_ID=$serial
CTRL_HOST=$CTRL_HOST
CTRL_PORT=$CTRL_PORT
CTRL_SCHEME=$CTRL_SCHEME
CTRL_TOKEN=$CTRL_TOKEN
EDGE_HOST=$EDGE_HOST
EDGE_PORT=$EDGE_PORT
RELAY_HOST=$RELAY_HOST
RELAY_PORT=$RELAY_PORT
RELAY_USER=$RELAY_USER
RELAY_PASS=$RELAY_PASS
SRC_HOST=$SRC_HOST
SRC_PORT=$SRC_PORT
SRC_PATH=$src_path
SRC_AUTH=$SRC_AUTH
LATENCY_MS=$latency_ms
VERBOSE=$verbose
EOF

        if [ "$DRY" -eq 1 ]; then
            printf "[%5d] %-22s  dry-run\n" "$row" "$serial"
            continue
        fi

        # v2.0.7+ — retained-config topic moved from /81 to /90 because the
        # ArcisAI Kitty QC Tool reserves /81 for tampering-detect GET.
        # For backwards compatibility during the cutover window, we ALSO
        # publish a zero-byte retained payload to /81 — this clears any
        # stale retained config from older fleet-provision runs so v2.0.7+
        # cameras don't bind onto the legacy topic and trigger the tampering
        # handler.  Old (pre-v2.0.7) cameras receive 0 bytes, which they
        # interpret as no-op (their handler short-circuits on empty body).
        if mosquitto_pub \
                -h "$BROKER_HOST" -p "$BROKER_PORT" \
                -u "$BROKER_USER" -P "$BROKER_PASS" \
                -t "torque/rx/${serial}/81" \
                -q 1 --retain -n 2>/dev/null && \
           mosquitto_pub \
                -h "$BROKER_HOST" -p "$BROKER_PORT" \
                -u "$BROKER_USER" -P "$BROKER_PASS" \
                -t "torque/rx/${serial}/90" \
                -q 1 --retain -f "$TMP" \
                2>/dev/null; then
            count_ok=$((count_ok + 1))
            # Progress every 100 cameras
            if [ "$((count_ok % 100))" -eq 0 ]; then
                echo "  progress: $count_ok rows published"
            fi
        else
            count_fail=$((count_fail + 1))
            echo "  ✗ $serial (row $row) — mosquitto_pub failed" >&2
        fi
    done
} < "$CSV"

echo "fleet-provision: rows=$row  published=$count_ok  skipped=$count_skip  failed=$count_fail"
exit "$([ "$count_fail" -gt 0 ] && echo 1 || echo 0)"
