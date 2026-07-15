#!/bin/bash
HERE="$(cd "$(dirname "$0")" && pwd)"
pkill -f "$HERE/bin/backend" 2>/dev/null
pkill -f "$HERE/bin/lb"      2>/dev/null
echo "[+] LoadForge stopped"
