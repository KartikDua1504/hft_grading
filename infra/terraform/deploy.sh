#!/bin/bash
# deploy.sh — One-Command Production Deployment
# Usage:
#   ./deploy.sh init        — First-time: terraform init + plan
#   ./deploy.sh plan        — Preview infrastructure changes
#   ./deploy.sh apply       — Deploy everything to AWS
#   ./deploy.sh sync        — Sync project files to running instance
#   ./deploy.sh verify      — Run E2E verification on the deployed instance
#   ./deploy.sh destroy     — Tear down everything (saves money!)
#   ./deploy.sh status      — Check instance status + health
#   ./deploy.sh fpga-on     — Enable FPGA instances
#   ./deploy.sh fpga-off    — Disable FPGA instances

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
TF_DIR="$SCRIPT_DIR"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

log() { echo -e "${CYAN}[deploy]${NC} $*"; }
ok()  { echo -e "${GREEN}[✓]${NC} $*"; }
warn(){ echo -e "${YELLOW}[!]${NC} $*"; }
err() { echo -e "${RED}[✗]${NC} $*"; }

# Preflight checks
preflight() {
    log "Running preflight checks..."
    local failed=0

    # Terraform
    if command -v terraform &>/dev/null; then
        ok "Terraform $(terraform version -json 2>/dev/null | jq -r '.terraform_version' 2>/dev/null || terraform version | head -1)"
    else
        err "Terraform not installed. Install: https://developer.hashicorp.com/terraform/install"
        failed=1
    fi

    # AWS CLI
    if command -v aws &>/dev/null; then
        ok "AWS CLI $(aws --version 2>&1 | awk '{print $1}')"
    else
        err "AWS CLI not installed. Install: https://docs.aws.amazon.com/cli/latest/userguide/getting-started-install.html"
        failed=1
    fi

    # AWS credentials
    if aws sts get-caller-identity &>/dev/null; then
        local account_id=$(aws sts get-caller-identity --query 'Account' --output text)
        ok "AWS credentials valid (account: $account_id)"
    else
        err "AWS credentials not configured. Run: aws configure"
        failed=1
    fi

    # SSH key
    local key_name=$(grep '^key_name' "$TF_DIR/terraform.tfvars" 2>/dev/null | awk -F'"' '{print $2}')
    if [ -n "$key_name" ]; then
        if [ -f "$HOME/.ssh/${key_name}.pem" ]; then
            ok "SSH key found: ~/.ssh/${key_name}.pem"
        else
            warn "SSH key not found locally: ~/.ssh/${key_name}.pem"
            warn "Make sure it exists in AWS EC2 Key Pairs"
        fi
    fi

    # C++ build
    if [ -d "$PROJECT_ROOT/build" ] && [ -f "$PROJECT_ROOT/build/bin/worker_agent" ]; then
        ok "C++ engine built"
    else
        warn "C++ engine not built. Run: cd build && cmake --build . -j\$(nproc)"
    fi

    # FPGA simulation
    if [ -f "$PROJECT_ROOT/fpga/build/ver_seq/Vtb_sequencer" ]; then
        ok "FPGA sequencer testbench built"
    else
        warn "FPGA testbench not built. Run: cd fpga && make sim_all"
    fi

    if [ $failed -ne 0 ]; then
        err "Preflight failed. Fix the above issues."
        exit 1
    fi
    ok "All preflight checks passed"
}

# Commands
cmd_init() {
    preflight
    log "Initializing Terraform..."
    cd "$TF_DIR"
    terraform init
    ok "Terraform initialized"
    log "Run './deploy.sh plan' to preview changes"
}

cmd_plan() {
    preflight
    log "Planning infrastructure..."
    cd "$TF_DIR"
    terraform plan -out=tfplan
    ok "Plan saved to tfplan"
    log "Review the plan, then run './deploy.sh apply' to deploy"
}

cmd_apply() {
    preflight
    log "Deploying IICPC infrastructure..."
    cd "$TF_DIR"

    # Run plan if not already done
    if [ ! -f tfplan ]; then
        terraform plan -out=tfplan
    fi

    echo ""
    warn "This will create AWS resources and incur costs!"
    echo -e "  Instance: $(grep 'instance_type' terraform.tfvars | awk -F'"' '{print $2}')"
    echo -e "  Region:   $(grep 'aws_region' terraform.tfvars | awk -F'"' '{print $2}')"
    echo ""
    read -p "Continue? [y/N] " -n 1 -r
    echo ""
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        log "Aborted."
        exit 0
    fi

    terraform apply tfplan
    rm -f tfplan

    echo ""
    ok "=== DEPLOYMENT COMPLETE ==="
    terraform output
    echo ""
    log "Next: Run './deploy.sh sync' to push project files"
}

cmd_sync() {
    cd "$TF_DIR"
    local ip=$(terraform output -raw arena_public_ip 2>/dev/null)
    local key_name=$(grep '^key_name' terraform.tfvars | awk -F'"' '{print $2}')
    local key_file="$HOME/.ssh/${key_name}.pem"

    if [ -z "$ip" ] || [ "$ip" = "" ]; then
        err "No arena instance found. Run './deploy.sh apply' first."
        exit 1
    fi

    log "Syncing project to $ip..."

    # Sync project files (exclude build, .git, node_modules)
    rsync -avz --progress \
        --exclude 'build/' \
        --exclude '.git/' \
        --exclude 'node_modules/' \
        --exclude '.cache/' \
        --exclude 'fpga/build/' \
        --exclude '*.o' \
        --exclude '*.vcd' \
        --exclude 'dump.rdb' \
        -e "ssh -i $key_file -o StrictHostKeyChecking=no" \
        "$PROJECT_ROOT/" \
        "ubuntu@${ip}:~/IICPC/"

    ok "Project synced to ubuntu@${ip}:~/IICPC/"

    # Remote build
    log "Building C++ engine on remote..."
    ssh -i "$key_file" -o StrictHostKeyChecking=no "ubuntu@${ip}" << 'REMOTE_BUILD'
set -e
cd ~/IICPC
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DIICPC_USE_URING=OFF -G Ninja 2>/dev/null || \
cmake .. -DCMAKE_BUILD_TYPE=Release -DIICPC_USE_URING=OFF
make -j$(nproc) 2>/dev/null || cmake --build . -j$(nproc)
echo "Remote build complete"

# Copy to /opt/iicpc
sudo mkdir -p /opt/iicpc
sudo cp -r ~/IICPC/* /opt/iicpc/ 2>/dev/null || true

# Start API service
sudo systemctl enable --now iicpc-api 2>/dev/null || echo "API service not configured yet"
REMOTE_BUILD

    ok "Remote build complete"
    log "Dashboard: http://${ip}"
    log "SSH: ssh -i $key_file ubuntu@${ip}"
}

cmd_verify() {
    cd "$TF_DIR"
    local ip=$(terraform output -raw arena_public_ip 2>/dev/null)
    local key_name=$(grep '^key_name' terraform.tfvars | awk -F'"' '{print $2}')
    local key_file="$HOME/.ssh/${key_name}.pem"

    if [ -z "$ip" ]; then
        err "No instance found."
        exit 1
    fi

    log "Running E2E verification on $ip..."

    ssh -i "$key_file" -o StrictHostKeyChecking=no "ubuntu@${ip}" << 'REMOTE_VERIFY'
set -e
echo "=== IICPC Production Verification ==="
echo ""

# 1. System checks
echo "[1/6] System checks..."
echo "  Instance: $(curl -s http://169.254.169.254/latest/meta-data/instance-type 2>/dev/null || echo 'unknown')"
echo "  Kernel:   $(uname -r)"
echo "  CPUs:     $(nproc)"
echo "  Memory:   $(free -h | awk '/Mem:/{print $2}')"
echo "  Huge:     $(cat /proc/meminfo | grep HugePages_Total | awk '{print $2}') pages"

# 2. C++ build
echo ""
echo "[2/6] C++ engine..."
if [ -f /opt/iicpc/build/bin/worker_agent ]; then
    echo "  ✓ worker_agent binary found"
else
    echo "  ✗ worker_agent not found"
fi

# 3. Docker services
echo ""
echo "[3/6] Docker services..."
for svc in redis questdb redpanda; do
    if docker ps --format '{{.Names}}' 2>/dev/null | grep -qi "$svc"; then
        echo "  ✓ $svc running"
    else
        echo "  ✗ $svc not running"
    fi
done

# 4. NGINX
echo ""
echo "[4/6] NGINX..."
if systemctl is-active --quiet nginx; then
    echo "  ✓ NGINX running"
else
    echo "  ✗ NGINX not running"
fi

# 5. Firecracker
echo ""
echo "[5/6] Firecracker..."
if command -v firecracker &>/dev/null; then
    echo "  ✓ Firecracker $(firecracker --version 2>&1 | head -1)"
else
    echo "  ✗ Firecracker not installed"
fi
if [ -f /opt/firecracker/vmlinux.bin ]; then
    echo "  ✓ Kernel image present"
else
    echo "  ✗ Kernel image missing"
fi

# 6. Quick benchmark
echo ""
echo "[6/6] Quick latency check..."
if [ -f /opt/iicpc/build/bin/bench_ultra ]; then
    timeout 10 /opt/iicpc/build/bin/bench_ultra --iterations 10000 2>&1 | tail -5 || echo "  Benchmark timeout"
else
    echo "  Skipped (bench_ultra not built)"
fi

echo ""
echo "=== Verification Complete ==="
REMOTE_VERIFY

    ok "Verification complete"
}

cmd_status() {
    cd "$TF_DIR"
    log "Infrastructure status:"
    echo ""

    # Arena
    local ip=$(terraform output -raw arena_public_ip 2>/dev/null || echo "")
    if [ -n "$ip" ] && [ "$ip" != "" ]; then
        ok "Arena instance: $ip"
        if curl -s --connect-timeout 3 "http://${ip}" &>/dev/null; then
            ok "  HTTP: responding"
        else
            warn "  HTTP: not responding"
        fi
    else
        warn "Arena: not deployed"
    fi

    # FPGA
    local fpga_ip=$(terraform output -raw fpga_public_ip 2>/dev/null || echo "N/A (disabled)")
    if [ "$fpga_ip" != "N/A (disabled)" ] && [ -n "$fpga_ip" ]; then
        ok "FPGA instance: $fpga_ip"
    else
        log "FPGA: disabled (set enable_fpga=true to enable)"
    fi

    echo ""
    terraform output 2>/dev/null || true
}

cmd_destroy() {
    cd "$TF_DIR"
    warn "This will DESTROY all AWS infrastructure!"
    read -p "Type 'destroy' to confirm: " confirm
    if [ "$confirm" != "destroy" ]; then
        log "Aborted."
        exit 0
    fi
    terraform destroy -auto-approve
    ok "All infrastructure destroyed"
}

cmd_fpga_on() {
    cd "$TF_DIR"
    sed -i 's/enable_fpga = false/enable_fpga = true/' terraform.tfvars
    ok "FPGA enabled in terraform.tfvars"
    log "Run './deploy.sh apply' to provision FPGA instances"
    warn "f2.2xlarge costs ~\$1.65/hr — destroy when done!"
}

cmd_fpga_off() {
    cd "$TF_DIR"
    sed -i 's/enable_fpga = true/enable_fpga = false/' terraform.tfvars
    ok "FPGA disabled in terraform.tfvars"
    log "Run './deploy.sh apply' to destroy FPGA instances"
}

# Main
case "${1:-help}" in
    init)     cmd_init ;;
    plan)     cmd_plan ;;
    apply)    cmd_apply ;;
    sync)     cmd_sync ;;
    verify)   cmd_verify ;;
    status)   cmd_status ;;
    destroy)  cmd_destroy ;;
    fpga-on)  cmd_fpga_on ;;
    fpga-off) cmd_fpga_off ;;
    *)
        echo "IICPC Deployment Tool"
        echo ""
        echo "Usage: $0 <command>"
        echo ""
        echo "Commands:"
        echo "  init       Initialize Terraform (first time)"
        echo "  plan       Preview infrastructure changes"
        echo "  apply      Deploy to AWS"
        echo "  sync       Push project files to instance"
        echo "  verify     Run E2E checks on deployed instance"
        echo "  status     Check infrastructure status"
        echo "  destroy    Tear down everything"
        echo "  fpga-on    Enable FPGA instances"
        echo "  fpga-off   Disable FPGA instances"
        ;;
esac
