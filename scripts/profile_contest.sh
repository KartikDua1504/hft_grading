#!/bin/bash
# profile_contest.sh — Flamegraph + perf profiling for contest runs
# Generates:
#   1. CPU flamegraph (SVG) for the contest pipeline
#   2. perf stat summary (IPC, cache misses, branch misses)
#   3. Latency distribution histogram (text)
#   4. Memory profiling (RSS peak, hugepages)
#
# Usage: ./scripts/profile_contest.sh <source.cpp|binary> [--duration 30]
# Output: build/profiles/<timestamp>/

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
PROFILE_DIR="$BUILD_DIR/profiles/$TIMESTAMP"
CONTEST_BINARY="$BUILD_DIR/run_contest"

CYAN='\033[0;36m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

log()  { echo -e "${CYAN}[profile]${NC} $*"; }
ok()   { echo -e "${GREEN}[  OK  ]${NC} $*"; }
warn() { echo -e "${YELLOW}[ WARN ]${NC} $*"; }
fail() { echo -e "${RED}[ FAIL ]${NC} $*"; }

# Parse args
SOURCE=""
DURATION=30
EXTRA_ARGS=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --duration) DURATION="$2"; shift 2 ;;
        --no-firecracker) EXTRA_ARGS="$EXTRA_ARGS --no-firecracker"; shift ;;
        --help)
            echo "Usage: $0 <source.cpp|binary> [--duration 30] [--no-firecracker]"
            exit 0
            ;;
        *) SOURCE="$1"; shift ;;
    esac
done

if [[ -z "$SOURCE" ]]; then
    fail "No source file or binary specified"
    echo "Usage: $0 <source.cpp|binary> [--duration SECS]"
    exit 1
fi

if [[ ! -f "$CONTEST_BINARY" ]]; then
    fail "Contest binary not found: $CONTEST_BINARY"
    echo "Run: cd build && cmake --build . -j\$(nproc)"
    exit 1
fi

mkdir -p "$PROFILE_DIR"

echo ""
echo -e "${CYAN}═══════════════════════════════════════════════════════════════${NC}"
echo -e "${CYAN}                    IICPC PROFILING SUITE                     ${NC}"
echo -e "${CYAN}═══════════════════════════════════════════════════════════════${NC}"
echo -e "  Source:    $SOURCE"
echo -e "  Duration:  ${DURATION}s"
echo -e "  Output:    $PROFILE_DIR"

# Detect Alder Lake P-core / E-core topology
TASKSET_PREFIX=""
if grep -q "core id" /proc/cpuinfo 2>/dev/null; then
    P_CORES=""
    TOTAL_CPUS=$(nproc)
    for cpu_num in $(seq 0 $((TOTAL_CPUS - 1))); do
        CORE_ID=$(awk "/^processor\t: ${cpu_num}$/,/^$/{if(/^core id/) print \$4}" /proc/cpuinfo 2>/dev/null)
        if [[ -n "$CORE_ID" && "$CORE_ID" -lt 24 ]]; then
            P_CORES="$P_CORES $cpu_num"
        fi
    done
    P_CORES_TRIMMED=$(echo $P_CORES | xargs)
    # If we found both P-cores and have more CPUs than P-cores, it's hybrid
    P_COUNT=$(echo $P_CORES_TRIMMED | wc -w)
    if [[ $P_COUNT -lt $TOTAL_CPUS && $P_COUNT -gt 0 ]]; then
        P_FIRST=$(echo $P_CORES_TRIMMED | awk '{print $1}')
        P_LAST=$(echo $P_CORES_TRIMMED | awk '{print $NF}')
        TASKSET_PREFIX="taskset -c ${P_FIRST}-${P_LAST}"
        echo -e "  Topology:  Hybrid CPU detected — pinning to P-cores [${P_FIRST}-${P_LAST}]"
    fi
fi
echo ""

# 1. perf stat — Hardware counters
log "Phase 1: Hardware performance counters (perf stat)..."

PERF_AVAILABLE=false
if command -v perf &>/dev/null; then
    PERF_AVAILABLE=true
fi

if $PERF_AVAILABLE; then
    if [[ -x "$SOURCE" ]]; then
        BINARY_ARG="--binary $SOURCE"
    else
        BINARY_ARG="--source $SOURCE"
    fi

    perf stat -d -d -d \
        -o "$PROFILE_DIR/perf_stat.txt" \
        -- $TASKSET_PREFIX "$CONTEST_BINARY" $BINARY_ARG \
        --duration "$DURATION" \
        --no-firecracker \
        $EXTRA_ARGS \
        2>"$PROFILE_DIR/contest_stderr.txt" || true

    if [[ -f "$PROFILE_DIR/perf_stat.txt" ]]; then
        ok "perf stat → $PROFILE_DIR/perf_stat.txt"

        # Extract key metrics
        echo "" >> "$PROFILE_DIR/perf_stat.txt"
        echo "=== KEY METRICS ===" >> "$PROFILE_DIR/perf_stat.txt"
        IPC=$(grep "insn per cycle" "$PROFILE_DIR/perf_stat.txt" | awk '{print $4}' || echo "N/A")
        L1_MISS=$(grep "L1-dcache-load-misses" "$PROFILE_DIR/perf_stat.txt" | awk '{print $1}' || echo "N/A")
        BRANCH_MISS=$(grep "branch-misses" "$PROFILE_DIR/perf_stat.txt" | awk '{print $4}' || echo "N/A")

        log "  IPC:           $IPC"
        log "  L1 cache miss: $L1_MISS"
        log "  Branch miss:   $BRANCH_MISS"
    else
        warn "perf stat output not generated"
    fi
else
    warn "perf not available — skipping hardware counters"
    # Run contest directly for timing
    if [[ -x "$SOURCE" ]]; then
        $TASKSET_PREFIX "$CONTEST_BINARY" --binary "$SOURCE" --duration "$DURATION" --no-firecracker $EXTRA_ARGS \
            2>"$PROFILE_DIR/contest_stderr.txt" || true
    else
        $TASKSET_PREFIX "$CONTEST_BINARY" --source "$SOURCE" --duration "$DURATION" --no-firecracker $EXTRA_ARGS \
            2>"$PROFILE_DIR/contest_stderr.txt" || true
    fi
fi

# 2. Flamegraph — CPU sampling profile
log "Phase 2: CPU flamegraph generation..."

FLAMEGRAPH_DIR=""
if [[ -d "/usr/share/FlameGraph" ]]; then
    FLAMEGRAPH_DIR="/usr/share/FlameGraph"
elif [[ -d "$HOME/FlameGraph" ]]; then
    FLAMEGRAPH_DIR="$HOME/FlameGraph"
elif [[ -d "$PROJECT_ROOT/tools/FlameGraph" ]]; then
    FLAMEGRAPH_DIR="$PROJECT_ROOT/tools/FlameGraph"
fi

if $PERF_AVAILABLE; then
    log "Recording CPU samples (${DURATION}s)..."

    if [[ -x "$SOURCE" ]]; then
        BINARY_ARG="--binary $SOURCE"
    else
        BINARY_ARG="--source $SOURCE"
    fi

    # Record at 999Hz for duration
    perf record -F 999 -g --call-graph dwarf \
        -o "$PROFILE_DIR/perf.data" \
        -- $TASKSET_PREFIX "$CONTEST_BINARY" $BINARY_ARG \
        --duration "$DURATION" \
        --no-firecracker \
        $EXTRA_ARGS \
        2>/dev/null || true

    if [[ -f "$PROFILE_DIR/perf.data" ]]; then
        # Generate folded stacks
        perf script -i "$PROFILE_DIR/perf.data" > "$PROFILE_DIR/perf_script.txt" 2>/dev/null || true

        if [[ -n "$FLAMEGRAPH_DIR" && -f "$FLAMEGRAPH_DIR/stackcollapse-perf.pl" ]]; then
            "$FLAMEGRAPH_DIR/stackcollapse-perf.pl" "$PROFILE_DIR/perf_script.txt" \
                > "$PROFILE_DIR/folded.txt" 2>/dev/null
            "$FLAMEGRAPH_DIR/flamegraph.pl" \
                --title "IICPC Contest Pipeline — CPU Flamegraph" \
                --subtitle "$(basename "$SOURCE") — ${DURATION}s" \
                --width 1200 \
                "$PROFILE_DIR/folded.txt" \
                > "$PROFILE_DIR/flamegraph.svg" 2>/dev/null

            if [[ -f "$PROFILE_DIR/flamegraph.svg" ]]; then
                ok "Flamegraph → $PROFILE_DIR/flamegraph.svg"
            fi
        else
            warn "FlameGraph tools not found. Install:"
            warn "  git clone https://github.com/brendangregg/FlameGraph ~/FlameGraph"
            warn "  Raw perf data saved → $PROFILE_DIR/perf.data"

            # Generate text-based hot functions instead
            perf report -i "$PROFILE_DIR/perf.data" --stdio \
                --no-children --percent-limit 1.0 \
                > "$PROFILE_DIR/perf_report.txt" 2>/dev/null || true
            if [[ -f "$PROFILE_DIR/perf_report.txt" ]]; then
                ok "Hot functions → $PROFILE_DIR/perf_report.txt"
            fi
        fi
    else
        warn "perf record failed (may need CAP_SYS_ADMIN or kernel.perf_event_paranoid=-1)"
    fi
else
    warn "perf not available — skipping flamegraph"
fi

# 3. Memory profiling
log "Phase 3: Memory profiling..."

# Capture HugePages info
{
    echo "=== HUGEPAGE STATUS ==="
    cat /proc/meminfo | grep -i huge
    echo ""
    echo "=== PROCESS MEMORY (from contest run) ==="
    grep -E "(VmPeak|VmRSS|VmHWM|HugetlbPages)" "$PROFILE_DIR/contest_stderr.txt" 2>/dev/null || echo "N/A"
} > "$PROFILE_DIR/memory_profile.txt"

# Check if hugepages are being used
HUGE_USED=$(grep "HugePages_Total" /proc/meminfo | awk '{print $2}' 2>/dev/null || echo "0")
if [[ "$HUGE_USED" -gt 0 ]]; then
    ok "HugePages: ${HUGE_USED} pages allocated"
else
    warn "HugePages: 0 allocated (run harden_determinism.sh for production)"
fi

# 4. Latency distribution analysis
log "Phase 4: Latency analysis from contest output..."

{
    echo "=== LATENCY ANALYSIS ==="
    echo ""
    # Extract HDR histogram data from contest stderr
    grep -E "(p50|p90|p99|p999|max|Throughput|TPS|Drops|Correctness)" \
        "$PROFILE_DIR/contest_stderr.txt" 2>/dev/null || echo "No latency data found in output"
    echo ""
    echo "=== FULL CONTEST OUTPUT ==="
    cat "$PROFILE_DIR/contest_stderr.txt" 2>/dev/null || echo "No output"
} > "$PROFILE_DIR/latency_analysis.txt"

ok "Latency analysis → $PROFILE_DIR/latency_analysis.txt"

# 5. CPU topology + scheduling analysis
log "Phase 5: System topology..."

{
    echo "=== CPU TOPOLOGY ==="
    lscpu 2>/dev/null | head -20
    echo ""
    echo "=== CPU FREQUENCY ==="
    cat /proc/cpuinfo | grep "cpu MHz" | head -4 2>/dev/null
    echo ""
    echo "=== CPU GOVERNOR ==="
    cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo "N/A"
    echo ""
    echo "=== ISOLCPUS ==="
    cat /proc/cmdline 2>/dev/null | tr ' ' '\n' | grep -E "(isolcpus|nohz)" || echo "None configured"
    echo ""
    echo "=== IRQ AFFINITY ==="
    cat /proc/interrupts | head -5 2>/dev/null
} > "$PROFILE_DIR/system_topology.txt"

ok "System topology → $PROFILE_DIR/system_topology.txt"

# Summary
echo ""
echo -e "${CYAN}═══════════════════════════════════════════════════════════════${NC}"
echo -e "${CYAN}                    PROFILING COMPLETE                        ${NC}"
echo -e "${CYAN}═══════════════════════════════════════════════════════════════${NC}"
echo -e "  Output directory: ${GREEN}$PROFILE_DIR${NC}"
echo ""

ls -lhS "$PROFILE_DIR/" | tail -n +2 | while read -r line; do
    echo "  $line"
done

echo ""
echo -e "  ${YELLOW}Key files:${NC}"
[[ -f "$PROFILE_DIR/flamegraph.svg" ]] && echo -e "    🔥 flamegraph.svg — Open in browser"
[[ -f "$PROFILE_DIR/perf_stat.txt" ]] && echo -e "    📊 perf_stat.txt — IPC, cache misses, branches"
[[ -f "$PROFILE_DIR/perf_report.txt" ]] && echo -e "    📈 perf_report.txt — Hot functions"
[[ -f "$PROFILE_DIR/latency_analysis.txt" ]] && echo -e "    ⏱  latency_analysis.txt — p50/p99/p999"
[[ -f "$PROFILE_DIR/memory_profile.txt" ]] && echo -e "    💾 memory_profile.txt — RSS, HugePages"
[[ -f "$PROFILE_DIR/system_topology.txt" ]] && echo -e "    🖥  system_topology.txt — CPU/governor/isolcpus"
echo ""
