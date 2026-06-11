#!/usr/bin/env bash
# =============================================================================
# cutover-v2.0.sh — one-shot v1.13.x → v2.0.0 config translator (OPERATOR-SIDE).
# =============================================================================
#
# This script runs ONCE per camera during v1.13.x → v2.0.0 cutover. After all
# cameras are migrated, this script should be DELETED from the repo (the
# translation table below is the only place in this codebase that knows the
# v1↔v2 mapping; we don't want it to leak into the camera or the new binary).
#
# v2.0 ARCHITECTURE NOTE
#   The v2.0 ap2p binary does NOT store the runtime config on the camera
#   filesystem.  Config is delivered to the camera as an MQTT **retained
#   message** on topic torque/rx/<SERIAL>/90.  The broker stores it; on every
#   subscribe (boot, reconnect) the broker auto-delivers it to the camera.
#   The camera applies it in memory only, no disk write.
#
#   So the "push" step here uses `mosquitto_pub --retain` — not a regular
#   publish.  The broker remembers the payload forever (or until you publish
#   a zero-byte retained message to that topic to clear it).
#
# Runs on the operator's laptop, NOT on the camera. Requires: expect, sed,
# grep, and (for --push-via-mqtt mode) mosquitto_pub.
#
# Usage:
#   ./cutover-v2.0.sh <SERIAL>                  # translate only, output local
#   ./cutover-v2.0.sh <SERIAL> --push-via-mqtt  # translate + publish retained
#   ./cutover-v2.0.sh --help
# =============================================================================

set -euo pipefail

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------
readonly CAMERA_HOST="${CAMERA_HOST:-192.168.12.129}"
readonly CAMERA_USER="${CAMERA_USER:-root}"
readonly CAMERA_PASS="${CAMERA_PASS:-adiance@999@arcisai}"
readonly REMOTE_CONF="/mny/mtd/ipc/ambicam/provider_srt.conf"
readonly BROKER_HOST="${BROKER_HOST:-mqtt-staging.devices.arcisai.io}"
readonly BROKER_PORT="${BROKER_PORT:-443}"
readonly BROKER_USER="${BROKER_USER:-Torque}"
readonly BROKER_PASS="${BROKER_PASS:-Raptor@0}"

# The 17 expected v2 keys after translation (used for the post-translation
# completeness check).  Order does not matter.
readonly EXPECTED_V2_KEYS=(
    NODE_ID CTRL_HOST CTRL_PORT CTRL_SCHEME CTRL_TOKEN
    EDGE_HOST EDGE_PORT
    RELAY_HOST RELAY_PORT RELAY_USER RELAY_PASS
    SRC_HOST SRC_PORT SRC_PATH SRC_AUTH
    LATENCY_MS VERBOSE
)

# ---------------------------------------------------------------------------
# Help
# ---------------------------------------------------------------------------
usage() {
    cat <<'EOF'
cutover-v2.0.sh — one-shot v1.13.x → v2.0.0 config translator

USAGE
    cutover-v2.0.sh <SERIAL>                   Fetch v1 conf from camera,
                                               translate to v2 schema, write
                                               result to /tmp/v2.conf.<SERIAL>.
                                               No publish step.

    cutover-v2.0.sh <SERIAL> --push-via-mqtt   Same as above, plus publish the
                                               translated conf to the broker on
                                               topic torque/rx/<SERIAL>/90
                                               WITH THE RETAIN FLAG SET, so the
                                               broker auto-delivers it to the
                                               camera on every subscribe.

    cutover-v2.0.sh --help                     Show this message.

ARGUMENTS
    SERIAL        Camera serial, format ATPL-NNNNNN-XXXX (e.g. ATPL-200007-TESTA).

ENVIRONMENT (override the defaults if needed)
    CAMERA_HOST   default: 192.168.12.129
    CAMERA_USER   default: root
    CAMERA_PASS   default: <baked-in staging password>
    BROKER_HOST   default: mqtt-staging.devices.arcisai.io
    BROKER_PORT   default: 443
    BROKER_USER   default: Torque
    BROKER_PASS   default: <baked-in staging password>

REQUIREMENTS
    expect, sed, grep, and (for --push-via-mqtt) mosquitto_pub.

EXIT CODES
    0  success
    1  bad arguments / missing dependency
    2  fetch from camera failed
    3  translation produced fewer than 17 expected keys (warn-only by default,
       but exits 3 if --strict is also set)
    4  publish via MQTT failed

NOTES
    This script is DISPOSABLE.  Once the fleet is migrated to v2.0.0, delete
    it from the repo — its translation table is the only place in this
    codebase that knows the v1↔v2 mapping, and we don't want that vocabulary
    to live on after cutover.
EOF
}

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
SERIAL=""
PUSH=0
STRICT=0
for arg in "$@"; do
    case "$arg" in
        -h|--help)           usage; exit 0 ;;
        --push-via-mqtt)     PUSH=1 ;;
        --strict)            STRICT=1 ;;
        -*)                  echo "unknown flag: $arg" >&2; usage >&2; exit 1 ;;
        *)
            if [ -z "$SERIAL" ]; then SERIAL="$arg"
            else echo "unexpected extra arg: $arg" >&2; exit 1
            fi
            ;;
    esac
done

if [ -z "$SERIAL" ]; then
    echo "error: SERIAL is required" >&2
    usage >&2
    exit 1
fi

# Validate serial format: ATPL-<digits>-<alnum>
if ! [[ "$SERIAL" =~ ^ATPL-[0-9]{4,}-[A-Za-z0-9]+$ ]]; then
    echo "error: serial '$SERIAL' is not of the form ATPL-NNNNNN-XXXX" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Dependency check
# ---------------------------------------------------------------------------
need() { command -v "$1" >/dev/null 2>&1 || { echo "error: missing required tool: $1" >&2; exit 1; }; }
need expect
need sed
need grep
if [ "$PUSH" -eq 1 ]; then need mosquitto_pub; fi

V1_FILE="/tmp/v1.conf.${SERIAL}"
V2_FILE="/tmp/v2.conf.${SERIAL}"

# ---------------------------------------------------------------------------
# Step 1: fetch v1 conf from the camera via telnet+expect
# ---------------------------------------------------------------------------
echo "[1/4] fetching ${REMOTE_CONF} from ${CAMERA_HOST}..."

# We use expect to drive telnet, mark output between MARK-START / MARK-END,
# and pull the body out with sed.  Same pattern as the other aug-*.exp helpers.
EXPECT_LOG="$(mktemp -t cutover-fetch.XXXXXX.log)"
trap 'rm -f "$EXPECT_LOG"' EXIT

if ! expect <<EOF > "$EXPECT_LOG"
set timeout 20
log_user 1
spawn telnet $CAMERA_HOST
expect "login:"; send "$CAMERA_USER\r"
expect "assword:"; send "$CAMERA_PASS\r"
expect "# "
send "PS1='zq> '\r"
expect "zq> "
send "echo MARK-START && cat $REMOTE_CONF && echo MARK-END\r"
expect "MARK-END"
expect "zq> "
send "exit\r"
expect eof
EOF
then
    echo "error: expect/telnet session failed (camera unreachable?)" >&2
    exit 2
fi

# Extract everything strictly between the markers.
sed -n '/MARK-START/,/MARK-END/{/MARK-START/d;/MARK-END/d;p;}' "$EXPECT_LOG" \
    | sed 's/\r$//' \
    > "$V1_FILE"

if [ ! -s "$V1_FILE" ]; then
    echo "error: extracted v1 conf is empty — see $EXPECT_LOG" >&2
    exit 2
fi
echo "       wrote $V1_FILE ($(wc -l < "$V1_FILE") lines)"

# ---------------------------------------------------------------------------
# Step 2: translate v1 → v2 (17 key rename rules)
# ---------------------------------------------------------------------------
echo "[2/4] translating to v2 schema..."

# Order matters where one key is a substring of another (e.g. SIGNALING_HOST
# vs SIGNALING_PORT / SIGNALING_SCHEME).  We anchor on the leading "^KEY=" to
# avoid collateral substitution inside comments or values.
sed -E \
    -e 's/^SERVICE_ID=/NODE_ID=/'             \
    -e 's/^SIGNALING_HOST=/CTRL_HOST=/'       \
    -e 's/^SIGNALING_PORT=/CTRL_PORT=/'       \
    -e 's/^SIGNALING_SCHEME=/CTRL_SCHEME=/'   \
    -e 's/^API_TOKEN=/CTRL_TOKEN=/'           \
    -e 's/^STUN_HOST=/EDGE_HOST=/'            \
    -e 's/^STUN_PORT=/EDGE_PORT=/'            \
    -e 's/^TURN_HOST=/RELAY_HOST=/'           \
    -e 's/^TURN_PORT=/RELAY_PORT=/'           \
    -e 's/^TURN_USERNAME=/RELAY_USER=/'       \
    -e 's/^TURN_PASSWORD=/RELAY_PASS=/'       \
    -e 's/^LOCAL_HOST=/SRC_HOST=/'            \
    -e 's/^LOCAL_PORT=/SRC_PORT=/'            \
    -e 's/^LOCAL_HTTP_PATH=/SRC_PATH=/'       \
    -e 's/^LOCAL_HTTP_AUTH=/SRC_AUTH=/'       \
    "$V1_FILE" > "$V2_FILE"

# Architecture note: there is NO on-camera config file in v2.0.  The payload
# we publish below is what the camera holds in memory.  When the operator
# updates the retained payload, all subscribing cameras pick up the change
# within seconds (case-81 hot reload).  When a camera reboots, it re-subscribes
# and the broker re-delivers the retained payload automatically.

echo "       wrote $V2_FILE ($(wc -l < "$V2_FILE") lines)"

# ---------------------------------------------------------------------------
# Step 3: verify all 17 expected v2 keys are present
# ---------------------------------------------------------------------------
echo "[3/4] verifying v2 key completeness..."
missing=()
for key in "${EXPECTED_V2_KEYS[@]}"; do
    if ! grep -qE "^${key}=" "$V2_FILE"; then
        missing+=("$key")
    fi
done

if [ "${#missing[@]}" -gt 0 ]; then
    echo "WARN: ${#missing[@]} expected key(s) missing from $V2_FILE:" >&2
    for k in "${missing[@]}"; do echo "        - $k" >&2; done
    echo "      (camera may have had an older or incomplete v1 conf)" >&2
    if [ "$STRICT" -eq 1 ]; then
        echo "      --strict was set → aborting" >&2
        exit 3
    fi
else
    echo "       all 17 keys present"
fi

# ---------------------------------------------------------------------------
# Step 4: optional push via MQTT
# ---------------------------------------------------------------------------
if [ "$PUSH" -eq 1 ]; then
    # v2.0.7+ — retained-config topic moved from /81 to /90 because the
    # ArcisAI Kitty QC Tool uses /81 for tampering-detect.  Clear /81
    # first (zero-byte retained → broker drops it), then publish to /90.
    echo "[4a/4] clearing legacy retained on torque/rx/${SERIAL}/81 (pre-v2.0.7 topic)..."
    mosquitto_pub \
        -h "$BROKER_HOST" -p "$BROKER_PORT" \
        -u "$BROKER_USER" -P "$BROKER_PASS" \
        -t "torque/rx/${SERIAL}/81" \
        -q 1 --retain -n  ||  true
    echo "[4b/4] publishing (retained) to ${BROKER_HOST}:${BROKER_PORT} (topic torque/rx/${SERIAL}/90)..."
    if ! mosquitto_pub \
            -h "$BROKER_HOST" -p "$BROKER_PORT" \
            -u "$BROKER_USER" -P "$BROKER_PASS" \
            -t "torque/rx/${SERIAL}/90" \
            -q 1 --retain -f "$V2_FILE"; then
        echo "error: mosquitto_pub failed" >&2
        exit 4
    fi
    echo "       published $(wc -c < "$V2_FILE") bytes (broker will auto-deliver on every subscribe)"
else
    echo "[4/4] --push-via-mqtt not set → translation-only run, no publish"
    echo "       review:  cat $V2_FILE"
    echo "       publish: $0 $SERIAL --push-via-mqtt"
fi

echo "done."
