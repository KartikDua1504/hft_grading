#!/bin/bash
# =============================================================================
# build_rootfs.sh — Create Minimal Alpine Rootfs for Firecracker
# =============================================================================
# Produces the SMALLEST possible Linux rootfs that can run a statically-linked
# C/C++ binary. Nothing else. No shell, no package manager, no extra tools.
#
# Target size: ~5-8MB ext4 image
#
# What's inside:
#   /init               → Busybox init (OpenRC-compatible)
#   /usr/bin/contestant  → Mount point for contestant's compiled binary
#   /tmp/gateway.sock   → UDS endpoint for host communication
#   /dev, /proc, /sys   → Minimal device nodes
#
# Usage: sudo ./build_rootfs.sh [output_path]
# =============================================================================

set -euo pipefail

OUTPUT="${1:-/home/junior/Desktop/Coding/IICPC/infra/firecracker/contestant_rootfs.ext4}"
ROOTFS_DIR="/tmp/iicpc_rootfs_$$"
IMAGE_SIZE_MB=64   # 64MB — plenty for a static binary + small runtime
ALPINE_VERSION="3.20"
ALPINE_ARCH="x86_64"
ALPINE_MIRROR="https://dl-cdn.alpinelinux.org/alpine"

echo "=== IICPC Minimal Rootfs Builder ==="
echo "  Output: ${OUTPUT}"
echo "  Size:   ${IMAGE_SIZE_MB}MB"
echo ""

# ============================================================
# 1. Create ext4 image
# ============================================================
echo "[1/6] Creating ${IMAGE_SIZE_MB}MB ext4 image..."
dd if=/dev/zero of="${OUTPUT}" bs=1M count=${IMAGE_SIZE_MB} status=none
mkfs.ext4 -F -q -L "iicpc_rootfs" "${OUTPUT}"

# ============================================================
# 2. Mount and populate
# ============================================================
echo "[2/6] Mounting image..."
mkdir -p "${ROOTFS_DIR}"
mount -o loop "${OUTPUT}" "${ROOTFS_DIR}"

trap "umount ${ROOTFS_DIR} 2>/dev/null; rm -rf ${ROOTFS_DIR}" EXIT

# ============================================================
# 3. Install Alpine minirootfs
# ============================================================
echo "[3/6] Installing Alpine ${ALPINE_VERSION} minirootfs..."
MINIROOTFS_URL="${ALPINE_MIRROR}/v${ALPINE_VERSION}/releases/${ALPINE_ARCH}/alpine-minirootfs-${ALPINE_VERSION}.0-${ALPINE_ARCH}.tar.gz"

# Try download, fallback to local if available
TARBALL="/tmp/alpine-minirootfs-${ALPINE_VERSION}.tar.gz"
if [ ! -f "${TARBALL}" ]; then
    curl -fsSL -o "${TARBALL}" "${MINIROOTFS_URL}" || {
        echo "ERROR: Failed to download Alpine minirootfs"
        echo "  URL: ${MINIROOTFS_URL}"
        echo "  Download manually and place at: ${TARBALL}"
        exit 1
    }
fi

tar xzf "${TARBALL}" -C "${ROOTFS_DIR}"

# ============================================================
# 4. Minimal init system
# ============================================================
echo "[4/6] Configuring minimal init..."

# Create the init script that launches the contestant binary
cat > "${ROOTFS_DIR}/init" << 'INIT_EOF'
#!/bin/sh
# IICPC Contestant Init — Hardened
# Security: drop caps, no-new-privs, ulimits, read-only /proc

mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev

# Remount /proc read-only (prevent sysrq, kcore abuse)
mount -o remount,ro /proc 2>/dev/null || true

hostname iicpc-contestant
mkdir -p /tmp

# Enforce resource limits (defense-in-depth, VM already constrains)
ulimit -v 262144       # 256MB virtual memory
ulimit -f 10240        # 10MB max file size
ulimit -n 64           # 64 file descriptors
ulimit -u 8            # 8 processes (prevent fork bombs)
ulimit -t 120          # 120s CPU time

if [ ! -x /usr/bin/contestant ]; then
    echo "[init] ERROR: No contestant binary at /usr/bin/contestant"
    poweroff -f 2>/dev/null || reboot -f
    exit 1
fi

echo "[init] Starting contestant (hardened)..."

# Run with no-new-privileges and dropped capabilities if setpriv available
if command -v setpriv >/dev/null 2>&1; then
    exec setpriv --no-new-privs --inh-caps=-all \
         -- /usr/bin/contestant --gateway /tmp/gateway.sock
else
    exec /usr/bin/contestant --gateway /tmp/gateway.sock
fi
INIT_EOF
chmod 755 "${ROOTFS_DIR}/init"

# OpenRC inittab pointing to our init
cat > "${ROOTFS_DIR}/etc/inittab" << 'INITTAB_EOF'
::sysinit:/init
::ctrlaltdel:/sbin/reboot
::shutdown:/bin/umount -a -r
INITTAB_EOF

# ============================================================
# 5. Strip unnecessary files
# ============================================================
echo "[5/6] Stripping unnecessary files..."

# Remove everything we don't need
rm -rf "${ROOTFS_DIR}/var/cache/apk"
rm -rf "${ROOTFS_DIR}/etc/apk"
rm -rf "${ROOTFS_DIR}/lib/apk"
rm -rf "${ROOTFS_DIR}/usr/share/man"
rm -rf "${ROOTFS_DIR}/usr/share/doc"
rm -rf "${ROOTFS_DIR}/usr/share/misc"
rm -rf "${ROOTFS_DIR}/usr/lib/engines*"
rm -rf "${ROOTFS_DIR}/usr/lib/modules"
rm -rf "${ROOTFS_DIR}/media"
rm -rf "${ROOTFS_DIR}/mnt"
rm -rf "${ROOTFS_DIR}/srv"
rm -rf "${ROOTFS_DIR}/opt"
rm -rf "${ROOTFS_DIR}/run"
rm -rf "${ROOTFS_DIR}/root/.ash_history"

# Create placeholder for contestant binary
touch "${ROOTFS_DIR}/usr/bin/contestant"
chmod 755 "${ROOTFS_DIR}/usr/bin/contestant"

# Create required directories
mkdir -p "${ROOTFS_DIR}/tmp"
mkdir -p "${ROOTFS_DIR}/dev"
mkdir -p "${ROOTFS_DIR}/proc"
mkdir -p "${ROOTFS_DIR}/sys"

# ============================================================
# 6. Report
# ============================================================
echo "[6/6] Finalizing..."

# Umount handled by trap
sync

USED=$(du -sh "${ROOTFS_DIR}" | cut -f1)
echo ""
echo "╔════════════════════════════════════════════════════════╗"
echo "║  Rootfs built successfully                            ║"
echo "╠════════════════════════════════════════════════════════╣"
echo "║  Image:     ${OUTPUT}"
echo "║  Used:      ${USED}"
echo "║  Init:      /init (launches /usr/bin/contestant)"
echo "║  Socket:    /tmp/gateway.sock"
echo "║  Base:      Alpine ${ALPINE_VERSION} musl libc"
echo "╚════════════════════════════════════════════════════════╝"
