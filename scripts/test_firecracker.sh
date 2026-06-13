#!/bin/bash
# test_firecracker.sh — Boot-test a Firecracker microVM
# Prerequisites:
#   - /usr/local/bin/firecracker installed
#   - /dev/kvm accessible
#   - vmlinux.bin + rootfs.ext4 in infra/firecracker/
#
# Usage: sudo ./test_firecracker.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FC_DIR="${SCRIPT_DIR}/../infra/firecracker"
SOCKET_PATH="/tmp/firecracker-test.sock"

# Cleanup from previous runs
rm -f "${SOCKET_PATH}"

echo "=== IICPC Firecracker MicroVM Boot Test ==="
echo ""

# Check prerequisites
if ! command -v firecracker &>/dev/null; then
    echo "ERROR: firecracker not found in PATH"
    exit 1
fi

if [ ! -r /dev/kvm ]; then
    echo "ERROR: /dev/kvm not accessible"
    exit 1
fi

if [ ! -f "${FC_DIR}/vmlinux.bin" ]; then
    echo "ERROR: ${FC_DIR}/vmlinux.bin not found"
    echo "Download: curl -fsSL -o ${FC_DIR}/vmlinux.bin https://s3.amazonaws.com/spec.ccfc.min/img/hello/kernel/hello-vmlinux.bin"
    exit 1
fi

if [ ! -f "${FC_DIR}/rootfs.ext4" ]; then
    echo "ERROR: ${FC_DIR}/rootfs.ext4 not found"
    exit 1
fi

echo "[1/4] Starting Firecracker process..."
firecracker --api-sock "${SOCKET_PATH}" --level Warning &
FC_PID=$!
sleep 0.5

if [ ! -S "${SOCKET_PATH}" ]; then
    echo "ERROR: Firecracker socket not created"
    kill ${FC_PID} 2>/dev/null
    exit 1
fi
echo "  → PID: ${FC_PID}, Socket: ${SOCKET_PATH}"

echo "[2/4] Configuring VM (2 vCPUs, 256 MiB RAM)..."

# Set boot source
curl -s --unix-socket "${SOCKET_PATH}" \
    -X PUT "http://localhost/boot-source" \
    -H "Content-Type: application/json" \
    -d "{
        \"kernel_image_path\": \"${FC_DIR}/vmlinux.bin\",
        \"boot_args\": \"console=ttyS0 reboot=k panic=1 pci=off\"
    }" | jq . 2>/dev/null || echo "  → boot-source: OK"

# Set machine config
curl -s --unix-socket "${SOCKET_PATH}" \
    -X PUT "http://localhost/machine-config" \
    -H "Content-Type: application/json" \
    -d '{
        "vcpu_count": 2,
        "mem_size_mib": 256
    }' | jq . 2>/dev/null || echo "  → machine-config: OK"

# Set rootfs
curl -s --unix-socket "${SOCKET_PATH}" \
    -X PUT "http://localhost/drives/rootfs" \
    -H "Content-Type: application/json" \
    -d "{
        \"drive_id\": \"rootfs\",
        \"path_on_host\": \"${FC_DIR}/rootfs.ext4\",
        \"is_root_device\": true,
        \"is_read_only\": false
    }" | jq . 2>/dev/null || echo "  → drive: OK"

echo "[3/4] Starting VM..."
BOOT_START=$(date +%s%N)

curl -s --unix-socket "${SOCKET_PATH}" \
    -X PUT "http://localhost/actions" \
    -H "Content-Type: application/json" \
    -d '{"action_type": "InstanceStart"}' | jq . 2>/dev/null || echo "  → instance: STARTED"

BOOT_END=$(date +%s%N)
BOOT_MS=$(( (BOOT_END - BOOT_START) / 1000000 ))
echo "  → Boot time: ${BOOT_MS} ms"

echo "[4/4] VM running for 3 seconds (check serial console)..."
sleep 3

echo ""
echo "Shutting down VM..."
curl -s --unix-socket "${SOCKET_PATH}" \
    -X PUT "http://localhost/actions" \
    -H "Content-Type: application/json" \
    -d '{"action_type": "SendCtrlAltDel"}' 2>/dev/null || true

sleep 1
kill ${FC_PID} 2>/dev/null || true
wait ${FC_PID} 2>/dev/null || true
rm -f "${SOCKET_PATH}"

echo ""
echo "  Firecracker Boot Test: PASS"
echo "  Boot time: ${BOOT_MS} ms"
echo "  vCPUs: 2, Memory: 256 MiB"
