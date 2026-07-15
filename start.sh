#!/bin/bash
# start.sh — Start LoadForge v3 (backends + load balancer)

HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"
chmod +x "$HERE"/*.sh 2>/dev/null

# Auto-build
if [ ! -f bin/lb ] || [ ! -f bin/backend ] || [ ! -f bin/client ]; then
    echo "[*] Building LoadForge v3..."
    make || { echo "[!] Build failed. Install: sudo apt install gcc make libjson-c-dev"; exit 1; }
fi

# Kill leftovers
pkill -f "$HERE/bin/backend" 2>/dev/null
pkill -f "$HERE/bin/lb"      2>/dev/null
sleep 0.3

echo "[+] Backend 1 → 127.0.0.1:9001"
"$HERE/bin/backend" 127.0.0.1 9001 1 > /tmp/lf_b1.log 2>&1 &

echo "[+] Backend 2 → 127.0.0.1:9002"
"$HERE/bin/backend" 127.0.0.1 9002 2 > /tmp/lf_b2.log 2>&1 &

echo "[+] Backend 3 → 127.0.0.1:9003"
"$HERE/bin/backend" 127.0.0.1 9003 3 > /tmp/lf_b3.log 2>&1 &

sleep 0.5

echo "[*] Starting Load Balancer → 127.0.0.1:8080"
exec "$HERE/bin/lb" config/config.json
