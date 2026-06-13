#!/bin/bash
# harden_determinism.sh — IICPC System Tuning for HFT Benchmarking
# Configures the host for maximum throughput and low-latency execution.
#
# Modes:
#   sudo ./harden_determinism.sh              # "perf" mode (default) — Turbo ON, max throughput
#   sudo ./harden_determinism.sh --mode perf  # Same as above
#   sudo ./harden_determinism.sh --mode determinism  # Turbo OFF, max reproducibility
#
# This script implements the strategies from ARCHITECTURE_BOOK.md:
#   1. CPU governor + Turbo Boost control
#   2. Alder Lake P-core / E-core topology detection
#   3. E-core parking (prevents scheduler migration to slow cores)
#   4. HugePages allocation
#   5. Kernel parameter tuning (scheduler, memory, network)
#   6. Transparent HugePages disabled
#   7. NMI watchdog disabled

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

# Parse mode flag
MODE="perf"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --mode) MODE="$2"; shift 2 ;;
        --perf) MODE="perf"; shift ;;
        --determinism) MODE="determinism"; shift ;;
        --help)
            echo "Usage: sudo $0 [--mode perf|determinism]"
            echo "  perf         Turbo ON, E-cores parked, max throughput (default)"
            echo "  determinism  Turbo OFF, stable clocks, max reproducibility"
            exit 0
            ;;
        *) shift ;;
    esac
done

if [[ $EUID -ne 0 ]]; then
    fail "Must run as root: sudo $0"
    exit 1
fi

echo -e "${CYAN}═══════════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  IICPC System Tuning (mode: ${MODE})${NC}"
echo -e "${CYAN}═══════════════════════════════════════════════════════${NC}"

# 1. CPU Governor → Performance
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

# 2. Turbo Boost — mode-dependent
if [[ "$MODE" == "determinism" ]]; then
    log "Disabling Turbo Boost (determinism mode — stable clocks)..."
    if [[ -f /sys/devices/system/cpu/intel_pstate/no_turbo ]]; then
        echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo
        ok "Intel Turbo Boost: DISABLED (deterministic, base clock only)"
    elif [[ -f /sys/devices/system/cpu/cpufreq/boost ]]; then
        echo 0 > /sys/devices/system/cpu/cpufreq/boost
        ok "AMD Boost: DISABLED"
    else
        warn "Turbo Boost control not found"
    fi
else
    log "Enabling Turbo Boost (perf mode — maximum frequency)..."
    if [[ -f /sys/devices/system/cpu/intel_pstate/no_turbo ]]; then
        echo 0 > /sys/devices/system/cpu/intel_pstate/no_turbo
        # Read the actual max freq to report
        MAX_MHZ=$(cat /sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq 2>/dev/null || echo 0)
        MAX_GHZ=$(echo "scale=1; $MAX_MHZ / 1000000" | bc 2>/dev/null || echo "?")
        ok "Intel Turbo Boost: ENABLED (P-cores up to ${MAX_GHZ} GHz)"
    elif [[ -f /sys/devices/system/cpu/cpufreq/boost ]]; then
        echo 1 > /sys/devices/system/cpu/cpufreq/boost
        ok "AMD Boost: ENABLED"
    else
        warn "Turbo Boost control not found"
    fi
fi

# 2b. Alder Lake Hybrid CPU: Detect and park E-cores
log "Detecting CPU topology (P-core / E-core)..."
P_CORES=""
E_CORES=""
HYBRID_DETECTED=false

# On Alder Lake i7-12700H: P-cores have hyperthreading (2 threads/core),
# E-cores are single-threaded with higher core_ids (>=24).
# We detect by checking if a CPU has a sibling (hyperthread) — E-cores don't.
if grep -q "core id" /proc/cpuinfo 2>/dev/null; then
    TOTAL_CPUS=$(nproc)
    for cpu_num in $(seq 0 $((TOTAL_CPUS - 1))); do
        CORE_ID=$(awk "/^processor\t: ${cpu_num}$/,/^$/{if(/^core id/) print \$4}" /proc/cpuinfo 2>/dev/null)
        if [[ -n "$CORE_ID" && "$CORE_ID" -ge 24 ]]; then
            E_CORES="$E_CORES $cpu_num"
            HYBRID_DETECTED=true
        else
            P_CORES="$P_CORES $cpu_num"
        fi
    done
fi

if $HYBRID_DETECTED; then
    P_CORES_TRIMMED=$(echo $P_CORES | xargs)
    E_CORES_TRIMMED=$(echo $E_CORES | xargs)
    P_COUNT=$(echo $P_CORES_TRIMMED | wc -w)
    E_COUNT=$(echo $E_CORES_TRIMMED | wc -w)
    ok "Hybrid CPU detected: ${P_COUNT} P-core threads [${P_CORES_TRIMMED}], ${E_COUNT} E-core threads [${E_CORES_TRIMMED}]"

    # Park E-cores: set them offline so the scheduler cannot use them
    log "Parking E-cores (prevents migration to slow efficiency cores)..."
    PARKED=0
    for ecpu in $E_CORES_TRIMMED; do
        if [[ -f "/sys/devices/system/cpu/cpu${ecpu}/online" ]]; then
            echo 0 > "/sys/devices/system/cpu/cpu${ecpu}/online" 2>/dev/null && PARKED=$((PARKED+1))
        fi
    done
    if [[ $PARKED -gt 0 ]]; then
        ok "E-cores parked: $PARKED cores taken offline (all threads on P-cores now)"
    else
        warn "Could not park E-cores (cpu0 cannot be offlined, or already parked)"
    fi

    # Export for other scripts
    export IICPC_P_CORES="$P_CORES_TRIMMED"
    export IICPC_E_CORES="$E_CORES_TRIMMED"
else
    ok "Homogeneous CPU (no P/E-core split detected)"
fi

# 3. HugePages
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

# 4. Disable Transparent HugePages (causes unpredictable compaction stalls)
log "Disabling Transparent HugePages..."
if [[ -f /sys/kernel/mm/transparent_hugepage/enabled ]]; then
    echo never > /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null || true
    echo never > /sys/kernel/mm/transparent_hugepage/defrag 2>/dev/null || true
    ok "THP: disabled (prevents compaction jitter)"
fi

# 5. Kernel Network Tuning
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

# 6. Memory Tuning
log "Tuning memory parameters..."
sysctl -qw vm.swappiness=0 2>/dev/null || true
sysctl -qw vm.dirty_ratio=10 2>/dev/null || true
sysctl -qw vm.dirty_background_ratio=5 2>/dev/null || true
sysctl -qw vm.zone_reclaim_mode=0 2>/dev/null || true
sysctl -qw vm.overcommit_memory=1 2>/dev/null || true
ok "Memory: swappiness=0, dirty_ratio=10"

# 7. Scheduler Tuning
log "Tuning scheduler..."
sysctl -qw kernel.sched_min_granularity_ns=10000000 2>/dev/null || true
sysctl -qw kernel.sched_wakeup_granularity_ns=15000000 2>/dev/null || true
sysctl -qw kernel.sched_migration_cost_ns=5000000 2>/dev/null || true
sysctl -qw kernel.sched_autogroup_enabled=0 2>/dev/null || true
ok "Scheduler: reduced preemption, migration cost 5ms"

# 8. ASLR Control
log "Configuring ASLR..."
# Keep ASLR enabled for security but allow contestants to disable for their process
sysctl -qw kernel.randomize_va_space=2 2>/dev/null || true
ok "ASLR: enabled (full randomization)"

# 9. Watchdog Timer
log "Disabling NMI watchdog (reduces interrupts)..."
sysctl -qw kernel.nmi_watchdog=0 2>/dev/null || true
ok "NMI watchdog: disabled"

# 10. File Descriptor Limits
log "Setting file descriptor limits..."
ulimit -n 1048576 2>/dev/null || true
sysctl -qw fs.file-max=2097152 2>/dev/null || true
sysctl -qw fs.nr_open=2097152 2>/dev/null || true
ok "FD limit: 2M"

# 11. cgroups v2 Verification
log "Verifying cgroups v2..."
if mount | grep -q "cgroup2"; then
    ok "cgroups v2: available"
    # Ensure subtree delegation
    CGROUP_ROOT="/sys/fs/cgroup"
    echo "+cpuset +cpu +io +memory +pids" > "$CGROUP_ROOT/cgroup.subtree_control" 2>/dev/null || true
else
    warn "cgroups v2: not mounted (sandbox isolation limited)"
fi

# Summary
TURBO_STATE="unknown"
if [[ -f /sys/devices/system/cpu/intel_pstate/no_turbo ]]; then
    NO_TURBO=$(cat /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null || echo "?")
    [[ "$NO_TURBO" == "0" ]] && TURBO_STATE="ON" || TURBO_STATE="OFF"
elif [[ -f /sys/devices/system/cpu/cpufreq/boost ]]; then
    BOOST=$(cat /sys/devices/system/cpu/cpufreq/boost 2>/dev/null || echo "?")
    [[ "$BOOST" == "1" ]] && TURBO_STATE="ON" || TURBO_STATE="OFF"
fi

ONLINE_CPUS=$(nproc)

echo ""
echo -e "${CYAN}═══════════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  Tuning Complete (mode: ${MODE})${NC}"
echo -e "${CYAN}═══════════════════════════════════════════════════════${NC}"

echo -e "  Mode:          $MODE"
echo -e "  CPU Governor:  $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo 'N/A')"
echo -e "  Turbo Boost:   $TURBO_STATE"
echo -e "  Online CPUs:   $ONLINE_CPUS"
if $HYBRID_DETECTED 2>/dev/null; then
    echo -e "  P-cores:       ${P_CORES_TRIMMED}"
    echo -e "  E-cores:       parked (offline)"
fi
echo -e "  HugePages:     $(cat /proc/sys/vm/nr_hugepages 2>/dev/null || echo 0) × 2MB"
echo -e "  THP:           $(cat /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null || echo 'N/A')"
echo -e "  Swappiness:    $(cat /proc/sys/vm/swappiness 2>/dev/null || echo 'N/A')"
echo -e "  NMI Watchdog:  $(cat /proc/sys/kernel/nmi_watchdog 2>/dev/null || echo 'N/A')"
echo -e "  cgroups v2:    $(mount | grep -q cgroup2 && echo 'active' || echo 'inactive')"

if [[ "$TURBO_STATE" == "ON" ]]; then
    echo ""
    echo -e "  ${GREEN}Tip:${NC} Run benchmarks pinned to P-cores for maximum throughput:"
    if $HYBRID_DETECTED 2>/dev/null; then
        # Build a taskset-friendly range from P_CORES
        P_FIRST=$(echo $P_CORES_TRIMMED | awk '{print $1}')
        P_LAST=$(echo $P_CORES_TRIMMED | awk '{print $NF}')
        echo -e "    taskset -c ${P_FIRST}-${P_LAST} ./build/bench_shm"
        echo -e "    taskset -c ${P_FIRST}-${P_LAST} ./build/pipeline_e2e"
    else
        echo -e "    ./build/bench_shm"
    fi
fi
echo ""
