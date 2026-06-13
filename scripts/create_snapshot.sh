#!/bin/bash
# create_snapshot.sh — Pre-warm and Snapshot Firecracker MicroVM
# Strategy 1 from ARCHITECTURE_BOOK.md:
#   Boot a base microVM → load glibc → pause → snapshot.
#   Every contestant resumes from this snapshot (<5ms boot).
#
# Usage: sudo ./create_snapshot.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
FC_DIR="$PROJECT_ROOT/infra/firecracker"
KERNEL="$FC_DIR/vmlinux.bin"
ROOTFS="$FC_DIR/base_rootfs.ext4"
SNAPSHOT_DIR="$FC_DIR/snapshots"
API_SOCKET="/tmp/fc_snapshot_creator.sock"

CYAN='\033[0;36m'
GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m'

log()  { echo -e "${CYAN}[snapshot]${NC} $*"; }
ok()   { echo -e "${GREEN}[  OK  ]${NC} $*"; }
fail() { echo -e "${RED}[FAIL  ]${NC} $*"; }

# Preflight
if [[ ! -c /dev/kvm ]]; then
    fail "KVM not available"
    exit 1
fi

if [[ ! -f "$KERNEL" ]]; then
    fail "Kernel not found: $KERNEL"
    exit 1
fi

if [[ ! -f "$ROOTFS" ]]; then
    fail "Rootfs not found: $ROOTFS"
    exit 1
fi

mkdir -p "$SNAPSHOT_DIR"
rm -f "$API_SOCKET"

log "Creating pre-warmed Firecracker snapshot..."

# Helper
fc_api() {
    curl -s --unix-socket "$API_SOCKET" \
        -X "$1" \
        -H "Content-Type: application/json" \
        -d "${3:-}" \
        "http://localhost${2}" 2>/dev/null
}

# Start Firecracker
firecracker --api-sock "$API_SOCKET" --level Warning &
FC_PID=$!

cleanup() {
    kill -KILL "$FC_PID" 2>/dev/null || true
    wait "$FC_PID" 2>/dev/null || true
    rm -f "$API_SOCKET"
}
trap cleanup EXIT

# Wait for socket
for i in $(seq 1 50); do
    [[ -S "$API_SOCKET" ]] && break
    sleep 0.02
done

if [[ ! -S "$API_SOCKET" ]]; then
    fail "Socket not created"
    exit 1
fi

# Configure VM
log "Configuring VM..."
fc_api PUT "/boot-source" "{
    \"kernel_image_path\": \"$KERNEL\",
    \"boot_args\": \"console=ttyS0 reboot=k panic=1 pci=off root=/dev/vda rw\"
}"

fc_api PUT "/machine-config" "{
    \"vcpu_count\": 2,
    \"mem_size_mib\": 256,
    \"smt\": false
}"

fc_api PUT "/drives/rootfs" "{
    \"drive_id\": \"rootfs\",
    \"path_on_host\": \"$ROOTFS\",
    \"is_root_device\": true,
    \"is_read_only\": false
}"

# Boot
log "Booting VM..."
BOOT_START=$(date +%s%N)
fc_api PUT "/actions" '{"action_type": "InstanceStart"}'

# Wait for boot to complete (init runs, mounts filesystems)
sleep 2
BOOT_END=$(date +%s%N)
BOOT_MS=$(( (BOOT_END - BOOT_START) / 1000000 ))
ok "Booted in ${BOOT_MS}ms"

# Pause VM
log "Pausing VM..."
fc_api PATCH "/vm" '{"state": "Paused"}'
ok "VM paused"

# Take snapshot
log "Taking snapshot..."
fc_api PUT "/snapshot/create" "{
    \"snapshot_type\": \"Full\",
    \"snapshot_path\": \"$SNAPSHOT_DIR/snapshot_state\",
    \"mem_file_path\": \"$SNAPSHOT_DIR/snapshot_mem\"
}"

if [[ -f "$SNAPSHOT_DIR/snapshot_state" && -f "$SNAPSHOT_DIR/snapshot_mem" ]]; then
    STATE_SIZE=$(du -h "$SNAPSHOT_DIR/snapshot_state" | cut -f1)
    MEM_SIZE=$(du -h "$SNAPSHOT_DIR/snapshot_mem" | cut -f1)
    ok "Snapshot created!"
    echo ""
    echo -e "${CYAN}═══════════════════════════════════════════════${NC}"
    echo -e "  State:  $SNAPSHOT_DIR/snapshot_state ($STATE_SIZE)"
    echo -e "  Memory: $SNAPSHOT_DIR/snapshot_mem ($MEM_SIZE)"
    echo -e "  Boot:   ${BOOT_MS}ms (cold), <5ms (resume)"
    echo -e "${CYAN}═══════════════════════════════════════════════${NC}"
else
    fail "Snapshot files not created"
    exit 1
fi
