#!/bin/bash
# run_demo.sh — Full LoadForge v3 demonstration (dynamic features)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

LB_IP="127.0.0.1"
LB_PORT=8080
CLIENT="$HERE/bin/client"
LB="$HERE/bin/lb"
BACKEND="$HERE/bin/backend"
CTL="$HERE/bin/lf-ctl"
CB_WAIT=35   # CB_OPEN_TIMEOUT_SEC (30) + margin

echo ""
echo "╔══════════════════════════════════════════╗"
echo "║   LoadForge v3 — Dynamic Feature Demo    ║"
echo "╚══════════════════════════════════════════╝"
echo ""

# Build if needed
if [ ! -f "$LB" ] || [ ! -f "$BACKEND" ] || [ ! -f "$CLIENT" ] || [ ! -f "$CTL" ]; then
    echo "[*] Building..."
    make all
fi

cleanup() {
    pkill -f "$HERE/bin/backend" 2>/dev/null || true
    pkill -f "$HERE/bin/lb"      2>/dev/null || true
    sleep 0.5
}
trap cleanup EXIT

cleanup
rm -f /tmp/lf_stats.log

echo "[1] Starting backends (1 & 3 fast, 2 CPU-intensive)..."
"$BACKEND" 127.0.0.1 9001 1         > /tmp/lf_b1.log 2>&1 &
"$BACKEND" 127.0.0.1 9002 2 cpu     > /tmp/lf_b2.log 2>&1 &
"$BACKEND" 127.0.0.1 9003 3         > /tmp/lf_b3.log 2>&1 &
sleep 1

echo "[2] Starting load balancer..."
printf '\n' | "$LB" config/config.json > /tmp/lf_lb.log 2>&1 &
sleep 2

send_burst() {
    local n=$1 label=$2
    echo ""
    echo "── $label ($n requests) ──"
    "$CLIENT" --lb "${LB_IP}:${LB_PORT}" --burst "$n"
    sleep 1
}

echo "[3] Initial distribution (warm-up / round-robin phase)..."
send_burst 20 "Phase A — warm-up"

echo "[4] Waiting for warm-up to complete..."
sleep 5

echo "[5] Adaptive routing (backend 2 should receive less traffic)..."
send_burst 30 "Phase B — adaptive"

echo "[6] Adding backend 4 via lf-ctl..."
"$CTL" add 127.0.0.1 9004
"$BACKEND" 127.0.0.1 9004 4 > /tmp/lf_b4.log 2>&1 &
sleep 1
send_burst 20 "Phase C — after dynamic add"

echo "[7] Killing backend 2 (circuit breaker OPEN)..."
pkill -f "backend 127.0.0.1 9002" 2>/dev/null || true
sleep 1
send_burst 10 "Phase D — backend 2 down"

echo "[8] Waiting ${CB_WAIT}s for HALF_OPEN → CLOSED recovery..."
sleep "$CB_WAIT"
send_burst 10 "Phase E — after CB recovery window"

echo ""
echo "── Stats log (last 5 lines) ──"
if [ -f /tmp/lf_stats.log ]; then
    tail -5 /tmp/lf_stats.log
else
    echo "(no stats log yet — wait for STATS_LOGGER_INTERVAL_SEC=10)"
fi

echo ""
echo "── lf-ctl status ──"
"$CTL" status

echo ""
echo "Demo complete. LB log: /tmp/lf_lb.log"
