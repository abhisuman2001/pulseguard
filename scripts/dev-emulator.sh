#!/usr/bin/env bash
# Run the HOST emulator + dashboard for UI development (no QNX needed).
set -e
cd "$(dirname "$0")/.."
make host >/dev/null
pkill -x pg_emulator 2>/dev/null || true
pkill -f "[d]ashboard/server.py" 2>/dev/null || true
nohup ./build/host/pg_emulator > build/host/emu.log 2>&1 &
nohup python3 host/dashboard/server.py --target 127.0.0.1:7777 --port "${PORT:-8080}" --bind 0.0.0.0 > build/host/dash.log 2>&1 &
sleep 1
echo "Emulator + dashboard running: http://localhost:${PORT:-8080}"
