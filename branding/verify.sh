#!/bin/sh
# branding/verify.sh
#
# v2.0 brand-leak gate.  Checks that a built ap2p binary does NOT carry
# upstream library banners (OpenSSL/libcurl/Paho/libsrt/libjuice/Augentix)
# or v1.x brand strings (SIGNALING_HOST/STUN_/TURN_/SERVICE_ID/provider_srt),
# and that its ELF NEEDED set is a subset of the allowlist in
# branding/manifest.toml.
#
# Usage:  branding/verify.sh <binary-path>
# Exits non-zero with a diagnostic on the first failed check.
#
# This script is intentionally tolerant about how it parses manifest.toml:
# the allowed-NEEDED and forbidden-token lists are duplicated as shell
# variables below so the script works on any minimal POSIX shell with
# `strings` and `readelf` (both available in binutils).  If you change
# the lists in manifest.toml, mirror them here.
set -eu

BIN="${1:-}"
if [ -z "$BIN" ]; then
    echo "verify.sh: missing argument" >&2
    echo "Usage: $0 <binary-path>" >&2
    exit 2
fi
if [ ! -f "$BIN" ]; then
    echo "verify.sh: $BIN does not exist" >&2
    exit 2
fi
if [ ! -x "$BIN" ]; then
    echo "verify.sh: $BIN is not executable" >&2
    exit 2
fi

# ---- (1) Forbidden token scan over `strings` output -----------------------
# Two groups: case-INsensitive (upstream library banners) and case-SENSITIVE
# (legacy upper-case config keys; lower-case `service_id=` etc. are wire-
# protocol field names and intentionally allowed).
# Patterns are anchored to BANNER syntax — version separators (/, space,
# dash) — so they catch e.g. "libcurl/7.83.1", "libpaho-mqtt-c/1.3.x",
# "OpenSSL 3.0.16 ..." but NOT the DT_NEEDED sonames like "libcurl.so.4"
# (which uClibc's dynamic loader matches byte-for-byte; preserved on
# purpose, see verify.sh:ALLOWED_NEEDED).
FORBIDDEN_I='OpenSSL [0-9]|libcurl/|libcurl-|Eclipse Paho|libpaho-mqtt-c/|libpaho-mqtt-c-|paho-mqtt-c/|Mosquitto|libsrt/|libsrt v|libjuice/|libjuice-|libevent/|libevent-|Augentix|GCC:'
FORBIDDEN_C='SIGNALING_HOST|STUN_HOST|STUN_PORT|STUN_USERNAME|STUN_PASSWORD|TURN_HOST|TURN_PORT|TURN_USERNAME|TURN_PASSWORD|SERVICE_ID|provider_srt'

if ! command -v strings >/dev/null 2>&1; then
    echo "verify.sh: 'strings' not found in PATH" >&2
    exit 2
fi

LEAKS_I=$(strings "$BIN" | grep -iE  "$FORBIDDEN_I" || true)
LEAKS_C=$(strings "$BIN" | grep  -E  "$FORBIDDEN_C" || true)
LEAKS="$LEAKS_I$LEAKS_C"
if [ -n "$LEAKS" ]; then
    echo "verify.sh: FAIL — forbidden brand tokens found in $BIN:" >&2
    echo "$LEAKS" | sed 's/^/    /' >&2
    exit 1
fi

# ---- (2) NEEDED allowlist via readelf -d ----------------------------------
# Mirror of branding/manifest.toml [allowed_needed_libs].allowed.
ALLOWED_NEEDED='libc.so.0 ld-uClibc.so.1 libgcc_s.so.1 libatomic.so.1
                libap2p_tls.so.1 libap2p_crypto.so.1 libap2p_msg.so.1 libap2p_net.so.1
                libpaho-mqtt3cs.so.1 libcurl.so.4 libssl.so.3 libcrypto.so.3
                libssl.so.1.1 libcrypto.so.1.1'

if ! command -v readelf >/dev/null 2>&1; then
    echo "verify.sh: 'readelf' not found in PATH" >&2
    exit 2
fi

# readelf -d output line for NEEDED looks like:
#   0x00000001 (NEEDED)   Shared library: [libc.so.0]
NEEDED_LIBS=$(readelf -d "$BIN" 2>/dev/null \
    | awk '/\(NEEDED\)/ { match($0, /\[[^]]+\]/); if (RSTART) print substr($0, RSTART+1, RLENGTH-2) }')

BAD=""
for lib in $NEEDED_LIBS; do
    allowed=0
    for a in $ALLOWED_NEEDED; do
        if [ "$lib" = "$a" ]; then
            allowed=1
            break
        fi
    done
    if [ "$allowed" -eq 0 ]; then
        BAD="$BAD $lib"
    fi
done

if [ -n "$BAD" ]; then
    echo "verify.sh: FAIL — disallowed NEEDED entries in $BIN:" >&2
    for lib in $BAD; do echo "    $lib" >&2; done
    echo "verify.sh: allowed set is: $ALLOWED_NEEDED" >&2
    exit 1
fi

echo "verify.sh: OK — $BIN passes brand-leak checks"
exit 0
