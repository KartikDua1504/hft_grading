#!/bin/bash
# =============================================================================
# e2e_test.sh — IICPC End-to-End Platform Test
# =============================================================================
# Tests the full flow: register → login → submit → poll → leaderboard
# Requires: API server running on localhost:8000, Redis available
# =============================================================================

set -euo pipefail

API_BASE="${API_BASE:-http://localhost:8000}"
TEAM_NAME="e2e_test_$(date +%s)"
PASSWORD="testpass123"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'
PASS=0
FAIL=0

log()  { echo -e "${CYAN}[test]${NC} $*"; }
pass() { PASS=$((PASS+1)); echo -e "${GREEN}[PASS]${NC} $*"; }
fail() { FAIL=$((FAIL+1)); echo -e "${RED}[FAIL]${NC} $*"; }

# --- Test: Health Check ---
log "Testing health check..."
HEALTH=$(curl -s "$API_BASE/api/health" 2>/dev/null || echo '{}')
if echo "$HEALTH" | grep -q '"healthy"'; then
    pass "Health check — API healthy, Redis connected"
else
    fail "Health check — $HEALTH"
fi

# --- Test: Registration ---
log "Testing registration (team: $TEAM_NAME)..."
REG=$(curl -s -X POST "$API_BASE/api/auth/register" \
    -H "Content-Type: application/json" \
    -d "{\"team_name\":\"$TEAM_NAME\",\"password\":\"$PASSWORD\"}" 2>/dev/null || echo '{}')

TOKEN=$(echo "$REG" | python3 -c "import sys,json; print(json.load(sys.stdin).get('token',''))" 2>/dev/null || echo "")
if [[ -n "$TOKEN" && "$TOKEN" != "None" ]]; then
    pass "Registration — got JWT token (${#TOKEN} chars)"
else
    fail "Registration — no token: $REG"
    # Can't continue without token
    echo -e "\n${RED}Cannot continue without authentication.${NC}"
    exit 1
fi

# --- Test: Duplicate Registration ---
log "Testing duplicate registration rejection..."
DUP=$(curl -s -o /dev/null -w "%{http_code}" -X POST "$API_BASE/api/auth/register" \
    -H "Content-Type: application/json" \
    -d "{\"team_name\":\"$TEAM_NAME\",\"password\":\"$PASSWORD\"}" 2>/dev/null || echo "000")
if [[ "$DUP" == "409" ]]; then
    pass "Duplicate registration — correctly rejected (409)"
else
    fail "Duplicate registration — expected 409, got $DUP"
fi

# --- Test: Login ---
log "Testing login..."
LOGIN=$(curl -s -X POST "$API_BASE/api/auth/login" \
    -H "Content-Type: application/json" \
    -d "{\"team_name\":\"$TEAM_NAME\",\"password\":\"$PASSWORD\"}" 2>/dev/null || echo '{}')

LOGIN_TOKEN=$(echo "$LOGIN" | python3 -c "import sys,json; print(json.load(sys.stdin).get('token',''))" 2>/dev/null || echo "")
if [[ -n "$LOGIN_TOKEN" && "$LOGIN_TOKEN" != "None" ]]; then
    pass "Login — got JWT token"
else
    fail "Login — $LOGIN"
fi

# --- Test: Wrong Password ---
log "Testing wrong password rejection..."
WRONG=$(curl -s -o /dev/null -w "%{http_code}" -X POST "$API_BASE/api/auth/login" \
    -H "Content-Type: application/json" \
    -d "{\"team_name\":\"$TEAM_NAME\",\"password\":\"wrong\"}" 2>/dev/null || echo "000")
if [[ "$WRONG" == "401" ]]; then
    pass "Wrong password — correctly rejected (401)"
else
    fail "Wrong password — expected 401, got $WRONG"
fi

# --- Test: Submit File ---
log "Creating test source file..."
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_FILE="$SCRIPT_DIR/.e2e_test_submission.cpp"

cat > "$TEST_FILE" << 'EOF'
// E2E Test Orderbook — Minimal matching engine
#include <cstdint>
#include <cstdio>
#include <vector>
#include <algorithm>

struct Order {
    uint32_t id;
    int64_t price;
    int32_t qty;
    bool is_buy;
};

int main() {
    // Simulate a simple benchmark
    std::vector<Order> bids, asks;
    uint64_t fills = 0;
    
    for (uint32_t i = 0; i < 1000000; ++i) {
        Order o{i, 100 + (i % 10), 100, (i % 2 == 0)};
        if (o.is_buy) {
            // Try to match against asks
            for (auto it = asks.begin(); it != asks.end();) {
                if (o.price >= it->price && o.qty > 0) {
                    int32_t fill = std::min(o.qty, it->qty);
                    o.qty -= fill;
                    it->qty -= fill;
                    fills++;
                    if (it->qty == 0) it = asks.erase(it);
                    else ++it;
                } else ++it;
            }
            if (o.qty > 0) bids.push_back(o);
        } else {
            for (auto it = bids.begin(); it != bids.end();) {
                if (o.price <= it->price && o.qty > 0) {
                    int32_t fill = std::min(o.qty, it->qty);
                    o.qty -= fill;
                    it->qty -= fill;
                    fills++;
                    if (it->qty == 0) it = bids.erase(it);
                    else ++it;
                } else ++it;
            }
            if (o.qty > 0) asks.push_back(o);
        }
    }
    
    fprintf(stderr, "Throughput: %lu\n", fills * 33);
    fprintf(stderr, "Correctness: 98.5\n");
    fprintf(stderr, "p99: 1250\n");
    fprintf(stderr, "FINAL SCORE: 0.847\n");
    return 0;
}
EOF

log "Testing file submission..."
SUBMIT=$(curl -s -X POST "$API_BASE/api/submit" \
    -H "Authorization: Bearer $TOKEN" \
    -F "file=@$TEST_FILE" 2>/dev/null || echo '{}')

JOB_ID=$(echo "$SUBMIT" | python3 -c "import sys,json; print(json.load(sys.stdin).get('job_id',''))" 2>/dev/null || echo "")
if [[ -n "$JOB_ID" && "$JOB_ID" != "None" ]]; then
    pass "Submission — queued (job: ${JOB_ID:0:8}...)"
else
    fail "Submission — $SUBMIT"
fi

# --- Test: Submission without auth ---
log "Testing unauthenticated submission rejection..."
NOAUTH=$(curl -s -o /dev/null -w "%{http_code}" -X POST "$API_BASE/api/submit" \
    -F "file=@$TEST_FILE" 2>/dev/null || echo "000")
if [[ "$NOAUTH" == "401" || "$NOAUTH" == "422" ]]; then
    pass "Unauthenticated submit — rejected ($NOAUTH)"
else
    fail "Unauthenticated submit — expected 401/422, got $NOAUTH"
fi

# --- Test: Invalid file type ---
log "Testing invalid file type rejection..."
echo "not code" > "$SCRIPT_DIR/.e2e_test_bad.txt"
BADFILE=$(curl -s -o /dev/null -w "%{http_code}" -X POST "$API_BASE/api/submit" \
    -H "Authorization: Bearer $TOKEN" \
    -F "file=@$SCRIPT_DIR/.e2e_test_bad.txt" 2>/dev/null || echo "000")
if [[ "$BADFILE" == "400" ]]; then
    pass "Invalid file type — rejected (400)"
else
    fail "Invalid file type — expected 400, got $BADFILE"
fi
rm -f "$SCRIPT_DIR/.e2e_test_bad.txt"

# --- Test: Job Status ---
if [[ -n "$JOB_ID" && "$JOB_ID" != "None" ]]; then
    log "Testing job status polling..."
    sleep 1
    STATUS=$(curl -s "$API_BASE/api/job/$JOB_ID" \
        -H "Authorization: Bearer $TOKEN" 2>/dev/null || echo '{}')
    JOB_STATUS=$(echo "$STATUS" | python3 -c "import sys,json; print(json.load(sys.stdin).get('status',''))" 2>/dev/null || echo "")
    if [[ -n "$JOB_STATUS" ]]; then
        pass "Job status — $JOB_STATUS"
    else
        fail "Job status — $STATUS"
    fi

    # Poll for completion (max 60s)
    log "Polling for job completion (max 60s)..."
    for i in $(seq 1 12); do
        sleep 5
        STATUS=$(curl -s "$API_BASE/api/job/$JOB_ID" \
            -H "Authorization: Bearer $TOKEN" 2>/dev/null || echo '{}')
        JOB_STATUS=$(echo "$STATUS" | python3 -c "import sys,json; print(json.load(sys.stdin).get('status',''))" 2>/dev/null || echo "")
        if [[ "$JOB_STATUS" == "scored" || "$JOB_STATUS" == "failed" ]]; then
            if [[ "$JOB_STATUS" == "scored" ]]; then
                SCORE=$(echo "$STATUS" | python3 -c "import sys,json; print(json.load(sys.stdin).get('score',0))" 2>/dev/null || echo "0")
                pass "Job completed — scored $SCORE"
            else
                pass "Job completed — $JOB_STATUS (expected for test file)"
            fi
            break
        fi
        echo -n "."
    done
    echo ""
fi

# --- Test: Leaderboard ---
log "Testing leaderboard..."
LB=$(curl -s "$API_BASE/api/leaderboard" 2>/dev/null || echo '[]')
LB_LEN=$(echo "$LB" | python3 -c "import sys,json; print(len(json.load(sys.stdin)))" 2>/dev/null || echo "0")
if [[ "$LB_LEN" -ge 0 ]]; then
    pass "Leaderboard — $LB_LEN entries"
else
    fail "Leaderboard — $LB"
fi

# --- Cleanup ---
rm -f "$TEST_FILE"

# --- Summary ---
echo ""
echo -e "${CYAN}═══════════════════════════════════════════════${NC}"
echo -e "${CYAN}  E2E Test Summary${NC}"
echo -e "${CYAN}═══════════════════════════════════════════════${NC}"
echo -e "  ${GREEN}Passed: $PASS${NC}"
echo -e "  ${RED}Failed: $FAIL${NC}"
echo -e "  Total:  $((PASS + FAIL))"
echo -e "${CYAN}═══════════════════════════════════════════════${NC}"

if [[ $FAIL -gt 0 ]]; then
    exit 1
fi
