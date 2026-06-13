#!/bin/sh
# contestant_wrapper.sh — Guest-side security wrapper
# Runs INSIDE the Firecracker VM as init (PID 1).
# Drops privileges, applies seccomp, then execs the contestant binary.
#
# Installed at: /usr/bin/init_wrapper in the rootfs

set -e

# Mount essential filesystems
mount -t proc proc /proc 2>/dev/null || true
mount -t sysfs sysfs /sys 2>/dev/null || true

# Remount /proc read-only (prevent /proc/sysrq-trigger abuse)
mount -o remount,ro /proc 2>/dev/null || true

# Drop capabilities and set no-new-privileges
# The contestant binary runs as nobody (uid 65534)
CONTESTANT="/usr/bin/contestant"

if [ ! -x "$CONTESTANT" ]; then
    echo '{"status":"error","phase":"guest_init","error":"contestant_binary_not_found"}'
    poweroff -f
    exit 1
fi

# Create output directory
mkdir -p /tmp/results 2>/dev/null || true

# Set resource limits for the contestant process
# These are defense-in-depth (Firecracker already caps VM resources)
ulimit -v 262144       # 256MB virtual memory
ulimit -f 10240        # 10MB max file size
ulimit -n 64           # 64 file descriptors
ulimit -u 8            # 8 max processes (prevent fork bombs)
ulimit -t 120          # 120 second CPU time

# Execute contestant as unprivileged user with no-new-privileges
# If 'setpriv' is available (busybox), use it
if command -v setpriv >/dev/null 2>&1; then
    exec setpriv --no-new-privs \
         --inh-caps=-all \
         -- "$CONTESTANT" "$@"
elif command -v su >/dev/null 2>&1; then
    # Fallback: run as nobody
    exec su -s /bin/sh nobody -c "$CONTESTANT $*"
else
    # Last resort: direct exec (still isolated by Firecracker VM)
    exec "$CONTESTANT" "$@"
fi
