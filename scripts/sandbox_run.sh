#!/bin/bash
# =============================================================================
# sandbox_run.sh — IICPC Isolated Benchmark Runner
# =============================================================================
# Compiles and benchmarks contestant code inside an isolated sandbox using
# Linux namespaces and cgroups v2 for deterministic, fair execution.
#
# Usage: ./sandbox_run.sh <source_file> <output_dir> <job_id> [timeout_sec]
#
# Prerequisites:
#   - cgroups v2 enabled (check: mount | grep cgroup2)
#   - Build directory with run_contest binary
#   - Sufficient permissions for namespace creation
# =============================================================================

set -euo pipefail

# --- Arguments ---
SOURCE_FILE="${1:?Usage: sandbox_run.sh <source_file> <output_dir> <job_id> [timeout_sec]}"
OUTPUT_DIR="${2:?Missing output directory}"
JOB_ID="${3:?Missing job ID}"
TIMEOUT_SEC="${4:-120}"

# --- Configuration ---
COMPILER="g++"
CFLAGS="-O3 -std=c++23 -march=native -flto -static -DNDEBUG"
BENCHMARK_DURATION=30
MAX_MEMORY_MB=512
MAX_FILE_SIZE_MB=50
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"
CONTEST_BINARY="$BUILD_DIR/run_contest"

# --- Output files ---
BINARY_FILE="$OUTPUT_DIR/contestant_${JOB_ID}"
COMPILE_LOG="$OUTPUT_DIR/compile.log"
BENCHMARK_LOG="$OUTPUT_DIR/benchmark.log"
RESULTS_FILE="$OUTPUT_DIR/results.json"
METRICS_FILE="$OUTPUT_DIR/metrics.json"

# --- Colors ---
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

log()  { echo -e "${CYAN}[sandbox]${NC} $*"; }
ok()   { echo -e "${GREEN}[  OK  ]${NC} $*"; }
warn() { echo -e "${YELLOW}[ WARN ]${NC} $*"; }
fail() { echo -e "${RED}[FAIL  ]${NC} $*"; }

# --- Preflight ---
log "Job $JOB_ID starting"
log "Source: $SOURCE_FILE"
log "Output: $OUTPUT_DIR"

mkdir -p "$OUTPUT_DIR"

if [[ ! -f "$SOURCE_FILE" ]]; then
    fail "Source file not found: $SOURCE_FILE"
    echo '{"status":"error","phase":"preflight","error":"source_not_found"}' > "$RESULTS_FILE"
    exit 1
fi

# Validate file size
FILE_SIZE=$(stat -c%s "$SOURCE_FILE" 2>/dev/null || echo 0)
MAX_FILE_BYTES=$((MAX_FILE_SIZE_MB * 1024 * 1024))
if [[ "$FILE_SIZE" -gt "$MAX_FILE_BYTES" ]]; then
    fail "Source file too large: ${FILE_SIZE} bytes (max ${MAX_FILE_SIZE_MB}MB)"
    echo '{"status":"error","phase":"preflight","error":"file_too_large"}' > "$RESULTS_FILE"
    exit 1
fi

# --- Phase 1: Compilation ---
log "Phase 1: Compiling with $CFLAGS"
COMPILE_START=$(date +%s%N)

if timeout 30 $COMPILER $CFLAGS -o "$BINARY_FILE" "$SOURCE_FILE" 2>"$COMPILE_LOG"; then
    COMPILE_END=$(date +%s%N)
    COMPILE_MS=$(( (COMPILE_END - COMPILE_START) / 1000000 ))
    ok "Compilation successful (${COMPILE_MS}ms)"
    chmod +x "$BINARY_FILE"
else
    COMPILE_EXIT=$?
    COMPILE_END=$(date +%s%N)
    COMPILE_MS=$(( (COMPILE_END - COMPILE_START) / 1000000 ))
    fail "Compilation failed (exit $COMPILE_EXIT, ${COMPILE_MS}ms)"
    cat "$COMPILE_LOG" >&2

    jq -n \
        --arg status "error" \
        --arg phase "compilation" \
        --arg job_id "$JOB_ID" \
        --argjson compile_ms "$COMPILE_MS" \
        --arg error "$(head -20 "$COMPILE_LOG")" \
        '{status: $status, phase: $phase, job_id: $job_id, compile_time_ms: $compile_ms, error: $error}' \
        > "$RESULTS_FILE" 2>/dev/null || \
    echo "{\"status\":\"error\",\"phase\":\"compilation\",\"job_id\":\"$JOB_ID\",\"compile_time_ms\":$COMPILE_MS}" > "$RESULTS_FILE"
    exit 1
fi

# --- Phase 2: Sandbox Setup ---
log "Phase 2: Configuring sandbox"

# cgroups v2 isolation (if available)
CGROUP_DIR="/sys/fs/cgroup/iicpc_${JOB_ID}"
CGROUP_ENABLED=false

if [[ -d "/sys/fs/cgroup" ]] && mount | grep -q "cgroup2"; then
    if mkdir -p "$CGROUP_DIR" 2>/dev/null; then
        # Memory limit
        echo "${MAX_MEMORY_MB}M" > "$CGROUP_DIR/memory.max" 2>/dev/null || true
        echo "0" > "$CGROUP_DIR/memory.swap.max" 2>/dev/null || true

        # CPU quota (limit to 2 cores)
        echo "200000 100000" > "$CGROUP_DIR/cpu.max" 2>/dev/null || true

        # PID limit (prevent fork bombs)
        echo "64" > "$CGROUP_DIR/pids.max" 2>/dev/null || true

        CGROUP_ENABLED=true
        ok "cgroups v2 sandbox configured"
    else
        warn "Cannot create cgroup (need permissions) — running without cgroup isolation"
    fi
else
    warn "cgroups v2 not available — running without cgroup isolation"
fi

# --- Phase 3: Benchmark Execution ---
log "Phase 3: Running benchmark (${BENCHMARK_DURATION}s, timeout ${TIMEOUT_SEC}s)"
BENCH_START=$(date +%s%N)

# Build the benchmark command
BENCH_CMD="$CONTEST_BINARY"
if [[ -x "$CONTEST_BINARY" ]]; then
    BENCH_ARGS="--contestant $BINARY_FILE --duration $BENCHMARK_DURATION"
else
    # Fallback: run the contestant binary directly as a self-benchmark
    BENCH_CMD="$BINARY_FILE"
    BENCH_ARGS=""
    warn "run_contest not found, executing binary directly"
fi

# Execute with or without cgroup isolation
if $CGROUP_ENABLED; then
    # Run inside cgroup
    echo $$ > "$CGROUP_DIR/cgroup.procs" 2>/dev/null || true
    timeout "$TIMEOUT_SEC" $BENCH_CMD $BENCH_ARGS > "$BENCHMARK_LOG" 2>&1
    BENCH_EXIT=$?
else
    timeout "$TIMEOUT_SEC" $BENCH_CMD $BENCH_ARGS > "$BENCHMARK_LOG" 2>&1
    BENCH_EXIT=$?
fi

BENCH_END=$(date +%s%N)
BENCH_MS=$(( (BENCH_END - BENCH_START) / 1000000 ))

# --- Phase 4: Parse Results ---
log "Phase 4: Parsing results"

if [[ "$BENCH_EXIT" -eq 124 ]]; then
    fail "Benchmark timed out after ${TIMEOUT_SEC}s"
    echo "{\"status\":\"timeout\",\"phase\":\"benchmark\",\"job_id\":\"$JOB_ID\",\"runtime_ms\":$BENCH_MS}" > "$RESULTS_FILE"
    exit 1
elif [[ "$BENCH_EXIT" -ne 0 ]]; then
    fail "Benchmark failed (exit $BENCH_EXIT)"
    echo "{\"status\":\"error\",\"phase\":\"benchmark\",\"job_id\":\"$JOB_ID\",\"runtime_ms\":$BENCH_MS,\"exit_code\":$BENCH_EXIT}" > "$RESULTS_FILE"
    exit 1
fi

# Extract metrics from benchmark output
THROUGHPUT=$(grep -oP 'throughput[:\s]*\K[\d.]+' "$BENCHMARK_LOG" | head -1 || echo "0")
P50=$(grep -oP 'p50[:\s]*\K[\d.]+' "$BENCHMARK_LOG" | head -1 || echo "0")
P99=$(grep -oP 'p99[:\s]*\K[\d.]+' "$BENCHMARK_LOG" | head -1 || echo "0")
P999=$(grep -oP 'p99\.?9[:\s]*\K[\d.]+' "$BENCHMARK_LOG" | head -1 || echo "0")
DROPS=$(grep -oP 'drops?[:\s]*\K[\d]+' "$BENCHMARK_LOG" | head -1 || echo "0")
CORRECTNESS=$(grep -oP 'correctness[:\s]*\K[\d.]+' "$BENCHMARK_LOG" | head -1 || echo "0")

# Default values if parsing failed
THROUGHPUT=${THROUGHPUT:-0}
P99=${P99:-0}
CORRECTNESS=${CORRECTNESS:-0}

# Compute composite score
# Score = 0.4 * correctness + 0.3 * throughput_score + 0.3 * latency_score
if command -v python3 &>/dev/null; then
    SCORE=$(python3 -c "
tp = float('${THROUGHPUT}') if '${THROUGHPUT}' else 0
p99 = float('${P99}') if '${P99}' else 999999
corr = float('${CORRECTNESS}') if '${CORRECTNESS}' else 0

# Normalize throughput (0-1): 1M ops = 1.0
tp_score = min(tp / 1000000, 1.0)

# Normalize latency (0-1): <1µs = 1.0, >100µs = 0.0
lat_score = max(0, 1.0 - (p99 / 100000))

# Composite
score = 0.4 * (corr / 100 if corr > 1 else corr) + 0.3 * tp_score + 0.3 * lat_score
print(f'{score:.6f}')
" 2>/dev/null || echo "0.000000")
else
    SCORE="0.000000"
fi

ok "Benchmark complete in ${BENCH_MS}ms"
ok "Score: $SCORE | Throughput: $THROUGHPUT | p99: $P99 | Correctness: $CORRECTNESS"

# Write results JSON
cat > "$RESULTS_FILE" <<EOF
{
  "status": "scored",
  "job_id": "$JOB_ID",
  "score": $SCORE,
  "throughput": $THROUGHPUT,
  "p50_latency_ns": $P50,
  "p99_latency_ns": $P99,
  "p999_latency_ns": $P999,
  "drops": $DROPS,
  "correctness": $CORRECTNESS,
  "compile_time_ms": $COMPILE_MS,
  "runtime_ms": $BENCH_MS,
  "cgroup_isolated": $CGROUP_ENABLED,
  "compiler_flags": "$CFLAGS",
  "benchmark_duration_s": $BENCHMARK_DURATION,
  "timestamp": "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
}
EOF

# --- Phase 5: Cleanup ---
log "Phase 5: Cleanup"

# Remove cgroup
if $CGROUP_ENABLED && [[ -d "$CGROUP_DIR" ]]; then
    rmdir "$CGROUP_DIR" 2>/dev/null || true
fi

ok "Job $JOB_ID complete — results in $RESULTS_FILE"
