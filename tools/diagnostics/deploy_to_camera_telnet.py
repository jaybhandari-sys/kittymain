#!/usr/bin/env python3
"""
Push provider_srt + provider_srt.conf to the Augentix LAN test camera at
192.168.12.147.  The camera has telnet (no SSH) and busybox tools.

Strategy:
  1. Telnet → login as root
  2. Run `nc -l -p 9999 > /tmp/provider_srt` on the camera in background
  3. From the host, `nc 192.168.12.147 9999 < provider_srt`  (push the file)
  4. Verify md5 matches
  5. Push the conf via printf '...' > /tmp/provider_srt.conf  (small file, no nc needed)
  6. chmod +x /tmp/provider_srt
  7. Print run command for the operator (don't auto-launch — user wants control)

Designed to be safe and re-runnable: it kills any old /tmp/provider_srt before
copying, and it never modifies anything outside /tmp (squashfs / is read-only
on this firmware).
"""
import argparse
import hashlib
import os
import socket
import subprocess
import sys
import time
from pathlib import Path


CAM         = "192.168.12.147"
TELNET_PORT = 23
NC_PORT     = 9999
USER        = "root"
PASS        = os.environ.get("CAMERA_ROOT_PASSWORD", "")


# ---------------------------------------------------------------------------
# Telnet client (reused pattern from .claude/probe.py — Python 3.13 dropped
# telnetlib so we negotiate IAC by hand on a raw socket).
# ---------------------------------------------------------------------------

class Telnet:
    def __init__(self, host, port=23, timeout=8):
        # Force IPv4: socket.create_connection prefers IPv6 if getaddrinfo
        # returns an AAAA-ish result, and that can stall for the full
        # timeout against IPv4-only hosts.
        self.s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.s.settimeout(timeout)
        self.s.connect((host, port))
        self.buf = b""

    def _negotiate(self, data: bytes) -> bytes:
        """Strip IAC option bytes; auto-refuse all WILL/DO."""
        out = bytearray()
        replies = bytearray()
        i = 0
        while i < len(data):
            b = data[i]
            if b == 0xFF and i + 2 < len(data):
                cmd, opt = data[i + 1], data[i + 2]
                if   cmd == 0xFB: replies += bytes([0xFF, 0xFE, opt])  # WILL → DON'T
                elif cmd == 0xFD: replies += bytes([0xFF, 0xFC, opt])  # DO   → WON'T
                i += 3
            else:
                out.append(b)
                i += 1
        if replies:
            self.s.sendall(bytes(replies))
        return bytes(out)

    def read_until(self, marker: bytes, timeout: float = 8.0) -> bytes:
        deadline = time.time() + timeout
        while marker not in self.buf and time.time() < deadline:
            try:
                self.s.settimeout(max(0.5, deadline - time.time()))
                chunk = self.s.recv(4096)
                if not chunk: break
                self.buf += self._negotiate(chunk)
            except socket.timeout:
                continue
        if marker in self.buf:
            idx = self.buf.index(marker) + len(marker)
            data, self.buf = self.buf[:idx], self.buf[idx:]
            return data
        raise TimeoutError(f"didn't see {marker!r}; got {self.buf[-200:]!r}")

    def send(self, line: str) -> None:
        self.s.sendall((line + "\n").encode())

    def run(self, cmd: str, timeout: float = 8.0) -> str:
        """Run cmd, return everything between an echo'd marker."""
        marker = f"___MARK_{int(time.time() * 1000)}___"
        self.send(f"{cmd}; echo {marker}")
        out = self.read_until(marker.encode(), timeout)
        return out.decode("utf-8", errors="replace")

    def close(self):
        try: self.s.close()
        except Exception: pass


def login(t: Telnet):
    t.read_until(b"login:")
    t.send(USER)
    t.read_until(b"Password:")
    t.send(PASS)
    # Wait for shell prompt (busybox uses # for root)
    t.read_until(b"# ")


# ---------------------------------------------------------------------------
# Push file to camera
# ---------------------------------------------------------------------------

def push_via_nc(t: Telnet, local_path: Path, remote_path: str):
    """Use netcat: camera listens, host pushes."""
    print(f"  push {local_path} → {remote_path}")
    # Clear any prior file and start nc listener on the camera (background)
    t.run(f"rm -f {remote_path}")
    # busybox nc syntax: -l -p PORT
    # Run in background, redirect to file
    t.send(f"nc -l -p {NC_PORT} > {remote_path} &")
    time.sleep(0.4)   # let nc bind

    # Push the bytes from the host
    size = local_path.stat().st_size
    s = socket.create_connection((CAM, NC_PORT), timeout=15)
    sent = 0
    with open(local_path, "rb") as f:
        while True:
            buf = f.read(64 * 1024)
            if not buf: break
            s.sendall(buf)
            sent += len(buf)
    s.shutdown(socket.SHUT_WR)
    # Drain ack (nc has nothing to say but we wait for close)
    s.settimeout(5)
    try:
        while s.recv(4096): pass
    except Exception: pass
    s.close()
    print(f"  pushed {sent} bytes (expected {size})")
    if sent != size:
        raise RuntimeError(f"size mismatch on push: sent {sent} of {size}")

    # Wait for the camera-side nc to exit + the busybox shell to flush its
    # "[1]+  Done   nc..." background-job notification.
    time.sleep(1.2)
    # Drain any pending output (job notifications etc.) so it doesn't bleed
    # into the next command.
    try:
        t.s.settimeout(0.5)
        while True:
            chunk = t.s.recv(4096)
            if not chunk: break
            t.buf += t._negotiate(chunk)
    except (socket.timeout, OSError):
        pass

    # Compare md5.  Output may include a busybox "[1]+ Done" notification
    # plus a shell prompt; pick out the first 32-hex-char run.
    md5_out = t.run(f"md5sum {remote_path} 2>/dev/null")
    import re
    m = re.search(r'\b([0-9a-f]{32})\b', md5_out)
    if not m:
        raise RuntimeError(f"could not parse md5 from: {md5_out!r}")
    remote_md5 = m.group(1)
    local_md5 = hashlib.md5(local_path.read_bytes()).hexdigest()
    if remote_md5 != local_md5:
        # Print up to 200 chars of remote stat for diagnosis
        print(f"  WARN: md5 mismatch  local={local_md5}  remote={remote_md5}")
        print(t.run(f"ls -la {remote_path}"))
        raise RuntimeError("md5 mismatch")
    print(f"  md5 OK: {local_md5}")


def push_text(t: Telnet, content: str, remote_path: str):
    """Write a small text file via nc — same method as the binary push.
    The earlier echo+base64 approach silently produced 0-byte files (busybox
    echo wrapping or quoting issue); piping bytes through nc avoids the
    shell entirely."""
    tmp = Path("/tmp/_arcis_push_conf.tmp")
    tmp.write_bytes(content.encode())
    try:
        push_via_nc(t, tmp, remote_path)
    finally:
        tmp.unlink(missing_ok=True)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin",  default="/mnt/e/coturn/coturn/Signaling_Server/P2P_Libjuice-main/binaries-arm-augentix-linux-uclibcgnueabihf/provider_srt")
    ap.add_argument("--signaling-host", default="142.93.223.221")
    ap.add_argument("--signaling-port", default="8888")
    ap.add_argument("--stun-host",      default="142.93.223.221")
    ap.add_argument("--stun-port",      default="3478")
    ap.add_argument("--service-id",     default="cam001")
    ap.add_argument("--api-token",      default=os.environ.get("ARCISAI_API_TOKEN", ""))
    args = ap.parse_args()

    bin_path  = Path(args.bin)
    if not bin_path.is_file():
        print(f"FAIL: binary not found: {bin_path}"); return 2
    print(f"[1/5] connect telnet {CAM}:{TELNET_PORT}")
    t = Telnet(CAM)
    try:
        login(t)
        print("       login OK")
        # Verify nc available
        ncver = t.run("which nc; nc 2>&1 | head -1; uname -m", timeout=5)
        print("       env:", ncver.strip().splitlines()[:5])

        print("[2/5] push provider_srt binary to /tmp")
        push_via_nc(t, bin_path, "/tmp/provider_srt")
        t.run("chmod +x /tmp/provider_srt")

        print("[3/5] write provider_srt.conf (real-P2P rendezvous mode)")
        conf = (
            "# generated by deploy_to_camera_telnet.py — provider_srt v2 (rendezvous)\n"
            "LOCAL_HOST=127.0.0.1\n"
            "LOCAL_PORT=80\n"
            "LOCAL_HTTP_PATH=/flv/live_ch0_1.flv?verify=a%2Fb4Znt%2BOFGrYtmHw0T16Q%3D%3D\n"
            "LOCAL_HTTP_AUTH=Basic YWRtaW46\n"
            f"SERVICE_ID={args.service_id}\n"
            f"SIGNALING_HOST={args.signaling_host}\n"
            f"SIGNALING_PORT={args.signaling_port}\n"
            f"STUN_HOST={args.stun_host}\n"
            f"STUN_PORT={args.stun_port}\n"
            f"API_TOKEN={args.api_token}\n"
            "LATENCY_MS=300\n"
            "VERBOSE=1\n"
        )
        push_text(t, conf, "/tmp/provider_srt.conf")
        print("       wrote /tmp/provider_srt.conf")

        print("[4/5] sanity check: file exists and is executable")
        out = t.run("ls -la /tmp/provider_srt /tmp/provider_srt.conf", timeout=5)
        print(out.strip())

        print("[5/5] DONE.  To start the provider on the camera:")
        print(f"        telnet {CAM}     # then: cd /tmp && ./provider_srt provider_srt.conf")
        print()
        print("       Or run it in background from this script with --run.")
    finally:
        t.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
