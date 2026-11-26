#!/usr/bin/env bash
# Build for QNX, copy to the target, (re)start PulseGuard, open the dashboard.
#
#   scripts/deploy.sh [target-ip]          (default: ip from scripts/qnx-vm.sh ip)
#   ARCH=aarch64le scripts/deploy.sh 192.168.1.50   (e.g. Raspberry Pi 4)
set -euo pipefail
cd "$(dirname "$0")/.."
ARCH=${ARCH:-x86_64}
IP=${1:-$(scripts/qnx-vm.sh ip | tail -1)}
DEST=${DEST:-/data/pulseguard}
SSH="ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR root@$IP"

make qnx ARCH="$ARCH"
$SSH "mkdir -p $DEST"
scp -O -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR \
    build/qnx-"$ARCH"/pg_* build/qnx-"$ARCH"/rtbench target/pulseguard.sh root@"$IP":"$DEST"/ 2>/dev/null || \
scp    -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR \
    build/qnx-"$ARCH"/pg_* build/qnx-"$ARCH"/rtbench target/pulseguard.sh root@"$IP":"$DEST"/
$SSH "chmod +x $DEST/pg_* $DEST/pulseguard.sh && $DEST/pulseguard.sh restart"

echo
echo "PulseGuard running on $IP. Starting dashboard -> http://localhost:8080"
exec python3 host/dashboard/server.py --target "$IP:7777" --port 8080
