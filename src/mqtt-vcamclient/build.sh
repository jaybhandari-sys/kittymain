#!/bin/bash
# Build script - run this inside WSL
# Usage: wsl bash /mnt/c/Users/super/Downloads/Retail_kitty-10/build.sh

set -e

PROJECT_DIR="/mnt/c/Users/super/Downloads/Retail_kitty-10"
cd "$PROJECT_DIR"

echo "=== Cross-compiling MQTT Camera Client for ARM ==="
echo ""

make clean 2>/dev/null || true
make

echo ""
file "$PROJECT_DIR/MQTT_vcamclient_Augentix"
echo ""
echo "=== Build complete ==="
