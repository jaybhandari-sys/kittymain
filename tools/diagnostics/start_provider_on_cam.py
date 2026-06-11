#!/usr/bin/env python3
"""Launch provider_srt on the LAN test camera and watch its log + the cloud
signaling log for SRT_REGISTER."""
import socket, subprocess, sys, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
from deploy_to_camera_telnet import Telnet, login, CAM   # type: ignore

t = Telnet(CAM, timeout=8)
try:
    login(t)
    t.run("killall provider_srt 2>/dev/null; rm -f /tmp/provider_srt.log; sync")
    t.send("cd /tmp && ./provider_srt provider_srt.conf > /tmp/provider_srt.log 2>&1 &")
    time.sleep(0.5)
    out = t.run("sleep 3; ps w | grep provider_srt | grep -v grep; echo --LOG--; cat /tmp/provider_srt.log | tail -25", timeout=12)
    print(out)
finally:
    t.close()

print("=== cloud signaling log (last 20 lines) ===")
subprocess.run([
    "sshpass", "-p", "toR@8155que", "ssh", "-o", "StrictHostKeyChecking=no",
    "root@142.93.223.221",
    "tail -20 /P2PV4_extracted/Signaling_Server/P2P_Libjuice-main/binaries/signaling.log",
])
