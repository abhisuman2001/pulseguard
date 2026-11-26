#!/usr/bin/env bash
# Create / start / stop the QNX 8.0 QEMU target with mkqnximage.
#
#   scripts/qnx-vm.sh create   build an x86_64 QEMU image (root ssh via your key)
#   scripts/qnx-vm.sh start    boot it headless in the background
#   scripts/qnx-vm.sh ip       print the target IP
#   scripts/qnx-vm.sh console  boot it in the foreground (serial console; Ctrl-A x quits)
#   scripts/qnx-vm.sh stop
#
# Requires: QNX SDP 8.0 (source ~/qnx800/qnxsdp-env.sh), qemu-system-x86, Ubuntu 22.04/24.04.
set -euo pipefail
cd "$(dirname "$0")/.."
VM_DIR=${VM_DIR:-$PWD/vm}
KEY=${SSH_KEY:-$HOME/.ssh/id_ed25519}

need_sdp() {
  if ! command -v mkqnximage >/dev/null; then
    echo "mkqnximage not found. Run: source ~/qnx800/qnxsdp-env.sh" >&2; exit 1
  fi
}

case "${1:-}" in
  create)
    need_sdp
    [ -f "$KEY" ] || ssh-keygen -t ed25519 -N "" -f "$KEY"
    if [ ! -e /dev/kvm ]; then
      echo "WARNING: /dev/kvm not available - QEMU will use software emulation (TCG)." >&2
      echo "         Timing results will be much worse than on real hardware." >&2
    fi
    mkdir -p "$VM_DIR" && cd "$VM_DIR"
    mkqnximage --type=qemu --arch=x86_64 --hostname=pulseguard --ssh-ident="$KEY" --build
    echo "Image built in $VM_DIR. Next: scripts/qnx-vm.sh start"
    ;;
  start)   need_sdp; cd "$VM_DIR"; mkqnximage --run=-h; mkqnximage --getip ;;
  console) need_sdp; cd "$VM_DIR"; mkqnximage --run ;;
  ip)      need_sdp; cd "$VM_DIR"; mkqnximage --getip ;;
  stop)    need_sdp; cd "$VM_DIR"; mkqnximage --stop ;;
  *) sed -n '2,10p' "$0"; exit 1 ;;
esac
