#!/bin/bash
# =============================================================================
# pre_submit.sh — Pre-Submission Validation
# =============================================================================
# Runs ALL checks locally to ensure the project is submission-ready.
# Exit code 0 = safe to submit. Non-zero = fix issues first.
#
# Usage: ./scripts/pre_submit.sh
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

PASS=0
FAIL=0
WARN=0

pass() { echo -e "  ${GREEN}✓${NC} $*"; PASS=$((PASS+1)); }
fail() { echo -e "  ${RED}✗${NC} $*"; FAIL=$((FAIL+1)); }
warn() { echo -e "  ${YELLOW}!${NC} $*"; WARN=$((WARN+1)); }

echo -e "${BOLD}╔═══════════════════════════════════════════════════════╗${NC}"
echo -e "${BOLD}║     IICPC Pre-Submission Validation Suite             ║${NC}"
echo -e "${BOLD}╚═══════════════════════════════════════════════════════╝${NC}"
echo ""

# =============================================================================
# 1. Project structure
# =============================================================================
echo -e "${CYAN}[1/7] Project Structure${NC}"

for f in CMakeLists.txt README.md; do
    [ -f "$PROJECT_ROOT/$f" ] && pass "$f exists" || fail "$f missing"
done

for d in core sdk exchange fpga infra scripts web; do
    [ -d "$PROJECT_ROOT/$d" ] && pass "$d/ directory" || fail "$d/ missing"
done

# FPGA RTL
for f in sequencer_core.sv order_parser.sv dma_ring.sv sequencer_top.sv match_engine_fpga.sv; do
    [ -f "$PROJECT_ROOT/fpga/rtl/$f" ] && pass "fpga/rtl/$f" || fail "fpga/rtl/$f missing"
done

# Testbenches
for f in tb_sequencer.sv tb_match_engine.sv; do
    [ -f "$PROJECT_ROOT/fpga/sim/$f" ] && pass "fpga/sim/$f" || fail "fpga/sim/$f missing"
done

# =============================================================================
# 2. C++ Build
# =============================================================================
echo ""
echo -e "${CYAN}[2/7] C++ Build${NC}"

cd "$PROJECT_ROOT"
mkdir -p build && cd build

# Configure if needed
if [ ! -f "CMakeCache.txt" ]; then
    cmake .. -DCMAKE_BUILD_TYPE=Release -DIICPC_USE_URING=OFF 2>/dev/null
fi

BUILD_OUTPUT=$(cmake --build . -j$(nproc) 2>&1)
if [ $? -eq 0 ]; then
    pass "C++ build succeeded"
else
    fail "C++ build failed"
    echo "$BUILD_OUTPUT" | tail -10
fi

# Check for warnings/errors
ERROR_COUNT=$(echo "$BUILD_OUTPUT" | grep -c "error:" || true)
WARN_COUNT=$(echo "$BUILD_OUTPUT" | grep -c "warning:" || true)
ERROR_COUNT=${ERROR_COUNT:-0}
WARN_COUNT=${WARN_COUNT:-0}
[ "$ERROR_COUNT" -eq 0 ] 2>/dev/null && pass "0 build errors" || fail "$ERROR_COUNT build errors"
[ "$WARN_COUNT" -eq 0 ] 2>/dev/null && pass "0 build warnings" || warn "$WARN_COUNT build warnings"

# Critical binaries
for bin in worker_agent integrated_worker bench_ultra; do
    if [ -f "$PROJECT_ROOT/build/bin/$bin" ] || find . -name "$bin" -type f 2>/dev/null | head -1 | grep -q .; then
        pass "Binary: $bin"
    else
        warn "Binary not found: $bin"
    fi
done

# =============================================================================
# 3. Header compilation (new components)
# =============================================================================
echo ""
echo -e "${CYAN}[3/7] New Component Headers${NC}"

cd "$PROJECT_ROOT"
for header in sequencer.hpp audit_log.hpp correctness_validator.hpp; do
    if g++ -std=c++23 -O2 -fsyntax-only \
        -I exchange/include -I sdk/include -I core/include \
        -c -x c++ - <<< "#include \"exchange/$header\"" 2>/dev/null; then
        pass "Header compiles: $header"
    else
        fail "Header fails: $header"
    fi
done

# =============================================================================
# 4. FPGA Simulation (if Verilator installed)
# =============================================================================
echo ""
echo -e "${CYAN}[4/7] FPGA Simulation${NC}"

if command -v verilator &>/dev/null; then
    pass "Verilator $(verilator --version 2>&1 | awk '{print $2}')"

    cd "$PROJECT_ROOT/fpga"

    # Sequencer testbench
    SEQ_OUTPUT=$(make sim 2>&1)
    if echo "$SEQ_OUTPUT" | grep -q "ALL TESTS PASSED"; then
        pass "Sequencer testbench: ALL TESTS PASSED"
    elif echo "$SEQ_OUTPUT" | grep -q "Errors:.*0"; then
        pass "Sequencer testbench: 0 errors"
    else
        fail "Sequencer testbench failed"
    fi

    # Matching engine testbench
    MATCH_OUTPUT=$(make sim_match 2>&1)
    MATCH_FILLS=$(echo "$MATCH_OUTPUT" | grep "Total fills:" | awk '{print $3}')
    MATCH_ERRORS=$(echo "$MATCH_OUTPUT" | grep "Errors:" | awk '{print $2}')
    if [ -n "$MATCH_FILLS" ] && [ "$MATCH_FILLS" -gt 0 ]; then
        pass "Matching engine: $MATCH_FILLS fills generated"
    else
        warn "Matching engine: no fills (check testbench)"
    fi

    # Throughput
    THROUGHPUT=$(echo "$MATCH_OUTPUT" | grep "Throughput:" | head -1)
    if [ -n "$THROUGHPUT" ]; then
        pass "FPGA $THROUGHPUT"
    fi
else
    warn "Verilator not installed — skipping FPGA simulation"
fi

# =============================================================================
# 5. Infrastructure files
# =============================================================================
echo ""
echo -e "${CYAN}[5/7] Infrastructure${NC}"

# Terraform
for f in main.tf fpga.tf bootstrap.sh terraform.tfvars deploy.sh; do
    [ -f "$PROJECT_ROOT/infra/terraform/$f" ] && pass "terraform/$f" || fail "terraform/$f missing"
done

# Terraform validate (if installed)
if command -v terraform &>/dev/null; then
    cd "$PROJECT_ROOT/infra/terraform"
    if terraform validate 2>/dev/null; then
        pass "Terraform config valid"
    else
        # Init first
        terraform init -backend=false 2>/dev/null
        if terraform validate 2>/dev/null; then
            pass "Terraform config valid"
        else
            fail "Terraform validation failed"
        fi
    fi
else
    warn "Terraform not installed — skipping validation"
fi

# Docker compose
if [ -f "$PROJECT_ROOT/infra/docker/docker-compose.yml" ]; then
    pass "docker-compose.yml exists"
else
    warn "docker-compose.yml not found"
fi

# =============================================================================
# 6. Scripts
# =============================================================================
echo ""
echo -e "${CYAN}[6/7] Scripts${NC}"

for script in build_rootfs.sh firecracker_sandbox.sh soak_test.sh e2e_test.sh; do
    if [ -f "$PROJECT_ROOT/scripts/$script" ]; then
        if [ -x "$PROJECT_ROOT/scripts/$script" ] || head -1 "$PROJECT_ROOT/scripts/$script" | grep -q "#!/bin/bash"; then
            pass "scripts/$script"
        else
            warn "scripts/$script not executable"
        fi
    else
        warn "scripts/$script missing"
    fi
done

# FPGA
[ -f "$PROJECT_ROOT/fpga/Makefile" ] && pass "fpga/Makefile" || fail "fpga/Makefile missing"
[ -f "$PROJECT_ROOT/fpga/aws/build_afi.sh" ] && pass "fpga/aws/build_afi.sh" || fail "fpga/aws/build_afi.sh missing"

# =============================================================================
# 7. Documentation
# =============================================================================
echo ""
echo -e "${CYAN}[7/7] Documentation${NC}"

[ -f "$PROJECT_ROOT/README.md" ] && pass "README.md" || fail "README.md missing"

# Check README has key sections
if [ -f "$PROJECT_ROOT/README.md" ]; then
    for section in "Architecture" "Deploy" "FPGA"; do
        if grep -qi "$section" "$PROJECT_ROOT/README.md"; then
            pass "README contains: $section"
        else
            warn "README missing section: $section"
        fi
    done
fi

# =============================================================================
# Summary
# =============================================================================
echo ""
echo -e "${BOLD}═══════════════════════════════════════════════════════${NC}"
echo -e "  ${GREEN}PASS${NC}: $PASS   ${RED}FAIL${NC}: $FAIL   ${YELLOW}WARN${NC}: $WARN"
echo -e "${BOLD}═══════════════════════════════════════════════════════${NC}"

if [ $FAIL -eq 0 ]; then
    echo -e ""
    echo -e "  ${GREEN}${BOLD}✓ SUBMISSION READY${NC}"
    echo -e ""
    echo -e "  Deploy: cd infra/terraform && ./deploy.sh init && ./deploy.sh apply"
    echo -e ""
    exit 0
else
    echo -e ""
    echo -e "  ${RED}${BOLD}✗ $FAIL ISSUE(S) FOUND — FIX BEFORE SUBMITTING${NC}"
    echo -e ""
    exit 1
fi
