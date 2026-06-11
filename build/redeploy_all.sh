#!/bin/bash
# Rebuild + redeploy: signaling (cloud), provider_srt (camera ARM).
# All builds go through the Makefile so the same recipe runs identically
# on WSL (dev) or on the cloud server (where the rebuild can be in-place).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"

echo "=========================================="
echo "[1/4] rebuild everything via 'make all'"
echo "=========================================="
"$(command -v make)" -C "$ROOT" all 2>&1 | tail -10

echo
echo "=========================================="
echo "[2/4] redeploy signaling to cloud (atomic mv + restart)"
echo "=========================================="
"$ROOT/deploy_signaling.sh" 2>&1 | tail -10

echo
echo "=========================================="
echo "[3/4] redeploy provider_srt to LAN camera + relaunch"
echo "=========================================="
python3 "$ROOT/deploy_to_camera_telnet.py" --service-id cam001 2>&1 | tail -8
python3 "$ROOT/start_provider_on_cam.py" 2>&1 | tail -10

echo
echo "=========================================="
echo "[4/4] verify pipeline end-to-end (camera → cloud consumer → byte-level FLV walk)"
echo "=========================================="
sleep 2
python3 "$ROOT/cloud_e2e_test.py" 2>&1 | tail -30
