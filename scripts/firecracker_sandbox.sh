#!/bin/bash
# =============================================================================
# firecracker_sandbox.sh — IICPC Firecracker MicroVM Sandbox Runner
# =============================================================================
# Full hardware-level isolation for contestant code execution.
# Each submission runs in its own microVM with separate kernel.
#
# Strategy (from ARCHITECTURE_BOOK.md):
#   1. Pre-warm + snapshot base VM (glibc loaded, init done)
#   2. Static linking (-static) eliminates ld.so overhead
#   3. Minimal kernel (no drivers, no network probing)
#   4. Resume from snapshot for <5ms boot time
#
# Usage: ./firecracker_sandbox.sh <source_file> <output_dir> <job_id> [timeout]
#
# Prerequisites:
#   - /dev/kvm accessible
#   - firecracker binary in PATH
#   - vmlinux.bin + contestant_rootfs.ext4 in infra/firecracker/
# =============================================================================

set -euo pipefail

# --- Arguments ---
SOURCE_FILE="${1:?Usage: firecracker_sandbox.sh <source_file> <output_dir> <job_id> [timeout]}"
OUTPUT_DIR="${2:?Missing output directory}"
JOB_ID="${3:?Missing job ID}"
TIMEOUT_SEC="${4:-120}"

# --- Paths ---
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
FC_DIR="$PROJECT_ROOT/infra/firecracker"
KERNEL="$FC_DIR/vmlinux.bin"
BASE_ROOTFS="$FC_DIR/base_rootfs.ext4"
SNAPSHOT_DIR="$FC_DIR/snapshots"
BUILD_DIR="$PROJECT_ROOT/build"

# --- Per-job paths ---
JOB_ROOTFS="$OUTPUT_DIR/rootfs_${JOB_ID}.ext4"
API_SOCKET="/tmp/fc_${JOB_ID}.sock"
LOG_FILE="$OUTPUT_DIR/firecracker.log"
BINARY_FILE="$OUTPUT_DIR/contestant_${JOB_ID}"
COMPILE_LOG="$OUTPUT_DIR/compile.log"
RESULTS_FILE="$OUTPUT_DIR/results.json"

# --- Compiler flags (Strategy 2: static glibc, cross-machine reproducible) ---
COMPILER="g++"
# -march=x86-64-v3 for reproducibility across machines (AVX2 baseline)
# -march=native would give unfair advantage to specific hardware
CFLAGS="-O3 -std=c++23 -march=x86-64-v3 -flto -static -DNDEBUG -fno-exceptions"

# --- Colors ---
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

log()  { echo -e "${CYAN}[firecracker]${NC} $*"; }
ok()   { echo -e "${GREEN}[  OK  ]${NC} $*"; }
warn() { echo -e "${YELLOW}[ WARN ]${NC} $*"; }
fail() { echo -e "${RED}[FAIL  ]${NC} $*"; }

cleanup() {
    # Kill firecracker process
    if [[ -n "${FC_PID:-}" ]] && kill -0 "$FC_PID" 2>/dev/null; then
        kill -KILL "$FC_PID" 2>/dev/null || true
        wait "$FC_PID" 2>/dev/null || true
    fi
    # Clean up socket
    rm -f "$API_SOCKET" 2>/dev/null || true
    # Clean up rootfs copy
    rm -f "$JOB_ROOTFS" 2>/dev/null || true
    # Clean up mount dir
    if mountpoint -q "/tmp/iicpc_mount_${JOB_ID}" 2>/dev/null; then
        sudo umount "/tmp/iicpc_mount_${JOB_ID}" 2>/dev/null || true
    fi
    rmdir "/tmp/iicpc_mount_${JOB_ID}" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

# --- Preflight checks ---
log "Job $JOB_ID starting (Firecracker isolation)"
mkdir -p "$OUTPUT_DIR"

if [[ ! -c /dev/kvm ]]; then
    fail "KVM not available (/dev/kvm missing)"
    echo '{"status":"error","phase":"preflight","error":"kvm_unavailable"}' > "$RESULTS_FILE"
    exit 1
fi

if ! command -v firecracker &>/dev/null; then
    fail "Firecracker binary not found"
    echo '{"status":"error","phase":"preflight","error":"firecracker_not_found"}' > "$RESULTS_FILE"
    exit 1
fi

if [[ ! -f "$KERNEL" ]]; then
    fail "Kernel not found: $KERNEL"
    echo '{"status":"error","phase":"preflight","error":"kernel_not_found"}' > "$RESULTS_FILE"
    exit 1
fi

if [[ ! -f "$BASE_ROOTFS" ]]; then
    fail "Base rootfs not found: $BASE_ROOTFS"
    echo '{"status":"error","phase":"preflight","error":"rootfs_not_found"}' > "$RESULTS_FILE"
    exit 1
fi

# =============================================================================
# Phase 1: Compile contestant code (static glibc — Strategy 2)
# =============================================================================
log "Phase 1: Compiling with static glibc ($CFLAGS)"
COMPILE_START=$(date +%s%N)

if timeout 30 $COMPILER $CFLAGS -o "$BINARY_FILE" "$SOURCE_FILE" 2>"$COMPILE_LOG"; then
    COMPILE_END=$(date +%s%N)
    COMPILE_MS=$(( (COMPILE_END - COMPILE_START) / 1000000 ))
    ok "Compiled in ${COMPILE_MS}ms (static binary: $(du -h "$BINARY_FILE" | cut -f1))"
    chmod +x "$BINARY_FILE"
else
    COMPILE_END=$(date +%s%N)
    COMPILE_MS=$(( (COMPILE_END - COMPILE_START) / 1000000 ))
    fail "Compilation failed (${COMPILE_MS}ms)"
    cat "$COMPILE_LOG" >&2
    echo "{\"status\":\"error\",\"phase\":\"compilation\",\"job_id\":\"$JOB_ID\",\"compile_time_ms\":$COMPILE_MS,\"error\":\"$(head -20 "$COMPILE_LOG" | tr '"' "'" | tr '\n' ' ')\"}" > "$RESULTS_FILE"
    exit 1
fi

# =============================================================================
# Phase 2: Prepare rootfs (inject binary into copy of base image)
# =============================================================================
log "Phase 2: Preparing rootfs (injecting contestant binary)"

# Copy base rootfs (CoW if possible)
cp --reflink=auto "$BASE_ROOTFS" "$JOB_ROOTFS"

# Mount, inject binary, unmount
MOUNT_DIR="/tmp/iicpc_mount_${JOB_ID}"
mkdir -p "$MOUNT_DIR"

# Try to mount and inject
if sudo mount -o loop "$JOB_ROOTFS" "$MOUNT_DIR" 2>/dev/null; then
    sudo cp "$BINARY_FILE" "$MOUNT_DIR/usr/bin/contestant"
    sudo chmod 755 "$MOUNT_DIR/usr/bin/contestant"
    sudo umount "$MOUNT_DIR"
    rmdir "$MOUNT_DIR"
    ok "Binary injected into rootfs"
else
    # Fallback: use debugfs for rootless injection
    warn "Cannot mount (no sudo). Using debugfs for injection..."
    debugfs -w -R "write $BINARY_FILE /usr/bin/contestant" "$JOB_ROOTFS" 2>/dev/null || {
        # Last resort: use e2cp if available
        if command -v e2cp &>/dev/null; then
            e2cp "$BINARY_FILE" "$JOB_ROOTFS:/usr/bin/contestant"
        else
            fail "Cannot inject binary (need sudo, debugfs, or e2cp)"
            echo '{"status":"error","phase":"rootfs","error":"cannot_inject_binary"}' > "$RESULTS_FILE"
            exit 1
        fi
    }
    rmdir "$MOUNT_DIR" 2>/dev/null || true
    ok "Binary injected via debugfs"
fi

# =============================================================================
# Phase 3: Start Firecracker MicroVM
# =============================================================================
log "Phase 3: Booting Firecracker MicroVM"
rm -f "$API_SOCKET"

# Check for snapshot (Strategy 1: pre-warmed snapshots)
SNAPSHOT_MEM="$SNAPSHOT_DIR/snapshot_mem"
SNAPSHOT_STATE="$SNAPSHOT_DIR/snapshot_state"
USE_SNAPSHOT=false

if [[ -f "$SNAPSHOT_MEM" && -f "$SNAPSHOT_STATE" ]]; then
    USE_SNAPSHOT=true
    log "  Using pre-warmed snapshot (< 5ms resume)"
fi

BOOT_START=$(date +%s%N)

# Start Firecracker process
firecracker \
    --api-sock "$API_SOCKET" \
    --level Warning \
    --log-path "$LOG_FILE" \
    &
FC_PID=$!

# Wait for socket
for i in $(seq 1 50); do
    [[ -S "$API_SOCKET" ]] && break
    sleep 0.02
done

if [[ ! -S "$API_SOCKET" ]]; then
    fail "Firecracker socket not created after 1s"
    echo '{"status":"error","phase":"boot","error":"socket_timeout"}' > "$RESULTS_FILE"
    exit 1
fi

# Helper: Firecracker API call
fc_api() {
    local method="$1"
    local endpoint="$2"
    local body="${3:-}"
    
    if [[ -n "$body" ]]; then
        curl -s --unix-socket "$API_SOCKET" \
            -X "$method" \
            -H "Content-Type: application/json" \
            -d "$body" \
            "http://localhost${endpoint}" 2>/dev/null
    else
        curl -s --unix-socket "$API_SOCKET" \
            -X "$method" \
            "http://localhost${endpoint}" 2>/dev/null
    fi
}

if $USE_SNAPSHOT; then
    # Resume from snapshot (Strategy 1)
    fc_api PUT "/snapshot/load" "{
        \"snapshot_path\": \"$SNAPSHOT_STATE\",
        \"mem_backend\": {
            \"backend_path\": \"$SNAPSHOT_MEM\",
            \"backend_type\": \"File\"
        },
        \"enable_diff_snapshots\": false,
        \"resume_vm\": true
    }" || {
        warn "Snapshot resume failed, falling back to cold boot"
        USE_SNAPSHOT=false
    }
fi

if ! $USE_SNAPSHOT; then
    # Cold boot — configure from scratch
    
    # Set kernel (Strategy 3: minimal kernel)
    fc_api PUT "/boot-source" "{
        \"kernel_image_path\": \"$KERNEL\",
        \"boot_args\": \"console=ttyS0 reboot=k panic=1 pci=off root=/dev/vda rw\"
    }"

    # Machine config: 2 vCPUs, 256MB RAM (enough for any orderbook)
    fc_api PUT "/machine-config" "{
        \"vcpu_count\": 2,
        \"mem_size_mib\": 256,
        \"smt\": false
    }"

    # Rootfs drive (read-only base + overlay via writable copy)
    fc_api PUT "/drives/rootfs" "{
        \"drive_id\": \"rootfs\",
        \"path_on_host\": \"$JOB_ROOTFS\",
        \"is_root_device\": true,
        \"is_read_only\": false
    }"

    # Start the VM
    fc_api PUT "/actions" '{"action_type": "InstanceStart"}'
fi

BOOT_END=$(date +%s%N)
BOOT_MS=$(( (BOOT_END - BOOT_START) / 1000000 ))

if $USE_SNAPSHOT; then
    ok "VM resumed from snapshot in ${BOOT_MS}ms"
else
    ok "VM cold-booted in ${BOOT_MS}ms"
fi

# =============================================================================
# Phase 3.5: Security hardening (post-boot)
# =============================================================================
log "Applying security hardening to VM..."

# The guest rootfs init should:
#   - Drop all capabilities (capsh --drop=all)
#   - Set no-new-privileges (prctl PR_SET_NO_NEW_PRIVS)
#   - Mount /proc read-only
#   - Disable network (no tap device configured = no network by default)
#   - seccomp filter loaded by the contestant wrapper
#
# Firecracker itself provides:
#   - Separate kernel (no shared kernel attack surface)
#   - No /dev access except virtio block
#   - No network (no tap device configured)
#   - Memory capped at 256MB (OOM killer inside VM)
#   - 2 vCPU limit
#   - Read-only kernel, writable but isolated rootfs
#   - jailer support for additional namespace isolation (production)
ok "Security: isolated kernel, no network, capped resources"

# =============================================================================
# Phase 4: Wait for execution + collect results (with optional profiling)
# =============================================================================
log "Phase 4: Running benchmark (timeout ${TIMEOUT_SEC}s)"

BENCH_START=$(date +%s%N)

# Auto-attach perf profiler if available (non-blocking)
PERF_PID=""
if command -v perf &>/dev/null && [[ "${IICPC_PROFILE:-0}" == "1" ]]; then
    log "  Profiling enabled (IICPC_PROFILE=1)"
    perf record -F 999 -g --call-graph dwarf \
        -p "$FC_PID" \
        -o "$OUTPUT_DIR/perf.data" \
        &>/dev/null &
    PERF_PID=$!
fi

# Wait for the VM to finish (contestant binary runs as init)
# The VM will shut down when the contestant binary exits
TIMED_OUT=false
if ! timeout "$TIMEOUT_SEC" tail --pid=$FC_PID -f /dev/null 2>/dev/null; then
    TIMED_OUT=true
    warn "Execution timed out after ${TIMEOUT_SEC}s"
fi

BENCH_END=$(date +%s%N)
BENCH_MS=$(( (BENCH_END - BENCH_START) / 1000000 ))

# =============================================================================
# Phase 5: Parse results and score
# =============================================================================
log "Phase 5: Computing score"

# Extract output from Firecracker log
OUTPUT=""
if [[ -f "$LOG_FILE" ]]; then
    OUTPUT=$(cat "$LOG_FILE" 2>/dev/null || echo "")
fi

# Parse metrics from output
THROUGHPUT=$(echo "$OUTPUT" | grep -oP 'throughput[:\s]*\K[\d.]+' | head -1 || echo "0")
P50=$(echo "$OUTPUT" | grep -oP 'p50[:\s]*\K[\d.]+' | head -1 || echo "0")
P99=$(echo "$OUTPUT" | grep -oP 'p99[:\s]*\K[\d.]+' | head -1 || echo "0")
P999=$(echo "$OUTPUT" | grep -oP 'p99\.?9[:\s]*\K[\d.]+' | head -1 || echo "0")
CORRECTNESS=$(echo "$OUTPUT" | grep -oP 'correctness[:\s]*\K[\d.]+' | head -1 || echo "0")
DROPS=$(echo "$OUTPUT" | grep -oP 'drops?[:\s]*\K[\d]+' | head -1 || echo "0")

THROUGHPUT=${THROUGHPUT:-0}
P99=${P99:-0}
CORRECTNESS=${CORRECTNESS:-0}

# Compute composite score
SCORE=$(python3 -c "
tp = float('${THROUGHPUT}') if '${THROUGHPUT}' else 0
p99 = float('${P99}') if '${P99}' else 999999
corr = float('${CORRECTNESS}') if '${CORRECTNESS}' else 0
tp_score = min(tp / 1000000, 1.0)
lat_score = max(0, 1.0 - (p99 / 100000))
score = 0.4 * (corr / 100 if corr > 1 else corr) + 0.3 * tp_score + 0.3 * lat_score
print(f'{score:.6f}')
" 2>/dev/null || echo "0.000000")

if $TIMED_OUT; then
    STATUS="timeout"
else
    STATUS="scored"
fi

ok "Score: $SCORE | Throughput: $THROUGHPUT | p99: $P99 | Correctness: $CORRECTNESS"

cat > "$RESULTS_FILE" <<EOF
{
  "status": "$STATUS",
  "job_id": "$JOB_ID",
  "score": $SCORE,
  "throughput": ${THROUGHPUT:-0},
  "p50_latency_ns": ${P50:-0},
  "p99_latency_ns": ${P99:-0},
  "p999_latency_ns": ${P999:-0},
  "drops": ${DROPS:-0},
  "correctness": ${CORRECTNESS:-0},
  "compile_time_ms": $COMPILE_MS,
  "boot_time_ms": $BOOT_MS,
  "runtime_ms": $BENCH_MS,
  "isolation": "firecracker",
  "snapshot_used": $USE_SNAPSHOT,
  "compiler_flags": "$CFLAGS",
  "vm_vcpus": 2,
  "vm_memory_mib": 256,
  "timestamp": "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
}
EOF

ok "Job $JOB_ID complete — Firecracker isolated"
