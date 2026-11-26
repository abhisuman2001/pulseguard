#!/usr/bin/env bash
# Collect the numbers for README "Results" into docs/RESULTS.md.
#   scripts/collect-results.sh [target-ip]
# Runs: rtbench on QNX (idle + stress), rtbench on Linux (idle + stress, needs sudo
# for SCHED_FIFO), then live fault-tolerance tests against the running PulseGuard.
set -uo pipefail
cd "$(dirname "$0")/.."
IP=${1:-$(scripts/qnx-vm.sh ip | tail -1)}
D=/data/pulseguard
SSH="ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR root@$IP"
OUT=docs/RESULTS.md
SECS=${SECS:-30}

{
echo "# Results"
echo
echo "Collected $(date -u +%Y-%m-%dT%H:%MZ) - host: $(uname -sr), $(nproc) CPUs, KVM: $([ -e /dev/kvm ] && echo yes || echo no)"
echo
echo '## Timing: same 1 kHz loop (tools/rtbench.c), '"$SECS"' s each'
echo '```'
$SSH "uname -a"
$SSH "$D/pulseguard.sh stop" >/dev/null 2>&1
echo "--- QNX idle";   $SSH "$D/rtbench -s $SECS -p 60"
echo "--- QNX stress (FIFO 55 busy loop on CPUs 1..N-1)"
$SSH "$D/pg_stressor & sleep 1; $D/rtbench -s $SECS -p 60; slay -f pg_stressor"
make -s build/host/rtbench
echo "--- Linux idle";   sudo ./build/host/rtbench -s "$SECS" -p 60
echo "--- Linux stress ($(($(nproc)-1)) busy threads)"; sudo ./build/host/rtbench -s "$SECS" -p 60 -l $(($(nproc)-1))
echo '```'
} | tee "$OUT"

$SSH "$D/pulseguard.sh start" >/dev/null 2>&1
sleep 8

python3 - "$IP" >> "$OUT" <<'EOF'
import json, socket, sys, time
ip = sys.argv[1]
s = socket.create_connection((ip, 7777)); f = s.makefile("r")
def frame():
    while True:
        line = f.readline()
        if line.startswith("{"): return json.loads(line)
def cmd(o): s.sendall((json.dumps(o) + "\n").encode())
def settle(sec):
    end = time.time() + sec; fr = None
    while time.time() < end: fr = frame()
    return fr

print("\n## Control loop under PulseGuard (live system)\n")
base = settle(5)["ctl"]
cmd({"cmd": "stress", "on": 1}); st = settle(20)["ctl"]; cmd({"cmd": "stress", "on": 0})
print(f"- idle: cycles {base['cycles']}, misses {base['misses']}, jitter max {base['jmax']:.1f} us")
print(f"- after 20 s CPU stress: cycles {st['cycles']}, misses {st['misses']}, jitter max {st['jmax']:.1f} us")

print("\n## Fault tolerance\n")
cmd({"cmd": "crash"}); fr = settle(3)
wd = fr["wd"]
print(f"- kill controller: detection {wd['detect_us']/1000:.1f} ms after last heartbeat, "
      f"recovery (first new heartbeat) {wd['recov_us']/1000:.1f} ms, event: {wd['text']}")
cmd({"cmd": "rogue", "on": 1}); fr = settle(1.5)
ai = fr["ai"]
print(f"- rogue AI: state {'THROTTLED (demoted to prio 5)' if ai['state'] == 1 else ai['state']}, "
      f"clamped {ai['clamped']}, rejected {ai['rejected']}")
fr = settle(4)
print(f"- watchdog: aiguard restarts {fr['wd']['restarts'][2]}, last event: {fr['wd']['text']}")
print(f"- control loop during all of it: misses {fr['ctl']['misses']}, jitter max {fr['ctl']['jmax']:.1f} us")
EOF
echo; echo "Wrote $OUT"
