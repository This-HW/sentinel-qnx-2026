#!/bin/sh
# deploy-to-qemu.sh - copy the cross-built binaries into a running QNX VM.
#
# Prerequisite (once, on the host):
#   mkdir qnx-vm && cd qnx-vm
#   mkqnximage --type=qemu --build
#   mkqnximage --run            # add =-h for a headless guest
#   mkqnximage --getip          # prints the guest IP
#
# Then:
#   ./qnx/deploy-to-qemu.sh <vm-ip> [ssh-identity]
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
IP=${1:?usage: deploy-to-qemu.sh <vm-ip> [identity]}
IDENT=${2:-}
SCP="scp -o StrictHostKeyChecking=no"
[ -n "$IDENT" ] && SCP="$SCP -i $IDENT"

for f in sentineld plant-sim loadgen sentinel-ctl; do
    if [ ! -x "$ROOT/build-qnx/$f" ]; then
        echo "error: build-qnx/$f missing; run ./qnx/build-qnx.sh first" >&2
        exit 1
    fi
done

$SCP "$ROOT/build-qnx/sentineld" "$ROOT/build-qnx/plant-sim" \
     "$ROOT/build-qnx/loadgen" "$ROOT/build-qnx/sentinel-ctl" \
     "$ROOT/etc/sentinel.conf" "$ROOT/qnx/run-demo.sh" \
     "root@$IP:/tmp/"

echo "deployed. now on the guest:"
echo "  ssh root@$IP"
echo "  cd /tmp && chmod +x run-demo.sh sentineld plant-sim loadgen sentinel-ctl && ./run-demo.sh"
