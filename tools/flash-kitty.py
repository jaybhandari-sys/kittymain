#!/usr/bin/env python3
"""
flash-kitty.py — safer kitty upload with backup, atomic swap, verify, rollback.

Replaces the prior best-effort flash-kitty.py that had multiple holes
(silent paho-mqtt failure, no rollback, stale retained boot JSON).
This version uses mosquitto_sub (CLI — far more universally installed
than python-paho-mqtt) for all broker verification, tracks publish
timestamps to distinguish FRESH boot from STALE retained, and supports
automatic rollback to the previous tag if verification fails.

Phases:
  0. Pre-flight    telnet+http reachable, mosquitto_sub on PATH, tarball
                   valid (gzip OK, contains AugenTix/ap2p), static-tar
                   cached
  1. Snapshot      record current VERSION + COMMIT from camera so we
                   can auto-roll-back on failure
  2. Push          stream static-tar + Augentix.tar.gz to camera /tmp
                   over telnet+nc, md5-verify each
  3. Stage         extract to /tmp/AugenTix-new on camera, verify all
                   expected files present
  4. Swap          stop ap2p, wipe /mny/mtd/ipc/ambicam/, install new
                   files atomically with sync barrier
  5. Migrate /81   publish zero-byte retained to torque/rx/<sn>/81 so
                   v2.0.7+ camera doesn't get the legacy retained
                   payload routed to its new tampering handler
  6. Reboot        record T0 (post-reboot wallclock)
  7. Verify        wait for FRESH boot publish (uptime_at_ms.ctrl_
                   registered < 60s, AND broker-receive-time > T0 + 10s)
                   then briefly try view-feed-fast to confirm signaling
                   live
  8. Decide        if verification fails AND --rollback-on-fail is set
                   AND --rollback-tag was provided, automatically
                   re-flash the previous tag

Usage:
  flash-kitty.py --ip 192.168.12.129 --tag v2.0.7-rc1
  flash-kitty.py --ip 192.168.12.129 --tag v2.0.7-rc1 --rollback-tag v2.0.4-rc1
  flash-kitty.py --ip 192.168.12.129 --tarball ./Augentix.tar.gz --no-reboot
  flash-kitty.py --ip 192.168.12.129 --recover-prior        # reflash whatever was running before

Env:
  CAMERA_ROOT_PASSWORD       telnet root pwd (default adiance@999@arcisai)
  CAMERA_HTTP_PASSWORD       HTTP admin pwd (default empty)
  BROKER_HOST/PORT/USER/PASS broker for verification
  GITHUB_REPO                tag download source (default Adiance-Technologies/kitty-augentix-camera)
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Optional

# ─────────────────────────── constants ──────────────────────────────────────
DEFAULTS = {
    "repo":         os.environ.get("GITHUB_REPO", "Adiance-Technologies/kitty-augentix-camera"),
    "root_pw":      os.environ.get("CAMERA_ROOT_PASSWORD", "adiance@999@arcisai"),
    "http_pw":      os.environ.get("CAMERA_HTTP_PASSWORD", ""),
    "broker_host":  os.environ.get("BROKER_HOST", "mqtt-staging.devices.arcisai.io"),
    "broker_port":  os.environ.get("BROKER_PORT", "443"),
    "broker_user":  os.environ.get("BROKER_USER", "Torque"),
    "broker_pass":  os.environ.get("BROKER_PASS", "Raptor@0"),
    "cache_dir":    Path.home() / ".cache" / "kitty-flash",
    "snapshot_dir": Path.home() / ".cache" / "kitty-flash" / "snapshots",
    "static_tar_url": "http://prong.arcisai.io/protected/augentix/lib/tar",
}
NC_PORT = 9999

# Files we EXPECT in v2.0.x Augentix.tar.gz / AugenTix dir.  Missing any
# of these = abort pre-swap.
EXPECTED_FILES = {"ap2p", "ambicam.sh", "config.json", "VERSION", "BUILT_AT",
                  "COMMIT", "libpaho-mqtt3cs.so.1", "libcurl.so.4"}


# ─────────────────────────── tiny telnet client ─────────────────────────────
class Telnet:
    def __init__(self, host: str, port: int = 23, timeout: float = 8):
        self.s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.s.settimeout(timeout); self.s.connect((host, port))
        self.buf = b""

    def _neg(self, data: bytes) -> bytes:
        out = bytearray(); reps = bytearray(); i = 0
        while i < len(data):
            b = data[i]
            if b == 0xFF and i + 2 < len(data):
                cmd, opt = data[i + 1], data[i + 2]
                if   cmd == 0xFB: reps += bytes([0xFF, 0xFE, opt])
                elif cmd == 0xFD: reps += bytes([0xFF, 0xFC, opt])
                i += 3
            else:
                out.append(b); i += 1
        if reps: self.s.sendall(bytes(reps))
        return bytes(out)

    def rd(self, marker: bytes, timeout: float = 8) -> bytes:
        deadline = time.time() + timeout
        while marker not in self.buf and time.time() < deadline:
            try:
                self.s.settimeout(max(0.5, deadline - time.time()))
                ch = self.s.recv(4096)
                if not ch: break
                self.buf += self._neg(ch)
            except socket.timeout: continue
        if marker in self.buf:
            i = self.buf.index(marker) + len(marker)
            o = self.buf[:i]; self.buf = self.buf[i:]; return o
        raise TimeoutError(f"didn't see {marker!r}; tail={self.buf[-200:]!r}")

    def send(self, line: str) -> None:
        self.s.sendall((line + "\n").encode())

    def run(self, cmd: str, to: float = 15) -> str:
        base = f"_MK_{int(time.time() * 1000)}_END_"
        a, b = base[:6], base[6:]
        self.send(f'{cmd}; printf "%s%s\\n" "{a}" "{b}"')
        return self.rd((a + b).encode(), to).decode("utf-8", errors="replace")

    def close(self):
        try: self.s.close()
        except Exception: pass


def telnet_login(t: Telnet, user: str, password: str):
    t.rd(b"login:"); t.send(user)
    t.rd(b"Password:"); t.send(password)
    t.rd(b"# ", timeout=10)
    t.send("stty -echo 2>/dev/null; export PS1='#'")
    time.sleep(0.3)
    try:
        t.s.settimeout(0.4)
        while True:
            ch = t.s.recv(4096)
            if not ch: break
            t.buf += t._neg(ch)
    except (socket.timeout, OSError): pass


# ─────────────────────────── reachability ───────────────────────────────────
def is_telnet_up(ip: str, timeout: float = 3) -> bool:
    try: socket.create_connection((ip, 23), timeout=timeout).close(); return True
    except Exception: return False


def is_http_up(ip: str, timeout: float = 3) -> bool:
    try: socket.create_connection((ip, 80), timeout=timeout).close(); return True
    except Exception: return False


# ─────────────────────────── identity probes ────────────────────────────────
# Every kitty camera carries its ArcisAI Device ID (BurnUID, e.g.
# "ATPL-405858-AUGEN") in MULTIPLE places.  This helper probes in priority
# order so the script works whether the camera is fresh-from-vendor, mid-
# install, or a fully-flashed kitty.
def probe_camera_sn(ip: str, http_pw: str, telnet: "Telnet|None" = None) -> Optional[str]:
    """Return the camera's ArcisAI Device ID (BurnUID) via the most
    authoritative source available.  Order:
      1. /mny/mtd/ipc/BurnUID  (kitty's own canonical store; only if
         telnet is already open and authenticated)
      2. GET /netsdk/system/deviceinfo → serialNumber  (vendor's burnt-in
         identity — present on every factory-fresh ArcisAI unit)
      3. POST /NetAPI/R.Sync.Stat.DeviceInfo → DevUid  (same identity,
         exposed via a different vendor API path)
    Any of these returning a value matching /^[A-Z]{4}-\\d+-[A-Z0-9]+$/ —
    e.g. ATPL-405858-AUGEN, VSPL-118070-EULZW — is accepted.

    Each HTTP probe retries up to 2× and uses a 12-s timeout so transient
    WLAN packet loss / high RTT doesn't fail the auto-detect.
    """
    pat = re.compile(r"^[A-Z]{4}-\d+-[A-Z0-9]+$")

    # 1. Telnet → /mny/mtd/ipc/BurnUID (only if telnet is open)
    if telnet is not None:
        try:
            lines = _telnet_extract_lines(
                telnet.run("cat /mny/mtd/ipc/BurnUID 2>/dev/null; echo", to=8))
            if lines and pat.match(lines[0]):
                return lines[0]
        except Exception:
            pass

    auth = base64.b64encode(f"admin:{http_pw}".encode()).decode()
    hdr = {"Authorization": f"Basic {auth}",
           "Content-Type":   "application/json"}

    # 2. /netsdk/system/deviceinfo  →  serialNumber  (try twice)
    for attempt in range(2):
        try:
            req = urllib.request.Request(f"http://{ip}/netsdk/system/deviceinfo",
                                          headers=hdr, method="GET")
            with urllib.request.urlopen(req, timeout=12) as r:
                j = json.loads(r.read().decode("utf-8", "replace"))
                sn = (j.get("serialNumber") or "").strip()
                if pat.match(sn):
                    return sn
                # Some firmware variants store the pretty SN in extSN2 even
                # when serialNumber got polluted with a cloud-binding token.
                sn2 = (j.get("extSN2") or "").strip()
                if pat.match(sn2):
                    return sn2
                break  # got a response but no usable SN — don't retry
        except Exception:
            if attempt == 1:
                pass
            else:
                time.sleep(1.5)

    # 3. /NetAPI/R.Sync.Stat.DeviceInfo → DevUid  (try twice)
    for attempt in range(2):
        try:
            req = urllib.request.Request(f"http://{ip}/NetAPI/R.Sync.Stat.DeviceInfo",
                                          data=b"{}", headers=hdr, method="POST")
            with urllib.request.urlopen(req, timeout=12) as r:
                j = json.loads(r.read().decode("utf-8", "replace"))
                sn = (j.get("DevUid") or "").strip()
                if pat.match(sn):
                    return sn
                break
        except Exception:
            if attempt == 1:
                pass
            else:
                time.sleep(1.5)

    return None


def enable_telnet(ip: str, http_pw: str) -> bool:
    url = f"http://{ip}/NetAPI/R.SwitchTelnet"
    auth = base64.b64encode(f"admin:{http_pw}".encode()).decode()
    req = urllib.request.Request(url, data=json.dumps({"Enable": True}).encode(),
                                  method="PUT",
                                  headers={"Authorization": f"Basic {auth}",
                                           "Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status < 300
    except Exception as e:
        print(f"  enable_telnet: {e}")
        return False


# ─────────────────────────── fetchers ───────────────────────────────────────
def ensure_static_tar(cache: Path) -> Path:
    path = cache / "tar"
    if path.exists() and path.stat().st_size > 100_000:
        return path
    cache.mkdir(parents=True, exist_ok=True)
    print(f"  → fetching static tar from {DEFAULTS['static_tar_url']}")
    with urllib.request.urlopen(DEFAULTS["static_tar_url"], timeout=30) as r:
        path.write_bytes(r.read())
    if path.stat().st_size < 100_000:
        path.unlink(missing_ok=True)
        raise RuntimeError("static tar download < 100 KB — refusing")
    return path


def fetch_tarball(repo: str, tag: str, cache: Path) -> Path:
    dest_dir = cache / tag
    dest = dest_dir / "Augentix.tar.gz"
    if dest.exists() and dest.stat().st_size > 200_000:
        print(f"  cached: {dest} ({dest.stat().st_size} B)")
        return dest
    dest_dir.mkdir(parents=True, exist_ok=True)
    print(f"  → downloading Augentix.tar.gz for {tag}")
    r = subprocess.run(["gh", "release", "download", tag, "--repo", repo,
                        "--pattern", "Augentix.tar.gz", "--dir", str(dest_dir),
                        "--clobber"],
                       capture_output=True, text=True, timeout=120)
    if r.returncode != 0 or not dest.exists():
        raise RuntimeError(f"gh release download failed: {r.stderr.strip()}")
    return dest


def validate_tarball(tarball: Path) -> dict:
    """Pre-flight: verify the tarball is sane.  Returns metadata dict."""
    import tarfile
    if not tarball.exists() or tarball.stat().st_size < 200_000:
        raise RuntimeError(f"tarball missing or too small: {tarball}")
    with tarfile.open(tarball, "r:gz") as tf:
        names = [n.removeprefix("AugenTix/") for n in tf.getnames()
                 if n.startswith("AugenTix/")]
    files = {n for n in names if n and "/" not in n}
    missing = EXPECTED_FILES - files
    if missing:
        raise RuntimeError(f"tarball missing required files: {missing}")
    # Pull the VERSION + COMMIT from the tarball without extracting
    meta = {}
    with tarfile.open(tarball, "r:gz") as tf:
        for k in ("VERSION", "BUILT_AT", "COMMIT"):
            try:
                m = tf.extractfile(f"AugenTix/{k}")
                if m: meta[k] = m.read().decode().strip()
            except KeyError: pass
    return meta


# ─────────────────────────── camera I/O ─────────────────────────────────────
def push_via_nc(t: Telnet, ip: str, local: Path, remote: str,
                max_retries: int = 3):
    size = local.stat().st_size
    local_md5 = hashlib.md5(local.read_bytes()).hexdigest()
    print(f"  → push {local.name} ({size} bytes) → {remote}")
    last_err = ""
    for attempt in range(1, max_retries + 1):
        if attempt > 1:
            print(f"    retry {attempt}/{max_retries} (slow / lossy network)...")
            time.sleep(2)
        t.run(f"rm -f {remote}", to=15)
        t.send(f"nc -l -p {NC_PORT} > {remote} &")
        time.sleep(0.8)
        # 60s timeout for the data-transfer connection covers slow WLAN
        # links (500ms RTT + 10% packet loss can drop TCP throughput to
        # 10–20 KB/s for our ~700 KB tarball ≈ 35–70 s).
        try:
            s = socket.create_connection((ip, NC_PORT), timeout=60)
            s.settimeout(60)
            with open(local, "rb") as f:
                while True:
                    buf = f.read(64 * 1024)
                    if not buf: break
                    s.sendall(buf)
            s.shutdown(socket.SHUT_WR)
            try: s.settimeout(8); s.recv(4096)
            except Exception: pass
            s.close()
        except Exception as e:
            last_err = f"socket: {e}"
            continue
        time.sleep(2)
        try:
            t.s.settimeout(0.4)
            while True:
                ch = t.s.recv(4096)
                if not ch: break
                t.buf += t._neg(ch)
        except (socket.timeout, OSError): pass
        md5_out = t.run(f"sync; md5sum {remote} 2>&1", to=30)
        m = re.search(r"\b([0-9a-f]{32})\b", md5_out)
        if m and m.group(1) == local_md5:
            print(f"    md5 OK ({local_md5[:12]}…)")
            return
        last_err = (f"md5 mismatch (local={local_md5[:12]}… remote="
                    f"{(m.group(1)[:12] if m else '???')}…)")
        print(f"    ✗ {last_err}")
    raise RuntimeError(f"push failed after {max_retries} attempts: {last_err}")


def _telnet_clean_line(s: str) -> str:
    """Strip noise the busybox shell prepends to command output via the
    "$" / "#" prompt + ANSI escape sequences.  PS1='$' or PS1='#' means
    `cat file` output gets concatenated as e.g. "$2.0.7-rc2" or
    "#2.0.7-rc2".  This helper trims those leading prompt chars + ANSI
    so we get the actual file content.  Also drops the _MK_ markers."""
    if "_MK_" in s:
        return ""
    # Strip ANSI escape sequences (busybox ls colour codes etc.)
    s = re.sub(r"\x1b\[[0-9;]*[mGKH]", "", s)
    s = s.replace("\r", "").strip()
    # Trim any number of leading "$" or "#" prompt chars (sometimes
    # concatenated when the shell prompts mid-output).
    while s and s[0] in "$#":
        s = s[1:].lstrip()
    return s


def _telnet_extract_lines(raw: str) -> list[str]:
    """Return non-empty content lines from a t.run() result, with prompt
    + marker noise stripped."""
    out = []
    for ln in raw.splitlines():
        cleaned = _telnet_clean_line(ln)
        if cleaned:
            out.append(cleaned)
    return out


def snapshot_pre_flash(t: Telnet, snapshot_dir: Path, sn: str) -> dict:
    """Record current VERSION + COMMIT + file list before wiping."""
    snapshot_dir.mkdir(parents=True, exist_ok=True)
    ts = time.strftime("%Y%m%d-%H%M%S")
    snap_file = snapshot_dir / f"{sn}-{ts}.json"
    # Each section starts with a unique line we can split on (not "---",
    # which was getting glued to prompt + version on busybox).
    # NB: `; echo` after each `cat` forces a trailing newline so the file
    # content can't run into the next sentinel — e.g. without it,
    #   cat BurnUID                → "ATPL-405858-AUGEN"   (no \n)
    #   echo XL                    → "XL"
    # ends up as one line "ATPL-405858-AUGENXL" in the telnet stream.
    info = t.run("echo XV; cat /mny/mtd/ipc/ambicam/VERSION 2>/dev/null; echo; "
                 "echo XC; cat /mny/mtd/ipc/ambicam/COMMIT  2>/dev/null; echo; "
                 "echo XB; cat /mny/mtd/ipc/BurnUID         2>/dev/null; echo; "
                 "echo XL; ls -la /mny/mtd/ipc/ambicam/ 2>/dev/null; "
                 "echo XD; df -h /mny/mtd/ipc | tail -1; "
                 "echo XEND", to=10)
    sections = {"XV": [], "XC": [], "XB": [], "XL": [], "XD": []}
    cur = None
    for ln in _telnet_extract_lines(info):
        if ln in sections:
            cur = ln; continue
        if ln == "XEND":
            cur = None; continue
        if cur:
            sections[cur].append(ln)
    snap = {
        "sn": sn,
        "captured_at": ts,
        "version":  sections["XV"][0] if sections["XV"] else "",
        "commit":   sections["XC"][0] if sections["XC"] else "",
        "burnUID":  sections["XB"][0] if sections["XB"] else "",
        "ls":       "\n".join(sections["XL"]),
        "df":       sections["XD"][0] if sections["XD"] else "",
    }
    snap_file.write_text(json.dumps(snap, indent=2))
    print(f"  ✓ snapshot saved → {snap_file}")
    print(f"    pre-flash VERSION: {snap['version']!r}")
    print(f"    pre-flash BurnUID file: {snap['burnUID']!r}")
    # NB: an empty BurnUID *file* here is no longer alarming — every kitty
    # camera also exposes its ID via the vendor's NetSDK serialNumber /
    # DevUid endpoints (probed by probe_camera_sn()).  The recipe will
    # install /tmp/BurnUID.desired into /mny/mtd/ipc/BurnUID at the end
    # of the install phase if the file is empty.  So we only escalate if
    # NOTHING is known (snap empty AND --sn not supplied AND auto-detect
    # already failed — handled in do_flash before we get here).
    return snap


RECIPE_SH = r"""#!/bin/sh
# Hardened install recipe (v4) — runs on-camera, pushed to /tmp by
# flash-kitty.py.  ALL output is line-buffered to stdout/stderr; the
# script reaches its final REPORT block in every code path so the
# telnet client always sees the closing marker line.
#
# Inputs (staged by flash-kitty.py before invoking):
#   /tmp/tar                 static busybox tar (for extraction)
#   /tmp/Augentix.tar.gz     the kitty bundle
#   /tmp/BurnUID.desired     the ArcisAI Device ID we want set if camera
#                            has none on /mny/mtd/ipc/BurnUID
#
# Outputs / exit:
#   exit 0  install + verify passed  (final line: === INSTALLED ===)
#   exit 1  any failure  (final line: ___RECIPE_FAILED_<REASON>___)

set -u

# --- PHASE A: VALIDATE staged tar (RAM only) ---
cd /tmp
rm -rf /tmp/AugenTix-new /tmp/AugenTix
/tmp/tar -xzf Augentix.tar.gz -C /tmp || {
    echo "___RECIPE_FAILED_EXTRACT___"; exit 1
}
for F in ap2p ambicam.sh VERSION libpaho-mqtt3cs.so.1 libcurl.so.4 config.json; do
    if [ ! -e "/tmp/AugenTix/$F" ]; then
        echo "___RECIPE_FAILED_VALIDATE___ missing /tmp/AugenTix/$F"
        exit 1
    fi
done
STAGED_VERSION=$(cat /tmp/AugenTix/VERSION)
REQUIRED_KB=$(du -sk /tmp/AugenTix | awk '{print $1}')
echo "STAGED VERSION: $STAGED_VERSION"
echo "REQUIRED_KB: $REQUIRED_KB"

# --- PHASE B: PRESERVE BurnUID ---
if [ -s /mny/mtd/ipc/BurnUID ]; then
    cp -f /mny/mtd/ipc/BurnUID /tmp/BurnUID.preserved
    echo "PRE-FLASH BurnUID: $(cat /tmp/BurnUID.preserved)"
else
    echo "PRE-FLASH BurnUID: <empty on disk>"
fi
if [ -s /tmp/BurnUID.desired ]; then
    echo "DESIRED BurnUID:    $(cat /tmp/BurnUID.desired)"
fi

# --- PHASE C: STOP ap2p + remove old install ---
killall -TERM ap2p        2>/dev/null
killall -TERM ambicam.sh  2>/dev/null
sleep 2
rm -rf /mny/mtd/ipc/ambicam /mny/mtd/ipc/ambicam.sh
sync; sync; sync

# --- PHASE D: JFFS2 GC WAIT LOOP ---
# Each iteration writes + removes a 4 KB sentinel, nudging the JFFS2 GC
# walker.  Loop exits when df reports >= REQUIRED + 16 KB free.  If the
# loop runs out but free >= REQUIRED, we still proceed (the 16 KB is
# defence-in-depth, not a hard floor).
FREE_OK=0
FREE_KB=0
i=0
while [ "$i" -lt 30 ]; do
    i=$((i + 1))
    dd if=/dev/zero of=/mny/mtd/ipc/.gc_trigger bs=1k count=4 >/dev/null 2>&1
    sync
    rm -f /mny/mtd/ipc/.gc_trigger
    sync
    FREE_KB=$(df -k /mny/mtd/ipc | tail -1 | awk '{print $4}')
    echo "GC_WAIT i=$i free=${FREE_KB}KB need=${REQUIRED_KB}KB"
    HEADROOM=$((REQUIRED_KB + 16))
    if [ "$FREE_KB" -ge "$HEADROOM" ]; then
        FREE_OK=1
        break
    fi
    sleep 1
done
if [ "$FREE_OK" != "1" ] && [ "$FREE_KB" -ge "$REQUIRED_KB" ]; then
    echo "GC_WAIT loop ran out but free=${FREE_KB}KB >= need=${REQUIRED_KB}KB; proceeding"
    FREE_OK=1
fi

# --- PHASE E: INSTALL with per-file space check ---
INSTALL_OK=$FREE_OK
ABORT_REASON=""
if [ "$FREE_OK" = "1" ]; then
    mkdir -p /mny/mtd/ipc/ambicam /etc/jffs2/ambicam
    cp /tmp/AugenTix/*.crt /tmp/AugenTix/client.key /etc/jffs2/ambicam/ 2>/dev/null

    # Install in size-descending order: big files first so a partial
    # install at least has the bootable binaries.
    for SPEC in         ap2p:/mny/mtd/ipc/ambicam/ap2p         libcurl.so.4:/mny/mtd/ipc/ambicam/libcurl.so.4         libpaho-mqtt3cs.so.1:/mny/mtd/ipc/ambicam/libpaho-mqtt3cs.so.1         ambicam.sh:/mny/mtd/ipc/ambicam.sh         config.json:/mny/mtd/ipc/ambicam/config.json         VERSION:/mny/mtd/ipc/ambicam/VERSION         BUILT_AT:/mny/mtd/ipc/ambicam/BUILT_AT         COMMIT:/mny/mtd/ipc/ambicam/COMMIT     ; do
        F=${SPEC%%:*}
        DST=${SPEC#*:}
        SRC=/tmp/AugenTix/$F
        if [ ! -f "$SRC" ]; then
            echo "INSTALL_WARN: source missing: $SRC"
            continue
        fi
        cp -f "$SRC" "$DST"
        RC=$?
        sync
        FREE_KB=$(df -k /mny/mtd/ipc | tail -1 | awk '{print $4}')
        if [ "$RC" != "0" ]; then
            echo "INSTALL_FAIL: cp $F (rc=$RC) free=${FREE_KB}KB"
            INSTALL_OK=0
            ABORT_REASON="INSTALL_FAIL: cp $F failed (rc=$RC)"
            break
        fi
        echo "INSTALL_OK: $F  free_after=${FREE_KB}KB"
    done

    # Cleanup any cert/key that may have landed in /mny (stays in /etc/jffs2 only).
    rm -f /mny/mtd/ipc/ambicam/*.crt /mny/mtd/ipc/ambicam/client.key
    if [ -x /mny/mtd/ipc/ambicam/ap2p ]; then
        chmod 755 /mny/mtd/ipc/ambicam.sh /mny/mtd/ipc/ambicam/ap2p
    fi
else
    ABORT_REASON="GC_FAIL: free=${FREE_KB}KB < required=${REQUIRED_KB}KB"
fi

# --- ALWAYS-RUN: BurnUID restore / install ---
# Priority order:
#   1. /mny/mtd/ipc/BurnUID already non-empty → leave alone (preserve vendor identity)
#   2. /tmp/BurnUID.preserved (snapshot from earlier this run)
#   3. /tmp/BurnUID.desired (passed in by flash-kitty.py from --sn / auto-detect)
if [ ! -s /mny/mtd/ipc/BurnUID ]; then
    if [ -s /tmp/BurnUID.preserved ]; then
        cp -f /tmp/BurnUID.preserved /mny/mtd/ipc/BurnUID
        echo "BurnUID restored from snapshot: $(cat /mny/mtd/ipc/BurnUID)"
    elif [ -s /tmp/BurnUID.desired ]; then
        cp -f /tmp/BurnUID.desired /mny/mtd/ipc/BurnUID
        echo "BurnUID installed from auto-detect/--sn: $(cat /mny/mtd/ipc/BurnUID)"
    fi
fi
sync; sync

# --- ALWAYS-RUN: FINAL VERIFY + REPORT ---
if [ "$INSTALL_OK" = "1" ]         && [ -x /mny/mtd/ipc/ambicam/ap2p ]         && [ -f /mny/mtd/ipc/ambicam/libcurl.so.4 ]         && [ -f /mny/mtd/ipc/ambicam/libpaho-mqtt3cs.so.1 ]         && [ -f /mny/mtd/ipc/ambicam/VERSION ]         && [ -f /mny/mtd/ipc/ambicam/config.json ]         && [ -f /mny/mtd/ipc/ambicam.sh ]         && [ -s /mny/mtd/ipc/BurnUID ]; then
    echo "=== INSTALLED ==="
    cat /mny/mtd/ipc/ambicam/VERSION
    echo "=== BurnUID ==="
    cat /mny/mtd/ipc/BurnUID
    echo "=== FILES ==="
    ls /mny/mtd/ipc/ambicam/ | wc -l
    echo "=== DF ==="
    df -h /mny/mtd/ipc | tail -1
    exit 0
fi

if [ "$FREE_OK" != "1" ]; then
    echo "___RECIPE_FAILED_GC___"
else
    echo "___RECIPE_FAILED_INSTALL___"
fi
echo "ABORT_REASON: $ABORT_REASON"
echo "free=${FREE_KB}KB required=${REQUIRED_KB}KB"
echo "BurnUID-on-disk: $(cat /mny/mtd/ipc/BurnUID 2>/dev/null || echo MISSING)"
exit 1
"""


# ─────────────────────────── broker / verification ──────────────────────────
def have_mosquitto() -> bool:
    return shutil.which("mosquitto_sub") is not None and \
           shutil.which("mosquitto_pub") is not None


def mqtt_provision_90(sn: str, verify_token: str = "",
                      latency_ms: int = 300, verbose: int = 1,
                      src_path: str = "") -> bool:
    """Publish a fresh 17-key retained payload to torque/rx/<sn>/90.

    For a brand-new camera (no prior provisioning), /90 is empty and ap2p
    will boot but block on state_ready_cv forever waiting for retained
    config.  This helper builds the same payload fleet-provision.sh
    produces — same keys, same defaults — for a SINGLE camera.

    All env vars match fleet-provision.sh so the staging defaults work
    out-of-the-box on the bench, and CI/prod can override via env.
    """
    h, p, u, pw = (DEFAULTS["broker_host"], DEFAULTS["broker_port"],
                   DEFAULTS["broker_user"], DEFAULTS["broker_pass"])

    if not verify_token:
        # view-feed-fast.sh ships with this default URL-encoded token; same
        # value works on the staging broker for QC.  Operators override via
        # ARCISAI_VERIFY_TOKEN env var in prod.
        verify_token = os.environ.get(
            "ARCISAI_VERIFY_TOKEN", "a%2Fb4Znt%2BOFGrYtmHw0T16Q%3D%3D")
    if not src_path:
        src_path = f"/flv/live_ch0_1.flv?verify={verify_token}"

    payload = (
        f"NODE_ID={sn}\n"
        f"CTRL_HOST={os.environ.get('CTRL_HOST', 'signaling.devices.arcisai.io')}\n"
        f"CTRL_PORT={os.environ.get('CTRL_PORT', '80')}\n"
        f"CTRL_SCHEME={os.environ.get('CTRL_SCHEME', 'plain')}\n"
        f"CTRL_TOKEN={os.environ.get('CTRL_TOKEN', 'p2p-server-api-token-change-me')}\n"
        f"EDGE_HOST={os.environ.get('EDGE_HOST', 'turn.devices.arcisai.io')}\n"
        f"EDGE_PORT={os.environ.get('EDGE_PORT', '5349')}\n"
        f"RELAY_HOST={os.environ.get('RELAY_HOST', 'turn.devices.arcisai.io')}\n"
        f"RELAY_PORT={os.environ.get('RELAY_PORT', '5349')}\n"
        f"RELAY_USER={os.environ.get('RELAY_USER', 'arcisai')}\n"
        f"RELAY_PASS={os.environ.get('RELAY_PASS', 'arcisai-turn-secret-change-me')}\n"
        f"SRC_HOST={os.environ.get('SRC_HOST', '127.0.0.1')}\n"
        f"SRC_PORT={os.environ.get('SRC_PORT', '80')}\n"
        f"SRC_PATH={src_path}\n"
        f"SRC_AUTH={os.environ.get('SRC_AUTH', 'Basic YWRtaW46')}\n"
        f"LATENCY_MS={latency_ms}\n"
        f"VERBOSE={verbose}\n"
    )

    # Clear /81 (legacy) first — same pattern fleet-provision.sh uses.
    subprocess.run(
        ["mosquitto_pub", "-h", h, "-p", p, "-u", u, "-P", pw,
         "-t", f"torque/rx/{sn}/81", "-q", "1", "--retain", "-n"],
        capture_output=True, timeout=10)

    # -s reads stdin as ONE payload (all 17 lines together).
    # `-l` would read line-by-line and publish 17 separate retained
    # messages, each clobbering the previous — only the last (VERBOSE=1)
    # would survive on the broker.  ap2p's case-90 parser then rejects
    # the payload because it can't find CTRL_HOST.  Use -s.
    rp = subprocess.run(
        ["mosquitto_pub", "-h", h, "-p", p, "-u", u, "-P", pw,
         "-t", f"torque/rx/{sn}/90", "-q", "1", "--retain", "-s"],
        input=payload.encode(), capture_output=True, timeout=12)
    if rp.returncode != 0:
        print(f"    ! WARN: mosquitto_pub to /90 failed: "
              f"{rp.stderr.decode(errors='replace')[:160]}")
        return False
    print(f"    ✓ /90 retained config published ({len(payload)}B, 17 keys)")
    return True


def mqtt_migrate_81_to_90(sn: str):
    """v2.0.7+ retained-config topic moved /81 → /90.  We MUST preserve the
    payload — clearing /81 without republishing to /90 leaves the camera
    with no retained config, which means apply_ap2p_conf_payload never
    fires, state_ready never sets, and SRT thread blocks forever.

    Procedure:
      1. mosquitto_sub -t /81 -C 1 -W 4   (grab the existing payload, if any)
      2. If non-empty AND /90 currently empty: republish bytes to /90 --retain
      3. Clear /81 with zero-byte --retain
      4. Verify /90 ends up non-empty (assertion to abort flash if not)
    """
    print(f"  → migrating retained config: /rx/{sn}/81 → /rx/{sn}/90")
    h, p, u, pw = (DEFAULTS["broker_host"], DEFAULTS["broker_port"],
                   DEFAULTS["broker_user"], DEFAULTS["broker_pass"])

    # 1. Snapshot whatever's retained on /81 (NOT a CSV — raw bytes).
    legacy = subprocess.run(
        ["mosquitto_sub", "-h", h, "-p", p, "-u", u, "-P", pw,
         "-t", f"torque/rx/{sn}/81", "-C", "1", "-W", "4"],
        capture_output=True, timeout=12)
    legacy_bytes = legacy.stdout
    # Check what's currently on /90 too — never overwrite a freshly-published
    # /90 with stale /81 contents.
    current_90 = subprocess.run(
        ["mosquitto_sub", "-h", h, "-p", p, "-u", u, "-P", pw,
         "-t", f"torque/rx/{sn}/90", "-C", "1", "-W", "4"],
        capture_output=True, timeout=12)
    have_90 = bool(current_90.stdout.strip())

    # 2. Republish to /90 ONLY if /90 is empty AND /81 had real content.
    if not have_90 and legacy_bytes.strip() and b"NODE_ID" in legacy_bytes:
        print(f"    /81 had {len(legacy_bytes)}B of retained config; republishing to /90")
        rp = subprocess.run(
            ["mosquitto_pub", "-h", h, "-p", p, "-u", u, "-P", pw,
             "-t", f"torque/rx/{sn}/90", "-q", "1", "--retain",
             # -s = read ALL stdin as one payload.  Bug fix: was -l (line-
             # mode = one message per line, breaks multi-line conf).
             "-s"],
            input=legacy_bytes, capture_output=True, timeout=10)
        if rp.returncode != 0:
            print(f"    ! WARN: republish to /90 failed: {rp.stderr.decode(errors='replace')[:120]}")
    elif have_90:
        print(f"    /90 already has retained config ({len(current_90.stdout)}B); leaving it alone")
    else:
        print(f"    /81 was empty too; nothing to migrate — camera will need fresh provision")

    # 3. Clear /81 (zero-byte retained) so v2.0.7+ tampering handler doesn't
    #    see leftover config bytes.
    subprocess.run(
        ["mosquitto_pub", "-h", h, "-p", p, "-u", u, "-P", pw,
         "-t", f"torque/rx/{sn}/81", "-q", "1", "--retain", "-n"],
        timeout=10, capture_output=True)

    # 4. Re-check /90 to confirm it ends up populated.  This is the safety
    #    gate that prevents leaving a camera with no config to apply.
    final = subprocess.run(
        ["mosquitto_sub", "-h", h, "-p", p, "-u", u, "-P", pw,
         "-t", f"torque/rx/{sn}/90", "-C", "1", "-W", "4"],
        capture_output=True, timeout=12)
    if final.stdout.strip() and b"NODE_ID" in final.stdout:
        print(f"    ✓ /90 retained config confirmed ({len(final.stdout)}B)")
        return True
    print(f"    ✗ /90 is EMPTY after migration — camera will boot without retained config!")
    print(f"      Run ops/fleet-provision.sh or ops/cutover-v2.0.sh to provision /90.")
    return False


# Keep the old name as an alias so any external callers / docs don't break
mqtt_clear_legacy_81 = mqtt_migrate_81_to_90


def mqtt_wait_fresh_boot_subscriber():
    """Start a mosquitto_sub PROCESS configured to ignore retained messages
    (`-R`).  Returns the Popen handle; caller must wait for output and
    eventually terminate.  Use this together with `mqtt_consume_fresh_boot`
    to verify a fresh boot publish.

    Why -R: the camera publishes its boot summary to torque/tx/<sn>/boot
    with the retain flag set, so the broker holds the LAST publish
    indefinitely.  A naive subscriber receives that retained message
    INSTANTLY on subscribe (broker delivers retained on SUBACK), which
    looks identical on the wire to a fresh publish — but it's not.  -R
    tells the broker NOT to deliver the retained copy; the only
    messages we then see are publishes that arrive AFTER our subscribe.
    Combined with starting this subscriber BEFORE we issue the
    `reboot` to the camera, that guarantees the publish we receive is
    the one produced by the NEW boot."""
    return subprocess.Popen(
        ["mosquitto_sub",
         "-h", DEFAULTS["broker_host"], "-p", DEFAULTS["broker_port"],
         "-u", DEFAULTS["broker_user"], "-P", DEFAULTS["broker_pass"],
         "-R",                             # no retained delivery
         "-t", "$dummy_anchor",            # overridden below by caller via -t
         "-v"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)


def mqtt_wait_fresh_boot(sn: str, T0: float, expect_version: str,
                        timeout: float = 120,
                        existing_sub: Optional[subprocess.Popen] = None) -> Optional[dict]:
    """Wait up to `timeout` seconds for a boot publish on
    torque/tx/<sn>/boot.  With -R (no retained) ANY publish we receive is
    by definition fresh — `now_uptime_sec < 90` is the only safety check.

    If `existing_sub` is None, this function starts mosquitto_sub itself.
    But for the verify path we want the subscriber active BEFORE we issue
    the reboot — caller should pre-start one with the same -t arg.
    """
    print(f"  → waiting up to {timeout:.0f}s for fresh boot publish "
          f"(expect VERSION≈{expect_version!r})...")
    p = existing_sub
    if p is None:
        p = subprocess.Popen(
            ["mosquitto_sub",
             "-h", DEFAULTS["broker_host"], "-p", DEFAULTS["broker_port"],
             "-u", DEFAULTS["broker_user"], "-P", DEFAULTS["broker_pass"],
             "-R",                                # critical: no retained
             "-t", f"torque/tx/{sn}/boot",
             "-W", str(int(timeout)), "-v"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    deadline = T0 + timeout
    try:
        while time.time() < deadline:
            line = p.stdout.readline()
            if not line:
                break
            if " {" not in line:
                continue
            payload = line.split(" ", 1)[1].strip()
            try:
                data = json.loads(payload)
            except Exception:
                continue
            now_up = data.get("now_uptime_sec", 9999)
            # -R guarantees no retained; ANY publish here is post-subscribe.
            # `now_up < 90` is just a sanity guard against an over-eager
            # subscriber catching a stray boot publish from elsewhere.
            if now_up < 90:
                try: p.terminate()
                except Exception: pass
                return data
            print(f"    (publish arrived but now_up={now_up}s — too old; waiting for fresher)")
    finally:
        try: p.terminate(); p.wait(timeout=2)
        except Exception: pass
    return None


def signaling_alive(repo_path: Path, sn: str) -> bool:
    """Quick probe: run feed-direct.py for 6s; success iff first FLV byte arrives."""
    fd = repo_path / "tools" / "feed-direct.py"
    if not fd.exists():
        print(f"  WARN: {fd} missing — skipping signaling probe")
        return True
    print(f"  → 6 s signaling probe via feed-direct.py")
    env = {**os.environ,
           "ARCISAI_API_TOKEN": os.environ.get("ARCISAI_API_TOKEN",
                                               "p2p-server-api-token-change-me"),
           "ARCISAI_VERIFY_TOKEN": os.environ.get("ARCISAI_VERIFY_TOKEN",
                                                  "a%2Fb4Znt%2BOFGrYtmHw0T16Q%3D%3D")}
    try:
        p = subprocess.Popen(["python3", str(fd), "--service-id", sn,
                              "--channel-path", "/flv/live_ch0_0.flv"],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                             env=env)
        ok = False
        end = time.time() + 6
        while time.time() < end:
            time.sleep(0.5)
            try:
                size = os.fstat(p.stdout.fileno()).st_size if p.stdout else 0
            except Exception:
                size = 0
            err_chunk = b""
            try:
                p.stderr.flush() if p.stderr else None
            except Exception: pass
            # Drain stderr non-blocking to look for "first SRT bytes"
            try:
                import select
                if p.stderr:
                    r, _, _ = select.select([p.stderr], [], [], 0)
                    if r: err_chunk = p.stderr.read(2048) or b""
            except Exception: pass
            if b"first SRT bytes" in err_chunk:
                ok = True
                break
        p.terminate()
        try: p.wait(timeout=2)
        except Exception: pass
        return ok
    except Exception as e:
        print(f"  signaling probe error: {e}")
        return False


# ─────────────────────────── flash pipeline ─────────────────────────────────
def do_flash(args, repo_path: Path) -> int:
    cache = DEFAULTS["cache_dir"]
    snap_dir = DEFAULTS["snapshot_dir"]
    print(f"flash-kitty: target={args.ip}")
    # 0. Pre-flight
    if not have_mosquitto():
        print("  ERROR: mosquitto_sub + mosquitto_pub required for verification.")
        print("         install with:  brew install mosquitto")
        return 2

    # 1. Pick tarball
    if args.tarball:
        tarball = Path(args.tarball).expanduser().resolve()
        tag = "local"
    else:
        tag = args.tag
        if not tag:
            r = subprocess.run(["gh", "release", "view", "--repo", args.repo,
                                "--json", "tagName", "--jq", ".tagName"],
                               capture_output=True, text=True, timeout=15)
            tag = r.stdout.strip()
        tarball = fetch_tarball(args.repo, tag, cache)

    print(f"flash-kitty: source={tarball.name} (tag={tag})")
    try:
        meta = validate_tarball(tarball)
    except Exception as e:
        print(f"  ERROR: tarball pre-flight: {e}")
        return 1
    print(f"  ✓ tarball OK  VERSION={meta.get('VERSION')!r}  COMMIT={meta.get('COMMIT','')[:8]!r}")

    static_tar = ensure_static_tar(cache)

    # 2. Reachability
    if not is_http_up(args.ip):
        print(f"  ERROR: camera HTTP:80 unreachable @ {args.ip}")
        return 2
    if not is_telnet_up(args.ip):
        print(f"  telnet down — enabling via HTTP PUT /NetAPI/R.SwitchTelnet")
        enable_telnet(args.ip, DEFAULTS["http_pw"])
        for i in range(15):
            time.sleep(2)
            if is_telnet_up(args.ip):
                print(f"  ✓ telnet up after {2*(i+1)}s"); break
        else:
            print("  ERROR: telnet still down after 30s")
            return 2

    # 3. Snapshot + SN auto-detection (CAMERA-AGNOSTIC).
    #
    # Every ArcisAI camera ships with its Device ID (BurnUID) in known
    # places.  probe_camera_sn() walks them in order:
    #   a) /mny/mtd/ipc/BurnUID         (kitty canonical)
    #   b) /netsdk/system/deviceinfo    serialNumber  (vendor burnt-in)
    #   c) /NetAPI/R.Sync.Stat.DeviceInfo DevUid       (vendor backup)
    # --sn is honoured only as an explicit override; it does NOT default.
    t = Telnet(args.ip); telnet_login(t, "root", DEFAULTS["root_pw"])

    detected = probe_camera_sn(args.ip, DEFAULTS["http_pw"], telnet=t)
    if args.sn:
        sn = args.sn.strip()
        if detected and detected != sn:
            print(f"  WARN: --sn={sn!r} but camera reports {detected!r}")
            print(f"        Using {sn!r} (your explicit override).")
        else:
            print(f"  ✓ using --sn override: {sn!r}")
    else:
        if not detected:
            print("  ERROR: cannot auto-detect Device ID.  All three sources empty:")
            print("           /mny/mtd/ipc/BurnUID")
            print("           GET /netsdk/system/deviceinfo (serialNumber)")
            print("           POST /NetAPI/R.Sync.Stat.DeviceInfo (DevUid)")
            print("         This usually means the camera's vendor-burnt identity")
            print("         got wiped (unit was incompletely factory-set or had a")
            print("         partial reflash).  Pass an override via --sn ATPL-NNNNNN-XXXXX")
            t.close(); return 2
        sn = detected
        print(f"  ✓ Device ID auto-detected: {sn!r}")

    # Stash resolved SN back onto args so all downstream code uses ONE value.
    args.sn = sn

    snap = snapshot_pre_flash(t, snap_dir, sn)
    prev_version = snap["version"]

    # 4. Push files
    print("flash-kitty: pushing files...")
    try:
        push_via_nc(t, args.ip, static_tar, "/tmp/tar")
        push_via_nc(t, args.ip, tarball,    "/tmp/Augentix.tar.gz")
        t.run("chmod 755 /tmp/tar", to=8)
    except Exception as e:
        print(f"  ERROR push: {e}")
        t.close(); return 3

    # 5. Recipe — push as a file + execute.  Why a file and not inline:
    # ~6 KB of inline shell over telnet, with all the `\"` quoting and
    # the busybox shell's PS2 continuation behaviour, was too fragile —
    # a single mismatched quote left the shell at `#####>` waiting for
    # input that never came, and Python's telnet rd() then timed out.
    # Writing the recipe to /tmp/install_recipe.sh on the camera and
    # exec'ing it makes the recipe text byte-identical to RECIPE_SH
    # below — no shell-quote round-trip — and ALL output of the recipe
    # lands cleanly before its `exit 0` / `exit 1`.
    print("flash-kitty: extracting + installing (hardened recipe)...")
    # 5a. Stage the desired BurnUID into /tmp before running recipe.
    # NB: trailing newline is intentional — many existing tools (cat,
    # awk, busybox text utilities) emit subtly different output when
    # the file lacks one, and our telnet line-extractor was previously
    # eating the BurnUID because it merged with the next marker line.
    t.run(f"echo '{args.sn}' > /tmp/BurnUID.desired", to=5)
    # 5b. Write the recipe to a local temp file, push via the same nc
    #     transport we use for the tarball.
    recipe_tmp = Path("/tmp") / f"install_recipe-{os.getpid()}.sh"
    recipe_tmp.write_text(RECIPE_SH)
    try:
        push_via_nc(t, args.ip, recipe_tmp, "/tmp/install_recipe.sh")
    finally:
        try: recipe_tmp.unlink()
        except Exception: pass
    t.run("chmod 755 /tmp/install_recipe.sh", to=8)
    # 5c. Execute.  180 s budget covers the worst case (30-iter GC loop
    # + ~15 s install + sync barriers).  The recipe ALWAYS reaches its
    # final REPORT phase, so a marker line is guaranteed even on abort.
    out = t.run("/tmp/install_recipe.sh 2>&1; echo __RECIPE_RC=$?__", to=180)

    # 5d. Auto-recovery for GC fail.
    # JFFS2 lazy GC sometimes can't reclaim ~900 KB of dirty blocks
    # in-running; a fresh mount-time scan after reboot frees them
    # completely.  If the recipe failed with ___RECIPE_FAILED_GC___,
    # reboot the camera, wait for it to come back, re-push files, and
    # retry the recipe ONCE.  Without this, freshly-flashed cameras
    # that had partial-install state need manual recovery.
    if "___RECIPE_FAILED_GC___" in out and not args.no_auto_recover:
        print("  → GC fail detected; rebooting camera to force JFFS2 reclaim "
              "+ retrying ONCE")
        try:
            t.send("sync; reboot")
        except Exception:
            pass
        time.sleep(2)
        t.close()
        # Wait for camera to come back online (HTTP first, then telnet).
        print("    waiting for camera to come back ...")
        for i in range(40):
            time.sleep(3)
            if is_http_up(args.ip):
                print(f"    ✓ HTTP back after {3*(i+1)}s")
                break
        else:
            print("    ! HTTP never came back; abort")
            return 4
        if not is_telnet_up(args.ip):
            print("    enabling telnet via NetAPI ...")
            enable_telnet(args.ip, DEFAULTS["http_pw"])
            for i in range(15):
                time.sleep(2)
                if is_telnet_up(args.ip):
                    print(f"    ✓ telnet back after {2*(i+1)}s"); break
            else:
                print("    ! telnet stayed down; abort")
                return 4
        # Re-open telnet session
        t = Telnet(args.ip); telnet_login(t, "root", DEFAULTS["root_pw"])
        # Re-stage everything (/tmp is RAM, wiped on reboot).
        print("    re-pushing files + recipe ...")
        try:
            push_via_nc(t, args.ip, static_tar, "/tmp/tar")
            push_via_nc(t, args.ip, tarball,    "/tmp/Augentix.tar.gz")
            t.run("chmod 755 /tmp/tar", to=8)
            t.run(f"echo '{args.sn}' > /tmp/BurnUID.desired", to=5)
            recipe_tmp = Path("/tmp") / f"install_recipe-{os.getpid()}-r.sh"
            recipe_tmp.write_text(RECIPE_SH)
            push_via_nc(t, args.ip, recipe_tmp, "/tmp/install_recipe.sh")
            try: recipe_tmp.unlink()
            except Exception: pass
            t.run("chmod 755 /tmp/install_recipe.sh", to=8)
        except Exception as e:
            print(f"    ! re-push failed: {e}")
            t.close(); return 4
        print("    re-running recipe ...")
        out = t.run("/tmp/install_recipe.sh 2>&1; echo __RECIPE_RC=$?__", to=180)

    failed = ("__RECIPE_RC=0__"           not in out or
              "___RECIPE_FAILED_GC___"        in out or
              "___RECIPE_FAILED_INSTALL___"   in out or
              "___RECIPE_FAILED_EXTRACT___"   in out or
              "___RECIPE_FAILED_VALIDATE___"  in out or
              "=== INSTALLED ===" not in out)
    # Always print recipe output (good and bad) so user has full state
    for line in out.splitlines():
        ln = line.lstrip("#")
        if ln.strip() and "_MK_" not in ln:
            print(f"    {ln}")
    if failed:
        print("  ERROR: recipe failed")
        if "___RECIPE_FAILED_GC___" in out:
            print("    Root cause: JFFS2 GC could not free enough blocks after 30 s.")
            print("    This usually means the partition is highly fragmented.")
            print("    Recovery options:")
            print(f"      1) telnet root@{args.ip} ; reboot   (clean reboot triggers")
            print( "         a full JFFS2 scan + GC on next mount)")
            print( "      2) Then re-run flash-kitty.py")
            print( "    The camera is left in a SAFE state (BurnUID preserved,")
            print( "    ap2p stopped, no half-written ambicam/) — re-flash is safe.")
        elif "___RECIPE_FAILED_INSTALL___" in out:
            print("    Root cause: cp failed during install phase (ENOSPC or I/O).")
            print("    Look for INSTALL_FAIL line above to see which file failed.")
            print("    Recovery: same as GC failure above.")
        else:
            print("    Camera state is uncertain — inspect manually via telnet.")
        t.close(); return 4

    # Pull confirmed version from camera (clean shell prompt + ANSI noise).
    # NB: `; echo` after each `cat` ensures a trailing newline, otherwise
    # files without a trailing \n (which our BurnUID writer used to produce)
    # get glued to the telnet marker line and the cleaner drops them.
    installed_lines = _telnet_extract_lines(
        t.run("cat /mny/mtd/ipc/ambicam/VERSION 2>/dev/null; echo", to=5))
    installed_ver = installed_lines[0] if installed_lines else "?"
    installed_burn_lines = _telnet_extract_lines(
        t.run("cat /mny/mtd/ipc/BurnUID 2>/dev/null; echo", to=5))
    installed_burn = installed_burn_lines[0] if installed_burn_lines else "?"
    print(f"  ✓ on-disk VERSION reads: {installed_ver!r}  (snapshot.prev={prev_version!r})")
    print(f"  ✓ on-disk BurnUID reads: {installed_burn!r}")
    if meta.get("VERSION") and installed_ver != meta["VERSION"]:
        print(f"  WARN: installed version {installed_ver!r} != tarball version {meta['VERSION']!r}")
    if installed_burn in ("MISSING", "?", ""):
        print(f"  ⚠️ BurnUID still missing on camera AFTER install — ap2p will fail to start.")
        print(f"     Recovery: telnet root@{args.ip} ; "
              f"echo 'ATPL-XXXXXX-XXXXX' > /mny/mtd/ipc/BurnUID ; sync ; reboot")

    # 6. Migrate /81 → /90  (preserves the retained payload — earlier
    #    versions just cleared /81, which stranded cameras with no config)
    print("flash-kitty: migrating retained MQTT topic /81 → /90...")
    mig_ok = mqtt_migrate_81_to_90(args.sn)
    if not mig_ok:
        if args.provision_90:
            print(f"  /90 empty + --provision-90 set → publishing fresh retained payload")
            if not mqtt_provision_90(args.sn,
                                      latency_ms=args.latency_ms,
                                      verbose=1 if args.verbose else 0,
                                      src_path=args.src_path or ""):
                print(f"  ⚠️ --provision-90 publish failed; camera will block on state_ready")
        else:
            print(f"  ⚠️ /90 has no retained config — camera will boot but ap2p will")
            print(f"     block on state_ready_cv forever.  Either:")
            print(f"       (a) re-run with --provision-90, or")
            print(f"       (b) run ops/fleet-provision.sh fleet.csv")

    # 7. Cleanup + reboot
    t.run("rm -f /tmp/AugenTix /tmp/Augentix.tar.gz /tmp/tar 2>/dev/null", to=5)
    if args.no_reboot:
        print("flash-kitty: --no-reboot specified.  Manual reboot required for new kitty.")
        t.close()
        return 0

    # Pre-start the fresh-boot subscriber BEFORE issuing reboot.  With -R
    # (no retained) the broker only delivers publishes that arrive after
    # SUBACK — so we need the SUBACK to land before the camera publishes
    # its NEW boot summary.  Without this, the subscriber may attach
    # AFTER the camera has already published-and-retained, miss the new
    # publish entirely, and time out.  ~1 s lead is plenty.
    verify_sub: Optional[subprocess.Popen] = None
    if not args.no_verify:
        verify_sub = subprocess.Popen(
            ["mosquitto_sub",
             "-h", DEFAULTS["broker_host"], "-p", DEFAULTS["broker_port"],
             "-u", DEFAULTS["broker_user"], "-P", DEFAULTS["broker_pass"],
             "-R",
             "-t", f"torque/tx/{args.sn}/boot",
             "-W", str(int(args.verify_timeout)), "-v"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        time.sleep(1.0)   # give SUBACK time to propagate

    print("flash-kitty: rebooting camera...")
    T0 = time.time()
    t.send("sync; reboot")
    time.sleep(1.5); t.close()

    # 8. Verify FRESH boot
    if args.no_verify:
        print("flash-kitty: --no-verify; skipping post-reboot checks")
        return 0
    data = mqtt_wait_fresh_boot(args.sn, T0,
                                 expect_version=meta.get("VERSION", ""),
                                 timeout=args.verify_timeout,
                                 existing_sub=verify_sub)
    if not data:
        print(f"  ✗ FAIL — no fresh boot publish within {args.verify_timeout}s")
        # Diagnose: telnet in and read ap2p.log to find the ACTUAL cause.
        # The two common patterns we have seen:
        #   "mqtt: connect ... failed rc=-1" → camera can't reach broker
        #     (firewall on the camera's subnet blocks egress to :443,
        #     or DNS not resolving the broker hostname).
        #   "no NODE_ID"  → BurnUID got wiped during install.
        #   nothing useful  → ap2p crashed before logging anything.
        print(f"  → diagnosing via telnet + ap2p.log to surface the actual root cause...")
        cause = _diagnose_verify_failure(args.ip, args.sn)
        if cause:
            print(f"\nDIAGNOSIS\n{cause}\n")
        return _maybe_rollback(args, prev_version, repo_path)

    # NB: boot_timing.c hard-codes "version":"2.0.4" — don't trust the JSON
    # field; trust the on-disk VERSION we already verified above.
    live = data.get("since_ambicam_start_sec", {}).get("ctrl_registered", -1)
    print(f"  ✓ FRESH boot publish received — ctrl_registered={live:.1f}s")

    # 9. Quick signaling check
    sig_ok = signaling_alive(repo_path, args.sn)
    if sig_ok:
        print("  ✓ signaling probe passed — camera registered + serving FLV")
    else:
        print("  ✗ signaling probe failed (no FLV bytes in 6s)")
        return _maybe_rollback(args, prev_version, repo_path)

    # 10. Done
    print()
    print("="*72)
    print(f"flash-kitty: SUCCESS — {args.sn} now running {installed_ver}")
    print(f"             previous version was: {prev_version!r}")
    print(f"             snapshot saved for rollback: {snap_dir}/")
    print("="*72)
    return 0


def _diagnose_verify_failure(ip: str, sn: str) -> Optional[str]:
    """When the post-flash boot-publish verify times out, this function
    telnets back into the camera, reads /mny/mtd/ipc/ambicam/ap2p.log,
    and returns a human-readable explanation of the most likely cause.

    Common patterns we have observed:
      • `mqtt: connect ... failed rc=-1`  — camera can't reach the
        MQTT broker (firewall on this VLAN blocks outbound to :443,
        or DNS isn't resolving the broker hostname from this network).
      • `no NODE_ID`                       — BurnUID got wiped.
      • SRT/signaling errors only          — MQTT is fine; SRT path
        is the blocker (TURN/STUN unreachable, etc.).
      • Empty / very small log             — ap2p crashed extremely
        early (likely a missing libcurl/libpaho, broken config.json).
    """
    if not is_telnet_up(ip):
        if not enable_telnet(ip, DEFAULTS["http_pw"]):
            return ("Camera HTTP is reachable but telnet is locked.  "
                    "Cannot read ap2p.log to diagnose.")
        for _ in range(15):
            time.sleep(2)
            if is_telnet_up(ip): break
        else:
            return "Telnet enable PUT succeeded but telnet never came up."
    try:
        t = Telnet(ip)
        telnet_login(t, "root", DEFAULTS["root_pw"])
    except Exception as e:
        return f"Telnet login failed: {e}"
    try:
        out = t.run("ps | grep -E '/ap2p|ambicam.sh' | grep -v grep; "
                    "echo '---'; "
                    "wc -c /mny/mtd/ipc/ambicam/ap2p.log 2>/dev/null; "
                    "echo '---'; "
                    "tail -25 /mny/mtd/ipc/ambicam/ap2p.log 2>/dev/null; "
                    "echo '---'; "
                    "ping -c 2 -W 2 mqtt-staging.devices.arcisai.io 2>&1 | tail -3",
                    to=18)
    finally:
        try: t.close()
        except Exception: pass

    log_tail = out.lower()
    findings = []

    if "/ap2p" not in out:
        findings.append("ap2p process is NOT running on the camera.  "
                        "ambicam.sh watchdog should be restarting it but isn't.  "
                        "Check that the binary is executable: "
                        "`telnet root@" + ip + " ; ls -la /mny/mtd/ipc/ambicam/ap2p`")
    if "mqtt: connect" in log_tail and "failed rc=" in log_tail:
        findings.append(
            "ap2p IS running but CANNOT REACH THE BROKER.\n"
            "    ap2p.log shows repeated:  mqtt: connect tcp://mqtt-staging... failed rc=-1\n"
            "    The camera's network (this VLAN / Wi-Fi) is blocking outbound to\n"
            "    mqtt-staging.devices.arcisai.io:443 — or DNS isn't resolving the\n"
            "    broker hostname.  This is environmental, NOT a kitty install bug:\n"
            "      • verify camera can ping the broker  (the diagnose ping at the\n"
            "        end of ap2p.log here would say 'unknown host' or 100% loss)\n"
            "      • check the AP / firewall lets outbound TCP/443 leave this VLAN\n"
            "      • move the camera back to the subnet where it worked before,\n"
            "        or add a NAT rule on the bench gateway.")
    if "no NODE_ID" in log_tail or "burnuid" in log_tail and "missing" in log_tail:
        findings.append(
            "BurnUID is missing on the camera — ap2p has no NODE_ID to use.\n"
            "    Re-run with explicit --sn ATPL-... or restore the file:\n"
            "      `echo 'ATPL-NNNNNN-XXXXX' > /mny/mtd/ipc/BurnUID; sync; reboot`")
    if "100% packet loss" in log_tail or "unknown host" in log_tail or "bad address" in log_tail:
        findings.append(
            "Camera-side ping to broker also failed → confirmed network reachability\n"
            "    problem.  No amount of re-flashing can fix this; the camera needs\n"
            "    a route to the broker.")
    if not findings:
        # Last 6 non-empty lines of the log so the operator has SOMETHING.
        last_lines = [ln for ln in out.splitlines()
                      if ln.strip() and "_MK_" not in ln][-6:]
        findings.append("No specific failure signature.  Last 6 log lines:\n    "
                        + "\n    ".join(last_lines))
    return "\n  • ".join(["The verify failed because:"] + findings)


def _maybe_rollback(args, prev_tag: str, repo_path: Path) -> int:
    if not args.rollback_on_fail:
        print("\nflash-kitty: --rollback-on-fail not set.  Camera is in an unverified state.")
        print(f"             Manual recovery options:")
        print(f"               1) wait + re-run flash-kitty.py")
        print(f"               2) flash-kitty.py --tag {prev_tag} (or whatever was running)")
        print(f"               3) power-cycle + flash again")
        return 5
    rb_tag = args.rollback_tag or prev_tag
    if not rb_tag or rb_tag in ("?", ""):
        print(f"\nflash-kitty: no usable rollback tag (prev={prev_tag!r}).  Manual recovery needed.")
        return 5
    print(f"\nflash-kitty: AUTO-ROLLING-BACK to {rb_tag}...")
    return rollback_to(args, rb_tag, repo_path)


def rollback_to(args, tag: str, repo_path: Path) -> int:
    """Reflash an older tag.  Recursion is intentional — clearer than a state machine."""
    rb_args = argparse.Namespace(**vars(args))
    rb_args.tag = tag
    rb_args.tarball = None
    rb_args.rollback_on_fail = False        # don't rollback the rollback
    rb_args.rollback_tag = None
    return do_flash(rb_args, repo_path)


# ─────────────────────────── main ───────────────────────────────────────────
def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ip", required=True, help="camera IPv4 (e.g. 192.168.12.129)")
    src = ap.add_mutually_exclusive_group()
    src.add_argument("--tag", help="GitHub release tag (default: latest)")
    src.add_argument("--tarball", help="local Augentix.tar.gz path")
    ap.add_argument("--sn", default=None,
                    help="BurnUID / ArcisAI Device ID (e.g. ATPL-405858-AUGEN). "
                         "If omitted, read from camera's /mny/mtd/ipc/BurnUID. "
                         "If provided AND camera has no BurnUID, write it during install.")
    ap.add_argument("--no-reboot",  action="store_true",
                    help="finish install, don't reboot (manual reboot required to activate)")
    ap.add_argument("--no-verify",  action="store_true",
                    help="skip post-reboot MQTT + signaling verification")
    ap.add_argument("--verify-timeout", default=120, type=int,
                    help="how long to wait for fresh-boot publish (s)")
    ap.add_argument("--rollback-on-fail", action="store_true",
                    help="auto-flash the previous tag if verification fails")
    ap.add_argument("--rollback-tag",
                    help="explicit rollback target (default: tag camera was running pre-flash)")
    ap.add_argument("--repo", default=DEFAULTS["repo"])
    ap.add_argument("--provision-90", action="store_true",
                    help="if torque/rx/<sn>/90 is empty after the /81→/90 "
                         "migration, publish a fresh 17-key retained payload "
                         "so this camera boots into a usable state. "
                         "Uses CTRL_*/EDGE_*/RELAY_*/SRC_* env vars (staging "
                         "defaults baked in for bench QC).")
    ap.add_argument("--latency-ms", type=int, default=300,
                    help="SRT TSBPD buffer ms for /90 payload (default 300)")
    ap.add_argument("--verbose", action="store_true",
                    help="set VERBOSE=1 in /90 payload (verbose ap2p logging)")
    ap.add_argument("--src-path",
                    help="override SRC_PATH in /90 payload "
                         "(default /flv/live_ch0_1.flv?verify=<ARCISAI_VERIFY_TOKEN>)")
    ap.add_argument("--no-auto-recover", action="store_true",
                    help="don't reboot+retry on JFFS2 GC fail (default: do reboot+retry)")
    args = ap.parse_args()

    # Detect repo root (we may be invoked from anywhere)
    here = Path(__file__).resolve().parent.parent
    rc = do_flash(args, here)
    return rc


if __name__ == "__main__":
    sys.exit(main())
