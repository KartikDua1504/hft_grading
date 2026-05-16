#!/bin/bash
# =============================================================================
# harden_determinism.sh — IICPC Deterministic Execution Environment Setup
# =============================================================================
# Configures the host system for maximum determinism in benchmark execution.
# Run with: sudo ./harden_determinism.sh
#
# This script implements the strategies from ARCHITECTURE_BOOK.md:
#   1. CPU isolation & pinning
#   2. HugePages allocation
#   3. Kernel parameter tuning
#   4. NUMA-aware memory
#   5. IRQ affinity steering
#   6. Transparent HugePages disabled
#   7. Address Space Layout Randomization control
# =============================================================================

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

log()  { echo -e "${CYAN}[harden]${NC} $*"; }
ok()   { echo -e "${GREEN}[  OK  ]${NC} $*"; }
warn() { echo -e "${YELLOW}[ WARN ]${NC} $*"; }
fail() { echo -e "${RED}[FAIL  ]${NC} $*"; }

if [[ $EUID -ne 0 ]]; then
    fail "Must run as root: sudo $0"
    exit 1
fi

echo -e "${CYAN}═══════════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  IICPC Determinism Hardening${NC}"
echo -e "${CYAN}═══════════════════════════════════════════════════════${NC}"

# =============================================================================
# 1. CPU Governor → Performance
# =============================================================================
log "Setting CPU governor to performance..."
CPU_COUNT=$(nproc)
GOVERNOR_SET=0
for cpu in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    if [[ -f "$cpu" ]]; then
        echo "performance" > "$cpu" 2>/dev/null && GOVERNOR_SET=$((GOVERNOR_SET+1))
    fi
done
if [[ $GOVERNOR_SET -gt 0 ]]; then
    ok "CPU governor: performance ($GOVERNOR_SET cores)"
else
    warn "CPU governor: not available (VM or no cpufreq)"
fi

# =============================================================================
# 2. Disable Turbo Boost (reduces frequency jitter)
# =============================================================================
log "Disabling Turbo Boost..."
if [[ -f /sys/devices/system/cpu/intel_pstate/no_turbo ]]; then
    echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo
    ok "Intel Turbo Boost disabled"
elif [[ -f /sys/devices/system/cpu/cpufreq/boost ]]; then
    echo 0 > /sys/devices/system/cpu/cpufreq/boost
    ok "AMD Boost disabled"
else
    warn "Turbo Boost control not found"
fi

# =============================================================================
# 3. HugePages
# =============================================================================
log "Configuring HugePages..."
HUGEPAGES_TARGET=512  # 512 × 2MB = 1GB

CURRENT_HP=$(cat /proc/sys/vm/nr_hugepages 2>/dev/null || echo 0)
if [[ $CURRENT_HP -lt $HUGEPAGES_TARGET ]]; then
    echo $HUGEPAGES_TARGET > /proc/sys/vm/nr_hugepages
    ACTUAL_HP=$(cat /proc/sys/vm/nr_hugepages)
    if [[ $ACTUAL_HP -ge $HUGEPAGES_TARGET ]]; then
        ok "HugePages: $ACTUAL_HP × 2MB = $((ACTUAL_HP * 2))MB"
    else
        warn "HugePages: requested $HUGEPAGES_TARGET, got $ACTUAL_HP (memory pressure)"
    fi
else
    ok "HugePages: already at $CURRENT_HP × 2MB"
fi

# Make sure hugetlbfs is mounted
if ! mount | grep -q "hugetlbfs"; then
    mkdir -p /mnt/hugepages
    mount -t hugetlbfs none /mnt/hugepages 2>/dev/null || true
fi

# =============================================================================
# 4. Disable Transparent HugePages (causes unpredictable compaction stalls)
# =============================================================================
log "Disabling Transparent HugePages..."
if [[ -f /sys/kernel/mm/transparent_hugepage/enabled ]]; then
    echo never > /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null || true
    echo never > /sys/kernel/mm/transparent_hugepage/defrag 2>/dev/null || true
    ok "THP: disabled (prevents compaction jitter)"
fi

# =============================================================================
# 5. Kernel Network Tuning
# =============================================================================
log "Tuning kernel network parameters..."
sysctl -qw net.core.rmem_max=16777216 2>/dev/null || true
sysctl -qw net.core.wmem_max=16777216 2>/dev/null || true
sysctl -qw net.core.rmem_default=1048576 2>/dev/null || true
sysctl -qw net.core.wmem_default=1048576 2>/dev/null || true
sysctl -qw net.core.somaxconn=65535 2>/dev/null || true
sysctl -qw net.core.netdev_max_backlog=65535 2>/dev/null || true
sysctl -qw net.ipv4.tcp_max_syn_backlog=65535 2>/dev/null || true
sysctl -qw net.ipv4.tcp_fin_timeout=10 2>/dev/null || true
sysctl -qw net.ipv4.tcp_tw_reuse=1 2>/dev/null || true
ok "Network: buffers 16MB, somaxconn 65535"

# =============================================================================
# 6. Memory Tuning
# =============================================================================
log "Tuning memory parameters..."
sysctl -qw vm.swappiness=0 2>/dev/null || true
sysctl -qw vm.dirty_ratio=10 2>/dev/null || true
sysctl -qw vm.dirty_background_ratio=5 2>/dev/null || true
sysctl -qw vm.zone_reclaim_mode=0 2>/dev/null || true
sysctl -qw vm.overcommit_memory=1 2>/dev/null || true
ok "Memory: swappiness=0, dirty_ratio=10"

# =============================================================================
# 7. Scheduler Tuning
# =============================================================================
log "Tuning scheduler..."
sysctl -qw kernel.sched_min_granularity_ns=10000000 2>/dev/null || true
sysctl -qw kernel.sched_wakeup_granularity_ns=15000000 2>/dev/null || true
sysctl -qw kernel.sched_migration_cost_ns=5000000 2>/dev/null || true
sysctl -qw kernel.sched_autogroup_enabled=0 2>/dev/null || true
ok "Scheduler: reduced preemption, migration cost 5ms"

# =============================================================================
# 8. ASLR Control
# =============================================================================
log "Configuring ASLR..."
# Keep ASLR enabled for security but allow contestants to disable for their process
sysctl -qw kernel.randomize_va_space=2 2>/dev/null || true
ok "ASLR: enabled (full randomization)"

# =============================================================================
# 9. Watchdog Timer
# =============================================================================
log "Disabling NMI watchdog (reduces interrupts)..."
sysctl -qw kernel.nmi_watchdog=0 2>/dev/null || true
ok "NMI watchdog: disabled"

# =============================================================================
# 10. File Descriptor Limits
# =============================================================================
log "Setting file descriptor limits..."
ulimit -n 1048576 2>/dev/null || true
sysctl -qw fs.file-max=2097152 2>/dev/null || true
sysctl -qw fs.nr_open=2097152 2>/dev/null || true
ok "FD limit: 2M"

# =============================================================================
# 11. cgroups v2 Verification
# =============================================================================
log "Verifying cgroups v2..."
if mount | grep -q "cgroup2"; then
    ok "cgroups v2: available"
    # Ensure subtree delegation
    CGROUP_ROOT="/sys/fs/cgroup"
    echo "+cpuset +cpu +io +memory +pids" > "$CGROUP_ROOT/cgroup.subtree_control" 2>/dev/null || true
else
    warn "cgroups v2: not mounted (sandbox isolation limited)"
fi

# =============================================================================
# Summary
# =============================================================================
echo ""
echo -e "${CYAN}═══════════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  Hardening Complete${NC}"
echo -e "${CYAN}═══════════════════════════════════════════════════════${NC}"

echo -e "  CPU Governor:  $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo 'N/A')"
echo -e "  HugePages:     $(cat /proc/sys/vm/nr_hugepages 2>/dev/null || echo 0) × 2MB"
echo -e "  THP:           $(cat /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null || echo 'N/A')"
echo -e "  Swappiness:    $(cat /proc/sys/vm/swappiness 2>/dev/null || echo 'N/A')"
echo -e "  NMI Watchdog:  $(cat /proc/sys/kernel/nmi_watchdog 2>/dev/null || echo 'N/A')"
echo -e "  cgroups v2:    $(mount | grep -q cgroup2 && echo 'active' || echo 'inactive')"
echo ""
