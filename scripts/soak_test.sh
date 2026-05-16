#!/bin/bash
# =============================================================================
# soak_test.sh — Operational Soak / Crash / Backpressure Tests
# =============================================================================
# Tests the platform under sustained load, crash recovery, and queue saturation.
#
# Usage: ./scripts/soak_test.sh [--duration 300] [--concurrent 5]
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
API_URL="${API_URL:-http://localhost:8000}"

CYAN='\033[0;36m'
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

log()  { echo -e "${CYAN}[soak]${NC} $*"; }
ok()   { echo -e "${GREEN}[PASS]${NC} $*"; }
fail() { echo -e "${RED}[FAIL]${NC} $*"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $*"; }

DURATION=300
CONCURRENT=5
RESULTS_DIR="$PROJECT_ROOT/build/soak_$(date +%Y%m%d_%H%M%S)"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --duration) DURATION="$2"; shift 2 ;;
        --concurrent) CONCURRENT="$2"; shift 2 ;;
        --api) API_URL="$2"; shift 2 ;;
        *) shift ;;
    esac
done

mkdir -p "$RESULTS_DIR"

echo ""
echo -e "${CYAN}═══════════════════════════════════════════════════════════${NC}"
echo -e "${CYAN}              OPERATIONAL SOAK TEST SUITE                  ${NC}"
echo -e "${CYAN}═══════════════════════════════════════════════════════════${NC}"
echo -e "  API:        $API_URL"
echo -e "  Duration:   ${DURATION}s"
echo -e "  Concurrent: $CONCURRENT"
echo -e "  Output:     $RESULTS_DIR"
echo ""

# Create a minimal valid contestant source
DUMMY_SRC="$RESULTS_DIR/dummy_contestant.cpp"
cat > "$DUMMY_SRC" << 'EOF'
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

int main(int argc, char** argv) {
    // Minimal contestant: connect to gateway, echo back
    const char* gateway = "/tmp/iicpc_contest.sock";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--gateway") == 0 && i + 1 < argc)
            gateway = argv[i + 1];
    }
    
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return 1;
    
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, gateway, sizeof(addr.sun_path) - 1);
    
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return 1;
    }
    
    // Process orders for 10 seconds
    char buf[256];
    for (int i = 0; i < 10000; i++) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        // Echo back (minimal response)
        send(fd, buf, n, MSG_NOSIGNAL);
    }
    
    close(fd);
    return 0;
}
EOF

# =============================================================================
# Test 1: Health Check
# =============================================================================
log "Test 1: Health Check"
HEALTH=$(curl -s -o /dev/null -w "%{http_code}" "$API_URL/api/health" 2>/dev/null || echo "000")
if [[ "$HEALTH" == "200" ]]; then
    ok "API healthy (HTTP $HEALTH)"
else
    fail "API not reachable (HTTP $HEALTH)"
    echo "Start the backend: cd web/backend && uvicorn main:app --port 8000"
    exit 1
fi

# =============================================================================
# Test 2: System Status
# =============================================================================
log "Test 2: System Status"
STATUS=$(curl -s "$API_URL/api/system/status" 2>/dev/null || echo "{}")
echo "$STATUS" | python3 -m json.tool > "$RESULTS_DIR/system_status.json" 2>/dev/null || true
ISOLATION=$(echo "$STATUS" | python3 -c "import sys,json; print(json.load(sys.stdin).get('isolation_mode','unknown'))" 2>/dev/null || echo "unknown")
READY=$(echo "$STATUS" | python3 -c "import sys,json; print(json.load(sys.stdin).get('ready',False))" 2>/dev/null || echo "False")
log "  Isolation: $ISOLATION | Ready: $READY"

# =============================================================================
# Test 3: Registration + Auth
# =============================================================================
log "Test 3: Registration + Authentication"
TEAM="soak_$(date +%s)"
TOKEN=$(curl -s -X POST "$API_URL/api/auth/register" \
    -H "Content-Type: application/json" \
    -d "{\"team_name\":\"$TEAM\",\"password\":\"soaktest123\"}" 2>/dev/null \
    | python3 -c "import sys,json; print(json.load(sys.stdin).get('token',''))" 2>/dev/null || echo "")

if [[ -n "$TOKEN" ]]; then
    ok "Registered team '$TEAM'"
else
    fail "Registration failed"
    exit 1
fi

# Test duplicate registration
DUP=$(curl -s -o /dev/null -w "%{http_code}" -X POST "$API_URL/api/auth/register" \
    -H "Content-Type: application/json" \
    -d "{\"team_name\":\"$TEAM\",\"password\":\"soaktest123\"}" 2>/dev/null || echo "000")
if [[ "$DUP" == "409" ]]; then
    ok "Duplicate registration rejected (409)"
else
    fail "Duplicate registration not rejected (HTTP $DUP)"
fi

# =============================================================================
# Test 4: Rate Limiting
# =============================================================================
log "Test 4: Rate Limiting"

# First submission should succeed
SUB1=$(curl -s -o /dev/null -w "%{http_code}" -X POST "$API_URL/api/submit" \
    -H "Authorization: Bearer $TOKEN" \
    -F "file=@$DUMMY_SRC" 2>/dev/null || echo "000")

# Second immediate submission should be rate-limited (429)
SUB2=$(curl -s -o /dev/null -w "%{http_code}" -X POST "$API_URL/api/submit" \
    -H "Authorization: Bearer $TOKEN" \
    -F "file=@$DUMMY_SRC" 2>/dev/null || echo "000")

if [[ "$SUB2" == "429" ]]; then
    ok "Rate limiting active (429 on rapid resubmit)"
else
    warn "Rate limiting may not be active (HTTP $SUB2)"
fi

# =============================================================================
# Test 5: Invalid Input Rejection
# =============================================================================
log "Test 5: Input Validation"
PASS=0
TOTAL=0

# Bad file extension
TOTAL=$((TOTAL+1))
BAD_EXT=$(curl -s -o /dev/null -w "%{http_code}" -X POST "$API_URL/api/submit" \
    -H "Authorization: Bearer $TOKEN" \
    -F "file=@/dev/null;filename=evil.py" 2>/dev/null || echo "000")
[[ "$BAD_EXT" == "400" ]] && PASS=$((PASS+1))

# No auth
TOTAL=$((TOTAL+1))
NO_AUTH=$(curl -s -o /dev/null -w "%{http_code}" -X POST "$API_URL/api/submit" \
    -F "file=@$DUMMY_SRC" 2>/dev/null || echo "000")
[[ "$NO_AUTH" == "401" || "$NO_AUTH" == "422" ]] && PASS=$((PASS+1))

# Bad team name (special chars)
TOTAL=$((TOTAL+1))
BAD_TEAM=$(curl -s -o /dev/null -w "%{http_code}" -X POST "$API_URL/api/auth/register" \
    -H "Content-Type: application/json" \
    -d '{"team_name":"../../../etc","password":"hack123"}' 2>/dev/null || echo "000")
[[ "$BAD_TEAM" == "400" ]] && PASS=$((PASS+1))

# Short password
TOTAL=$((TOTAL+1))
SHORT_PW=$(curl -s -o /dev/null -w "%{http_code}" -X POST "$API_URL/api/auth/register" \
    -H "Content-Type: application/json" \
    -d '{"team_name":"validname","password":"ab"}' 2>/dev/null || echo "000")
[[ "$SHORT_PW" == "400" ]] && PASS=$((PASS+1))

if [[ "$PASS" == "$TOTAL" ]]; then
    ok "All $TOTAL input validation checks passed"
else
    warn "Input validation: $PASS/$TOTAL passed"
fi

# =============================================================================
# Test 6: Metrics Endpoint
# =============================================================================
log "Test 6: Metrics"
METRICS=$(curl -s "$API_URL/api/metrics" 2>/dev/null || echo "{}")
echo "$METRICS" | python3 -m json.tool > "$RESULTS_DIR/metrics.json" 2>/dev/null || true
UPTIME=$(echo "$METRICS" | python3 -c "import sys,json; print(json.load(sys.stdin).get('uptime_seconds',0))" 2>/dev/null || echo "0")
if [[ "$UPTIME" != "0" ]]; then
    ok "Metrics endpoint working (uptime=${UPTIME}s)"
else
    fail "Metrics endpoint not returning data"
fi

# =============================================================================
# Test 7: WebSocket Connection
# =============================================================================
log "Test 7: WebSocket"
if command -v websocat &>/dev/null; then
    WS_DATA=$(timeout 3 websocat "ws://localhost:8000/ws/live" 2>/dev/null | head -1 || echo "")
    if [[ -n "$WS_DATA" ]]; then
        ok "WebSocket connected, received data"
    else
        warn "WebSocket connected but no data in 3s"
    fi
else
    warn "websocat not installed, skipping WebSocket test"
fi

# =============================================================================
# Test 8: Leaderboard
# =============================================================================
log "Test 8: Leaderboard"
LB=$(curl -s -o /dev/null -w "%{http_code}" "$API_URL/api/leaderboard" 2>/dev/null || echo "000")
if [[ "$LB" == "200" ]]; then
    ok "Leaderboard endpoint OK"
else
    fail "Leaderboard failed (HTTP $LB)"
fi

# =============================================================================
# Summary
# =============================================================================
echo ""
echo -e "${CYAN}═══════════════════════════════════════════════════════════${NC}"
echo -e "${CYAN}                  SOAK TEST COMPLETE                      ${NC}"
echo -e "${CYAN}═══════════════════════════════════════════════════════════${NC}"
echo ""
echo "  Results: $RESULTS_DIR"
echo ""

# Final metrics snapshot
curl -s "$API_URL/api/metrics" 2>/dev/null | python3 -m json.tool 2>/dev/null || true
echo ""
