#!/bin/bash
# post_contest_test.sh — CF-Style Post-Contest System Testing
# Orchestrates the full post-contest rejudge flow:
#   1. Freeze leaderboard (set competition state = "system_testing")
#   2. For each team's best submission:
#      a. Locate or recompile the binary
#      b. Run ./run_contest --system-test --binary <path> --scenarios all
#      c. Collect system test results
#   3. Recompute final standings (blended scores)
#   4. Publish final leaderboard
#   5. Generate system test report (markdown)
#
# Usage:
#   ./scripts/post_contest_test.sh [options]
#
# Options:
#   --api URL         API base URL (default: http://localhost:8000)
#   --admin-key KEY   Admin key (default: iicpc-admin-2026)
#   --engine PATH     Path to run_contest binary
#   --no-firecracker  Run without VM isolation
#   --report PATH     Output report path (default: results/system_test_report.md)

set -euo pipefail

# Defaults
API_BASE="${API_BASE:-http://localhost:8000}"
ADMIN_KEY="${ADMIN_KEY:-iicpc-admin-2026}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
ENGINE="${PROJECT_ROOT}/build/run_contest"
REPORT_PATH="${PROJECT_ROOT}/results/system_test_report.md"
USE_FIRECRACKER="--no-firecracker"  # Default to direct for local testing

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

log()  { echo -e "${CYAN}[systest]${NC} $*"; }
pass() { echo -e "${GREEN}[✓]${NC} $*"; }
fail() { echo -e "${RED}[✗]${NC} $*"; }
warn() { echo -e "${YELLOW}[!]${NC} $*"; }

# Parse args
while [[ $# -gt 0 ]]; do
    case "$1" in
        --api)        API_BASE="$2"; shift 2 ;;
        --admin-key)  ADMIN_KEY="$2"; shift 2 ;;
        --engine)     ENGINE="$2"; shift 2 ;;
        --firecracker) USE_FIRECRACKER=""; shift ;;
        --no-firecracker) USE_FIRECRACKER="--no-firecracker"; shift ;;
        --report)     REPORT_PATH="$2"; shift 2 ;;
        *)            echo "Unknown arg: $1"; exit 1 ;;
    esac
done

echo ""
echo -e "${BOLD}--- IICPC Post-Contest System Testing ---${NC}"
echo ""

# Step 1: Preflight checks
log "Step 1: Preflight checks..."

if [[ ! -f "$ENGINE" ]]; then
    fail "Engine binary not found: $ENGINE"
    log "Run: cmake --build build --target run_contest"
    exit 1
fi
pass "Engine binary: $ENGINE"

# Check API health
HEALTH=$(curl -sf "$API_BASE/api/health" 2>/dev/null || echo '{}')
if echo "$HEALTH" | grep -q '"healthy"'; then
    pass "API server healthy"
else
    warn "API server not responding (running in offline mode)"
fi

# Step 2: Freeze competition
log "Step 2: Freezing competition..."

# Try to set competition state to system_testing via API
FREEZE=$(curl -sf -X POST "$API_BASE/api/admin/competition/stop" \
    -H "x-admin-key: $ADMIN_KEY" 2>/dev/null || echo '{}')

if echo "$FREEZE" | grep -q '"stopped"'; then
    pass "Competition frozen (submissions closed)"
else
    warn "Could not freeze via API — proceeding anyway"
fi

# Step 3: Trigger system tests via API (if API is available)
log "Step 3: Triggering system tests..."

TRIGGER=$(curl -sf -X POST "$API_BASE/api/admin/system-test" \
    -H "x-admin-key: $ADMIN_KEY" 2>/dev/null || echo '{}')

if echo "$TRIGGER" | grep -q '"running"'; then
    TOTAL=$(echo "$TRIGGER" | python3 -c "import sys,json; print(json.load(sys.stdin).get('total_teams',0))" 2>/dev/null || echo "0")
    pass "System tests triggered for $TOTAL teams"

    # Poll for completion
    log "Polling for system test completion..."
    TIMEOUT=600  # 10 minute timeout
    ELAPSED=0

    while [[ $ELAPSED -lt $TIMEOUT ]]; do
        sleep 5
        ELAPSED=$((ELAPSED + 5))

        STATUS=$(curl -sf "$API_BASE/api/admin/system-test/status" \
            -H "x-admin-key: $ADMIN_KEY" 2>/dev/null || echo '{}')

        STATE=$(echo "$STATUS" | python3 -c "import sys,json; print(json.load(sys.stdin).get('state',''))" 2>/dev/null || echo "")
        PROGRESS=$(echo "$STATUS" | python3 -c "import sys,json; print(json.load(sys.stdin).get('progress',''))" 2>/dev/null || echo "")

        echo -ne "\r  Progress: $PROGRESS ($ELAPSED s elapsed)    "

        if [[ "$STATE" == "completed" ]]; then
            echo ""
            pass "System tests completed"
            break
        elif [[ "$STATE" == "failed" ]]; then
            echo ""
            fail "System tests failed"
            break
        fi
    done

    if [[ $ELAPSED -ge $TIMEOUT ]]; then
        fail "System tests timed out after ${TIMEOUT}s"
    fi

else
    warn "API trigger failed — running system tests locally"

    # Offline mode: run system tests directly via CLI
    log "Running system tests in offline mode..."

    mkdir -p "$PROJECT_ROOT/results"
    RESULTS_DIR="$PROJECT_ROOT/results/system_tests"
    mkdir -p "$RESULTS_DIR"

    # Find all contestant binaries
    BINARIES_FOUND=0
    BINARIES_TESTED=0
    BINARIES_PASSED=0

    for binary in "$PROJECT_ROOT"/build/sample_orderbook; do
        if [[ -f "$binary" ]]; then
            BINARIES_FOUND=$((BINARIES_FOUND + 1))
            CONTESTANT=$(basename "$binary")

            log "Testing: $CONTESTANT"
            RESULT_FILE="$RESULTS_DIR/${CONTESTANT}.log"

            if "$ENGINE" --system-test \
                --binary "$binary" \
                --contestant "$CONTESTANT" \
                --original-score 1.0 \
                $USE_FIRECRACKER \
                --scenarios all \
                2>"$RESULT_FILE"; then
                pass "$CONTESTANT — all scenarios passed"
                BINARIES_PASSED=$((BINARIES_PASSED + 1))
            else
                fail "$CONTESTANT — some scenarios failed"
            fi

            BINARIES_TESTED=$((BINARIES_TESTED + 1))
        fi
    done

    if [[ $BINARIES_FOUND -eq 0 ]]; then
        warn "No contestant binaries found"
    fi
fi

# Step 4: Generate report
log "Step 4: Generating system test report..."

mkdir -p "$(dirname "$REPORT_PATH")"

# Try to fetch results from API
RESULTS_JSON=$(curl -sf "$API_BASE/api/admin/system-test/status" \
    -H "x-admin-key: $ADMIN_KEY" 2>/dev/null || echo '{}')

cat > "$REPORT_PATH" <<'HEADER'
# IICPC System Test Report

> Post-contest system testing (CF-style rejudge)

## Summary

HEADER

RESULTS_STATE=$(echo "$RESULTS_JSON" | python3 -c "import sys,json; print(json.load(sys.stdin).get('state','unknown'))" 2>/dev/null || echo "unknown")

if [[ "$RESULTS_STATE" == "completed" ]]; then
    # Parse and format results from API
    python3 -c "
import sys, json
data = json.loads('''$RESULTS_JSON''')
results = data.get('results', [])

print(f'| Rank | Team | Original | System | Final | Passed |')
print(f'|------|------|----------|--------|-------|--------|')

# Sort by final score descending
results.sort(key=lambda r: r.get('final_score', 0), reverse=True)

for i, r in enumerate(results, 1):
    team = r.get('team', '?')
    orig = r.get('original_score', 0)
    sys_score = r.get('system_score', 0)
    final = r.get('final_score', 0)
    passed = r.get('passed', 0)
    total = r.get('total', 10)
    status = '✓' if passed == total else '✗'
    print(f'| {i} | {team} | {orig:.4f} | {sys_score:.4f} | {final:.4f} | {status} {passed}/{total} |')
" >> "$REPORT_PATH" 2>/dev/null || echo "No API results available" >> "$REPORT_PATH"
else
    echo "System test state: $RESULTS_STATE" >> "$REPORT_PATH"
    echo "" >> "$REPORT_PATH"
    echo "Results not yet available. Run system tests via:" >> "$REPORT_PATH"
    echo '```' >> "$REPORT_PATH"
    echo "./scripts/post_contest_test.sh" >> "$REPORT_PATH"
    echo '```' >> "$REPORT_PATH"
fi

cat >> "$REPORT_PATH" <<FOOTER

## Scoring Formula

\`\`\`
final = min(contest_score, 0.6 * contest_score + 0.4 * system_score)
\`\`\`

System tests can only **lower** a ranking, never boost it.

## Stress Scenarios

| # | Name | Weight | Duration | Description |
|---|------|--------|----------|-------------|
| 1 | Crossed Book Stress | 15% | 8s | Rapid overlapping buy/sell at same prices |
| 2 | Deep Book Sweep | 12% | 10s | Large market orders sweeping 100+ levels |
| 3 | Cancel Storm | 10% | 8s | 90% cancel rate, tests cancel-rebooking |
| 4 | Self-Trade Trap | 8% | 8s | Tight spread, high volume near position limits |
| 5 | Tick-Size Edge | 8% | 6s | Min/max price boundaries, tick alignment |
| 6 | Burst Traffic | 12% | 5s | Short-duration extreme OPS spike |
| 7 | IOC Flood | 8% | 6s | Pure IOC/Market orders, nothing rests |
| 8 | Rapid Order ID Churn | 8% | 8s | High volume, small ID space |
| 9 | Position Limit Grind | 9% | 8s | Orders at position limit boundaries |
| 10 | Conservation Audit | 10% | 10s | Balanced buy/sell, qty conservation |

---

*Generated by \`post_contest_test.sh\` at $(date -u +"%Y-%m-%dT%H:%M:%SZ")*
FOOTER

pass "Report written to: $REPORT_PATH"

# Step 5: Fetch final leaderboard
log "Step 5: Final leaderboard..."

LEADERBOARD=$(curl -sf "$API_BASE/api/leaderboard" 2>/dev/null || echo '[]')
LB_COUNT=$(echo "$LEADERBOARD" | python3 -c "import sys,json; print(len(json.load(sys.stdin)))" 2>/dev/null || echo "0")

if [[ "$LB_COUNT" -gt 0 ]]; then
    echo ""
echo -e "${BOLD}--- Final Standings (Post System Test) ---${NC}"

    python3 -c "
import sys, json
lb = json.loads('''$LEADERBOARD''')
for entry in lb[:20]:
    rank = entry.get('rank', '?')
    team = entry.get('team_name', '?')
    score = entry.get('score', 0)
    print(f'  #{rank:>2}  {team:<30}  {score:>10.4f}')
" 2>/dev/null || echo "  Could not parse leaderboard"

fi

# Summary
echo ""
echo -e "${CYAN}═══════════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  Post-Contest System Testing Complete${NC}"
echo -e "${CYAN}═══════════════════════════════════════════════════════${NC}"
echo -e "  Report: ${GREEN}$REPORT_PATH${NC}"
echo -e "  State:  ${GREEN}$RESULTS_STATE${NC}"
echo -e "${CYAN}═══════════════════════════════════════════════════════${NC}"
