#!/bin/bash
# =============================================================================
# validate_determinism.sh — Snapshot Restore A/B Determinism Test
# =============================================================================
# Runs the same contestant binary N times from the same snapshot and compares
# scores/throughput/latency to prove the VM state is clean and repeatable.
#
# Usage: ./scripts/validate_determinism.sh <source.cpp> [--runs 5]
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SANDBOX="$SCRIPT_DIR/firecracker_sandbox.sh"

CYAN='\033[0;36m'
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

log()  { echo -e "${CYAN}[determinism]${NC} $*"; }
ok()   { echo -e "${GREEN}[  PASS  ]${NC} $*"; }
fail() { echo -e "${RED}[  FAIL  ]${NC} $*"; }
warn() { echo -e "${YELLOW}[  WARN  ]${NC} $*"; }

# Parse args
SOURCE=""
NUM_RUNS=5
while [[ $# -gt 0 ]]; do
    case "$1" in
        --runs) NUM_RUNS="$2"; shift 2 ;;
        *) SOURCE="$1"; shift ;;
    esac
done

if [[ -z "$SOURCE" || ! -f "$SOURCE" ]]; then
    echo "Usage: $0 <source.cpp> [--runs N]"
    exit 1
fi

if [[ ! -f "$SANDBOX" ]]; then
    fail "Sandbox script not found: $SANDBOX"
    exit 1
fi

echo ""
echo -e "${CYAN}═══════════════════════════════════════════════════════════${NC}"
echo -e "${CYAN}           DETERMINISM VALIDATION (A/B Test)              ${NC}"
echo -e "${CYAN}═══════════════════════════════════════════════════════════${NC}"
echo -e "  Source:  $SOURCE"
echo -e "  Runs:    $NUM_RUNS"
echo ""

RESULTS_DIR="$PROJECT_ROOT/build/determinism_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$RESULTS_DIR"

declare -a SCORES=()
declare -a THROUGHPUTS=()
declare -a P99S=()

for i in $(seq 1 "$NUM_RUNS"); do
    RUN_DIR="$RESULTS_DIR/run_$i"
    JOB_ID="det_$(printf '%03d' $i)"
    
    log "Run $i/$NUM_RUNS (job=$JOB_ID)..."
    
    if bash "$SANDBOX" "$SOURCE" "$RUN_DIR" "$JOB_ID" 60 >/dev/null 2>&1; then
        RESULTS_FILE="$RUN_DIR/results.json"
        if [[ -f "$RESULTS_FILE" ]]; then
            SCORE=$(python3 -c "import json; d=json.load(open('$RESULTS_FILE')); print(d.get('score', 0))" 2>/dev/null || echo "0")
            TP=$(python3 -c "import json; d=json.load(open('$RESULTS_FILE')); print(d.get('throughput', 0))" 2>/dev/null || echo "0")
            P99=$(python3 -c "import json; d=json.load(open('$RESULTS_FILE')); print(d.get('p99_latency_ns', 0))" 2>/dev/null || echo "0")
            
            SCORES+=("$SCORE")
            THROUGHPUTS+=("$TP")
            P99S+=("$P99")
            
            log "  Score=$SCORE  Throughput=$TP  p99=$P99"
        else
            warn "  Run $i: no results.json"
            SCORES+=("0")
            THROUGHPUTS+=("0")
            P99S+=("0")
        fi
    else
        warn "  Run $i: sandbox failed"
        SCORES+=("0")
        THROUGHPUTS+=("0")
        P99S+=("0")
    fi
done

# Compute statistics
echo ""
echo -e "${CYAN}═══════════════════════════════════════════════════════════${NC}"
echo -e "${CYAN}                    RESULTS                               ${NC}"
echo -e "${CYAN}═══════════════════════════════════════════════════════════${NC}"

python3 << 'PYEOF'
import sys, statistics

scores = [float(x) for x in """${SCORES[@]}""".split()]
throughputs = [float(x) for x in """${THROUGHPUTS[@]}""".split()]
p99s = [float(x) for x in """${P99S[@]}""".split()]

def report(name, values):
    if not values or all(v == 0 for v in values):
        print(f"  {name}: NO DATA")
        return False
    
    mean = statistics.mean(values)
    if len(values) > 1:
        stdev = statistics.stdev(values)
        cv = (stdev / mean * 100) if mean > 0 else 0
    else:
        stdev = 0
        cv = 0
    
    min_v = min(values)
    max_v = max(values)
    spread = ((max_v - min_v) / mean * 100) if mean > 0 else 0
    
    print(f"  {name}:")
    print(f"    Mean:   {mean:.6f}")
    print(f"    Stdev:  {stdev:.6f}")
    print(f"    CV:     {cv:.2f}%")
    print(f"    Range:  [{min_v:.6f}, {max_v:.6f}]")
    print(f"    Spread: {spread:.2f}%")
    
    # Determinism threshold: CV < 5% is acceptable, < 1% is excellent
    if cv < 1.0:
        print(f"    Status: ✅ EXCELLENT (CV < 1%)")
    elif cv < 5.0:
        print(f"    Status: ✅ ACCEPTABLE (CV < 5%)")
    else:
        print(f"    Status: ❌ NON-DETERMINISTIC (CV >= 5%)")
        return False
    return True

print()
s1 = report("Score", scores)
s2 = report("Throughput", throughputs)
s3 = report("p99 Latency", p99s)

print()
if s1 and s2 and s3:
    print("  🟢 DETERMINISM VALIDATION: PASSED")
elif s1:
    print("  🟡 DETERMINISM VALIDATION: PARTIAL (score deterministic, perf varies)")
else:
    print("  🔴 DETERMINISM VALIDATION: FAILED")
print()
PYEOF

echo "  Results directory: $RESULTS_DIR"
echo ""
