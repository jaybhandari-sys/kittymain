#!/usr/bin/env python3
"""
Deploy binaries to Augentix camera over telnet.

Flow:
  1. Start a temporary HTTP server on the runner machine.
  2. Telnet into the camera, stop running processes.
  3. wget each binary from the runner's HTTP server.
  4. chmod +x, restart ambicam.sh.

Required environment variables (set as GitHub Actions secrets):
  CAMERA_HOST  - camera LAN IP   (e.g. 192.168.5.97)
  CAMERA_USER  - telnet username (e.g. root)
  CAMERA_PASS  - telnet password
"""

import argparse
import http.server
import os
import socket
import sys
import threading
import time

try:
    import pexpect
except ImportError:
    sys.exit("pexpect not installed — run: pip install pexpect")

CAMERA_HOST = os.environ["CAMERA_HOST"]
CAMERA_USER = os.environ.get("CAMERA_USER", "root")
CAMERA_PASS = os.environ["CAMERA_PASS"]
AMBICAM_DIR = "/mny/mtd/ipc/ambicam"
TELNET_TIMEOUT = 30
# Anyka cameras use "$ " prompt; standard Linux root uses "# "
SHELL_PROMPT = r"[$#] $"


def local_ip_towards(target: str) -> str:
    """Return the local interface IP that routes towards target."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((target, 80))
        return s.getsockname()[0]
    finally:
        s.close()


class _SilentHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *_):
        pass


def start_http_server(directory: str, port: int) -> http.server.HTTPServer:
    os.chdir(directory)
    server = http.server.HTTPServer(("", port), _SilentHandler)
    t = threading.Thread(target=server.serve_forever, daemon=True)
    t.start()
    return server


def run_cmd(child, cmd: str, timeout: int = 60) -> str:
    """Send a command and wait for the shell prompt."""
    child.sendline(cmd)
    child.expect(SHELL_PROMPT, timeout=timeout)
    return child.before


def deploy(binaries_dir: str, binaries: list, port: int = 8888):
    runner_ip = local_ip_towards(CAMERA_HOST)
    print(f"[deploy] Runner IP visible to camera: {runner_ip}")
    print(f"[deploy] Serving {binaries_dir} on :{port}")

    server = start_http_server(binaries_dir, port)
    time.sleep(0.5)

    try:
        print(f"[deploy] Connecting to {CAMERA_HOST}:23 ...")
        child = pexpect.spawn(
            f"telnet {CAMERA_HOST}", timeout=TELNET_TIMEOUT, encoding="utf-8"
        )
        child.expect("login:", timeout=20)
        child.sendline(CAMERA_USER)

        idx = child.expect(["Password:", SHELL_PROMPT], timeout=10)
        if idx == 0:
            child.sendline(CAMERA_PASS)
            child.expect(SHELL_PROMPT, timeout=15)

        print("[deploy] Logged in.")

        # Stop existing processes
        for proc in ("provider_srt", "MQTT_vcamclient_Augentix", "ap2p"):
            run_cmd(child, f"killall {proc} 2>/dev/null; true", timeout=10)
        run_cmd(child, "sleep 1", timeout=5)

        # Check free space
        out = run_cmd(child, f"df -k {AMBICAM_DIR} 2>/dev/null | tail -1", timeout=10)
        print(f"[deploy] Disk: {out.strip()}")

        # Download each binary
        for binary in binaries:
            url = f"http://{runner_ip}:{port}/{binary}"
            dest = f"{AMBICAM_DIR}/{binary}"
            print(f"[deploy] Fetching {binary} ...")
            run_cmd(child, f"wget -q -O {dest} {url}", timeout=120)
            run_cmd(child, f"chmod +x {dest}", timeout=10)
            size = run_cmd(child, f"ls -la {dest}", timeout=10)
            print(f"[deploy] {size.strip()}")

        # Ensure launcher is executable
        run_cmd(child, f"chmod +x {AMBICAM_DIR}/ambicam.sh 2>/dev/null; true", timeout=5)

        # Restart service
        print("[deploy] Restarting ambicam.sh ...")
        run_cmd(
            child,
            f"nohup {AMBICAM_DIR}/ambicam.sh > /tmp/ambicam_restart.log 2>&1 &",
            timeout=10,
        )
        time.sleep(2)

        # Smoke check
        out = run_cmd(child, "ps | grep -E 'ap2p|provider_srt' | grep -v grep", timeout=10)
        if out.strip():
            print(f"[deploy] Running: {out.strip()}")
        else:
            print("[deploy] WARNING: binary not visible in ps — check /tmp/ambicam_restart.log on camera")

        child.sendline("exit")
        child.close()
        print("[deploy] Deploy complete.")

    finally:
        server.shutdown()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Deploy to Augentix camera via telnet")
    parser.add_argument("--binaries-dir", required=True, help="Directory with built binaries")
    parser.add_argument(
        "--binaries",
        nargs="+",
        default=["ap2p", "ambicam.sh"],
        help="Files to deploy (served from --binaries-dir)",
    )
    parser.add_argument("--http-port", type=int, default=8888)
    args = parser.parse_args()

    deploy(args.binaries_dir, args.binaries, args.http_port)
