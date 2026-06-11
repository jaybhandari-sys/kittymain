#!/bin/sh
# =============================================================================
# ambicam.sh — Production launcher for Augentix kitty (HC1703 family)
# =============================================================================
# Responsibilities:
#   1. Wait for filesystem + symlink libraries.
#   2. Bring up MQTT client (gets per-device config from the cloud).
#   3. Bring up provider_srt (the SRT P2P provider).
#   4. Monitor every 10 s: restart anything that died (exponential backoff
#      capped at 60 s; permanent give-up after 5 consecutive failures triggers
#      an MQTT alarm).
#   5. Once per minute: rotate logs (size-based) and check /mny/mtd/ipc free
#      space; if >=90 % full, emit alarm + truncate the oldest log.
#
# Differences from the original Eco-Series-Kitty ambicam.sh:
#   - Adds provider_srt to the monitored set.
#   - Adds exponential-backoff respawn (was: unbounded restart loop).
#   - Adds in-place log rotation that copy-truncates so the daemons don't
#     need a SIGHUP (busybox + uclibc systems usually don't have logrotate).
#   - Adds a free-space watchdog that fires before the FS goes 100 % full.
#   - Logs everything through a single function so format stays consistent.
#
# All paths and credentials come from /etc/jffs2/ambicam/P2Pambicam_min.ini
# which the MQTT client fetches from the cloud on first boot.
# =============================================================================

set -u
export TZ='GMT-5:30'

# ---------------------------------------------------------------------------- paths
AMBICAM_DIR="/mny/mtd/ipc/ambicam"
CONFIG_FILE="/etc/jffs2/ambicam/P2Pambicam_min.ini"
PROVIDER_SRT_CONF="${AMBICAM_DIR}/provider_srt.conf"

MQTT_BIN="${AMBICAM_DIR}/MQTT_vcamclient_Augentix"
P2P_BIN="${AMBICAM_DIR}/P2Pambicam"               # legacy libjuice provider
SRT_BIN="${AMBICAM_DIR}/provider_srt"             # new SRT provider

MQTT_LOG="${AMBICAM_DIR}/MQTT_vcamclient.log"
P2P_LOG="${AMBICAM_DIR}/P2Pambicam.log"
SRT_LOG="${AMBICAM_DIR}/provider_srt.log"
LAUNCHER_LOG="/tmp/ambicam.log"

# ---------------------------------------------------------------------------- knobs
CONFIG_WAIT_TIMEOUT=120         # seconds before giving up on cloud config
CONFIG_CHECK_INTERVAL=3
MONITOR_INTERVAL=10             # how often to check for dead processes
BACKOFF_MIN=2
BACKOFF_MAX=60
MAX_CONSECUTIVE_FAILS=5         # after this many failed restarts → alarm

# Log rotation thresholds (per file).  256 KB ≈ 30 minutes at sub-stream
# bitrate of provider_srt's INFO logs.  Keep 3 rotations = ~1.5 hr of history.
ROTATE_SIZE_BYTES=$((256 * 1024))
ROTATE_KEEP=3

# Free-space alarm threshold (percent).  /mny/mtd/ipc is a 1.5 MB jffs2
# partition shared by logs, configs, and the binaries — easy to fill.
FLASH_FULL_PCT=90

# ---------------------------------------------------------------------------- log helper
log() {
    # All output goes to /tmp/ambicam.log (tmpfs, survives until reboot) AND
    # to stdout so the boot console captures it on first boot.
    ts="$(date '+%Y-%m-%d %H:%M:%S')"
    line="$ts - $1"
    echo "$line"
    echo "$line" >> "$LAUNCHER_LOG" 2>/dev/null || true
}

mqtt_alarm() {
    # Best-effort alarm: ask the MQTT client to publish a system event.  If
    # MQTT is dead this is a no-op.  Topic is per the kitty/Augentix
    # convention.  Payload is JSON.
    payload="$1"
    [ -x /usr/bin/mosquitto_pub ] || return 0
    /usr/bin/mosquitto_pub \
        -h localhost -p 1883 \
        -t "kitty/$(hostname)/alarm" \
        -m "$payload" \
        -q 1 2>/dev/null || true
}

# ---------------------------------------------------------------------------- log rotation
rotate_one_log() {
    file="$1"
    [ -f "$file" ] || return 0
    size="$(wc -c < "$file" 2>/dev/null || echo 0)"
    [ "$size" -lt "$ROTATE_SIZE_BYTES" ] && return 0

    log "rotate: $file ($size bytes ≥ $ROTATE_SIZE_BYTES)"

    # Shift older rotations: file.2 → file.3 ; file.1 → file.2 ; etc.
    i=$ROTATE_KEEP
    while [ "$i" -gt 1 ]; do
        prev=$((i - 1))
        [ -f "${file}.${prev}.gz" ] && mv "${file}.${prev}.gz" "${file}.${i}.gz"
        i=$prev
    done

    # Copy current → file.1.gz and truncate.  copy-truncate so the daemon's
    # open file descriptor still works (no SIGHUP needed).  busybox gzip
    # writes to .gz and removes the input — so we have to use a temp file.
    cp -p "$file" "${file}.1" && gzip -9f "${file}.1"
    : > "$file"
}

rotate_all_logs() {
    for f in "$MQTT_LOG" "$P2P_LOG" "$SRT_LOG"; do
        rotate_one_log "$f"
    done
}

# ---------------------------------------------------------------------------- free-space watchdog
check_flash_space() {
    mount_pct="$(df /mny/mtd/ipc 2>/dev/null | awk 'NR==2 {gsub("%",""); print $5}')"
    case "$mount_pct" in
        ''|*[!0-9]*) return 0 ;;
    esac
    if [ "$mount_pct" -ge "$FLASH_FULL_PCT" ]; then
        log "ALARM: /mny/mtd/ipc at ${mount_pct}% (>= ${FLASH_FULL_PCT}%)"
        mqtt_alarm "{\"alarm\":\"flash_full\",\"pct\":${mount_pct},\"mount\":\"/mny/mtd/ipc\"}"
        # Force-rotate even if file isn't at size threshold — buy some headroom.
        for f in "$MQTT_LOG" "$P2P_LOG" "$SRT_LOG"; do
            [ -f "$f" ] && : > "$f"
        done
    fi
}

# ---------------------------------------------------------------------------- filesystem readiness
wait_for_filesystem() {
    log "waiting for ${AMBICAM_DIR} to mount..."
    n=0
    while [ ! -d "$AMBICAM_DIR" ]; do
        n=$((n + 1))
        if [ "$n" -ge 30 ]; then
            log "FATAL: ${AMBICAM_DIR} unavailable after 30 attempts"
            exit 1
        fi
        sleep 2
    done
    log "filesystem ready (${AMBICAM_DIR})"
}

# ---------------------------------------------------------------------------- library symlinks
create_symlinks() {
    log "creating library symlinks..."
    # Source:Target pairs.  Some libs are in /usr/lib, some bundled in AMBICAM_DIR.
    for pair in \
        "${AMBICAM_DIR}/libpaho-mqtt3cs.so.1.3.14:${AMBICAM_DIR}/libpaho-mqtt3cs.so.1" \
        "/usr/lib/libcrypto.so.3:${AMBICAM_DIR}/libcrypto.so.1.1" \
        "/usr/lib/libssl.so.3:${AMBICAM_DIR}/libssl.so.1.1" \
        "${AMBICAM_DIR}/libcurl.so.4.8.0:${AMBICAM_DIR}/libcurl.so.4"
    do
        src="${pair%%:*}"
        dst="${pair##*:}"
        if [ ! -f "$src" ]; then
            log "WARNING: missing $src"
            continue
        fi
        ln -sfn "$src" "$dst"
    done
    export LD_LIBRARY_PATH="${AMBICAM_DIR}"
}

# ---------------------------------------------------------------------------- internet probe
check_internet() {
    # Just confirm we can resolve+TCP to the cloud broker.  We don't fail
    # hard here — the MQTT client has its own reconnect.
    #
    # Parse mqttUrl out of config.json (real JSON, not key=value as the
    # original sed pattern assumed).  Pull the host[:port] between
    # "://" and the next "," / quote / end.
    host_port="$(sed -nE 's|.*"mqttUrl"[[:space:]]*:[[:space:]]*"[a-z]+://([^"/]+).*|\1|p' \
                 "${AMBICAM_DIR}/config.json" 2>/dev/null | head -1)"
    host="${host_port%%:*}"
    port="${host_port##*:}"
    [ "$port" = "$host" ] && port=1883
    [ -z "$host" ] && { host="mqtt-staging.devices.arcisai.io"; port=443; }
    log "internet probe: $host:$port"
    if ! (echo "" | nc -w 4 "$host" "$port" >/dev/null 2>&1); then
        log "WARN: cannot reach $host:$port yet (MQTT will retry)"
    fi
}

# ---------------------------------------------------------------------------- per-process state
# We track (pid, last_start_ts, consecutive_fails, current_backoff) per daemon.
mqtt_fails=0      ;  mqtt_backoff=$BACKOFF_MIN     ;  mqtt_next_try=0
p2p_fails=0       ;  p2p_backoff=$BACKOFF_MIN      ;  p2p_next_try=0
srt_fails=0       ;  srt_backoff=$BACKOFF_MIN      ;  srt_next_try=0

start_mqtt() {
    [ -x "$MQTT_BIN" ] || { log "MQTT bin not executable: $MQTT_BIN"; return 1; }
    log "starting MQTT_vcamclient_Augentix"
    (cd "$AMBICAM_DIR" && "$MQTT_BIN" >> "$MQTT_LOG" 2>&1 &) || return 1
    sleep 1
    pidof MQTT_vcamclient_Augentix >/dev/null
}

start_p2p() {
    [ -x "$P2P_BIN" ] || { log "P2Pambicam bin missing: $P2P_BIN"; return 1; }
    log "starting P2Pambicam (legacy libjuice)"
    (cd "$AMBICAM_DIR" && "$P2P_BIN" >> "$P2P_LOG" 2>&1 &) || return 1
    sleep 1
    pidof P2Pambicam >/dev/null
}

start_srt() {
    [ -x "$SRT_BIN" ] || { log "provider_srt bin missing: $SRT_BIN (SRT path disabled)"; return 2; }
    [ -f "$PROVIDER_SRT_CONF" ] || { log "$PROVIDER_SRT_CONF missing — SRT path disabled"; return 2; }
    log "starting provider_srt"
    (cd "$AMBICAM_DIR" && "$SRT_BIN" "$PROVIDER_SRT_CONF" >> "$SRT_LOG" 2>&1 &) || return 1
    sleep 1
    pidof provider_srt >/dev/null
}

# ---------------------------------------------------------------------------- wait-for-config
start_mqtt_and_wait_for_config() {
    start_mqtt || { log "MQTT failed to spawn"; return 1; }
    n=0
    while [ ! -f "$CONFIG_FILE" ]; do
        n=$((n + CONFIG_CHECK_INTERVAL))
        if [ "$n" -ge "$CONFIG_WAIT_TIMEOUT" ]; then
            log "FATAL: $CONFIG_FILE never appeared (waited ${n}s)"
            return 1
        fi
        log "waiting for cloud config..."
        sleep "$CONFIG_CHECK_INTERVAL"
    done
    log "cloud config arrived: $CONFIG_FILE"
}

# ---------------------------------------------------------------------------- monitor loop
monitor_step() {
    now="$(date +%s)"

    # MQTT-pushed provider_srt.conf rotation.  The MQTT_vcamclient touches
    # /tmp/provider_srt_conf_pushed each time it lands a new conf payload
    # from the broker (topic torque/rx/<sn>/81 — see ops/mqtt-provisioning.md).
    # When we see the sentinel, kill provider_srt — the respawn block below
    # picks up the new conf automatically.
    if [ -f /tmp/provider_srt_conf_pushed ]; then
        log "provider_srt.conf rotated by MQTT — restarting daemon to apply"
        # Atomic: remove the sentinel first so a race-with-write doesn't
        # leave us in an infinite restart loop.
        rm -f /tmp/provider_srt_conf_pushed
        # busybox on the Augentix camera ships kill+killall+pidof but NOT
        # pkill (verified on dev unit ATPL-200007-TESTA, v1.13.1).  The
        # original `pkill -TERM -f` here returned 127 silently because of
        # the `2>/dev/null` muffler — the case-81 push appeared to succeed
        # at every other layer but the daemon never actually restarted.
        # Use killall (busybox builtin) by basename.  Fall back to pidof
        # + kill if killall is somehow missing.
        if ! killall -TERM provider_srt 2>/dev/null; then
            srt_pids="$(pidof provider_srt 2>/dev/null)"
            if [ -n "$srt_pids" ]; then
                # shellcheck disable=SC2086  # intentional word-split for pid list
                kill -TERM $srt_pids 2>/dev/null || true
            fi
        fi
        # let the respawn-on-down branch below do the actual restart
    fi

    # MQTT
    if ! pidof MQTT_vcamclient_Augentix >/dev/null 2>&1; then
        if [ "$now" -ge "$mqtt_next_try" ]; then
            log "MQTT down — restarting (fail count $mqtt_fails, backoff ${mqtt_backoff}s)"
            if start_mqtt; then
                mqtt_fails=0; mqtt_backoff=$BACKOFF_MIN
            else
                mqtt_fails=$((mqtt_fails + 1))
                mqtt_backoff=$(( mqtt_backoff * 2 ))
                [ "$mqtt_backoff" -gt "$BACKOFF_MAX" ] && mqtt_backoff=$BACKOFF_MAX
                mqtt_next_try=$((now + mqtt_backoff))
                [ "$mqtt_fails" -ge "$MAX_CONSECUTIVE_FAILS" ] && \
                    mqtt_alarm "{\"alarm\":\"daemon_dead\",\"name\":\"MQTT_vcamclient_Augentix\",\"fails\":$mqtt_fails}"
            fi
        fi
    fi

    # legacy libjuice P2P (only restart if previously running — some
    # deployments don't ship it; absence is OK)
    if [ -x "$P2P_BIN" ] && ! pidof P2Pambicam >/dev/null 2>&1; then
        if [ "$now" -ge "$p2p_next_try" ]; then
            log "P2Pambicam down — restarting (fails $p2p_fails)"
            if start_p2p; then
                p2p_fails=0; p2p_backoff=$BACKOFF_MIN
            else
                p2p_fails=$((p2p_fails + 1))
                p2p_backoff=$(( p2p_backoff * 2 ))
                [ "$p2p_backoff" -gt "$BACKOFF_MAX" ] && p2p_backoff=$BACKOFF_MAX
                p2p_next_try=$((now + p2p_backoff))
                [ "$p2p_fails" -ge "$MAX_CONSECUTIVE_FAILS" ] && \
                    mqtt_alarm "{\"alarm\":\"daemon_dead\",\"name\":\"P2Pambicam\",\"fails\":$p2p_fails}"
            fi
        fi
    fi

    # SRT provider
    if [ -x "$SRT_BIN" ] && ! pidof provider_srt >/dev/null 2>&1; then
        if [ "$now" -ge "$srt_next_try" ]; then
            log "provider_srt down — restarting (fails $srt_fails)"
            if start_srt; then
                srt_fails=0; srt_backoff=$BACKOFF_MIN
            else
                srt_fails=$((srt_fails + 1))
                srt_backoff=$(( srt_backoff * 2 ))
                [ "$srt_backoff" -gt "$BACKOFF_MAX" ] && srt_backoff=$BACKOFF_MAX
                srt_next_try=$((now + srt_backoff))
                [ "$srt_fails" -ge "$MAX_CONSECUTIVE_FAILS" ] && \
                    mqtt_alarm "{\"alarm\":\"daemon_dead\",\"name\":\"provider_srt\",\"fails\":$srt_fails}"
            fi
        fi
    fi
}

# ---------------------------------------------------------------------------- main
log "============================================================"
log "ambicam.sh starting (production launcher)"
log "============================================================"

sleep 3                                   # let the kernel settle
wait_for_filesystem
create_symlinks
check_internet

if ! start_mqtt_and_wait_for_config; then
    log "MQTT/config phase failed once; retrying..."
    sleep 10
    if ! start_mqtt_and_wait_for_config; then
        log "FATAL: cannot obtain cloud config; exiting"
        exit 1
    fi
fi

# Spawn whichever P2P paths are available.  At least one should exist.
# NB: `A && B || C` isn't if-then-else (shellcheck SC2015) — be explicit.
if [ -x "$P2P_BIN" ]; then
    start_p2p || log "P2Pambicam first-spawn failed; monitor loop will retry"
else
    log "P2Pambicam absent — skipping"
fi
if [ -x "$SRT_BIN" ]; then
    start_srt || log "provider_srt first-spawn failed; monitor loop will retry"
else
    log "provider_srt absent — skipping"
fi

log "all available services launched; entering monitor loop"

# Rotate logs every minute even if not at size threshold.
loop_count=0
while true; do
    sleep "$MONITOR_INTERVAL"
    monitor_step

    loop_count=$((loop_count + 1))
    # Every minute (assuming MONITOR_INTERVAL=10): rotate logs + check space.
    if [ "$(( loop_count % 6 ))" -eq 0 ]; then
        rotate_all_logs
        check_flash_space
    fi
done
