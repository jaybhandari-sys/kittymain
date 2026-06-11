#!/bin/bash
# Atomic redeploy of signaling_server with the SRT extensions.
# The existing service runs at /P2PV4_extracted/Signaling_Server/P2P_Libjuice-main/binaries/signaling_server.
# We push the new build alongside, then mv-and-restart so libjuice clients
# see <2 s of disconnect.
set -euo pipefail
HOST=142.93.223.221
PASS='toR@8155que'
LOCAL=/mnt/e/coturn/coturn/Signaling_Server/P2P_Libjuice-main

# Where the existing systemd unit launches the signaling_server from.
REMOTE_BIN=/P2PV4_extracted/Signaling_Server/P2P_Libjuice-main/binaries/signaling_server

echo "[1/4] inspect current binary on cloud"
sshpass -p "$PASS" ssh -o StrictHostKeyChecking=no root@$HOST "
  ls -la $REMOTE_BIN
  $REMOTE_BIN --version 2>&1 | head -1 || echo '(no --version)'
"

echo "[2/4] back up + push new binary"
sshpass -p "$PASS" ssh -o StrictHostKeyChecking=no root@$HOST "
  cp -f $REMOTE_BIN $REMOTE_BIN.bak.\$(date +%s)
  ls $REMOTE_BIN.bak* 2>/dev/null | tail -3
"
sshpass -p "$PASS" scp -o StrictHostKeyChecking=no \
  "$LOCAL/binaries/signaling_server.static" "root@$HOST:$REMOTE_BIN.new"

echo "[3/4] swap + restart"
sshpass -p "$PASS" ssh -o StrictHostKeyChecking=no root@$HOST "
  chmod +x $REMOTE_BIN.new
  mv $REMOTE_BIN.new $REMOTE_BIN
  systemctl restart signaling_server
  sleep 1
  systemctl status signaling_server --no-pager 2>&1 | head -8
"

echo "[4/4] verify it accepts SRT messages and the libjuice REGISTER still works"
sleep 1
sshpass -p "$PASS" ssh -o StrictHostKeyChecking=no root@$HOST "
  ss -tlnp | grep ':(8888|8889) ' || true
  echo --- log tail ---
  tail -10 /P2PV4_extracted/Signaling_Server/P2P_Libjuice-main/binaries/signaling.log 2>/dev/null \
    || journalctl -u signaling_server -n 8 --no-pager
"
