#!/usr/bin/env python3
"""Pull camera-side provider_srt.log via telnet so we can see its reconnect history."""
import sys, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
from deploy_to_camera_telnet import Telnet, login, CAM   # type: ignore

t = Telnet(CAM, timeout=10)
try:
    login(t)
    print("=== ps for provider_srt ===")
    print(t.run("ps w | grep provider_srt | grep -v grep", timeout=8).strip())
    print()
    print("=== /tmp/provider_srt.log (full) ===")
    print(t.run("cat /tmp/provider_srt.log 2>/dev/null", timeout=15))
    print()
    print("=== reconnect / TCP-closed-by-peer / errors ===")
    print(t.run("grep -E 'reconnecting|TCP closed|TCP died|signaling.*lost|cancel|ERROR|WARN|signaling: registered' /tmp/provider_srt.log 2>/dev/null", timeout=8))
    print()
    print("=== uptime / dmesg tail ===")
    print(t.run("uptime; dmesg 2>/dev/null | tail -5", timeout=8).strip())
finally:
    t.close()
