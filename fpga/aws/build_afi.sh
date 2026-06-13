#!/bin/bash
# build_afi.sh — Build Amazon FPGA Image from SystemVerilog
# Runs on an AWS m5.4xlarge (NOT f2 — cheaper for synthesis)
#
# Prerequisites:
#   - AWS FPGA HDK cloned: git clone https://github.com/aws/aws-fpga.git
#   - Vivado 2023.2+ installed (FPGA Developer AMI has it)
#   - AWS CLI configured
#
# Steps:
#   1. Synthesize + Place & Route (2-4 hours)
#   2. Generate DCP (Design Checkpoint)
#   3. Submit to AWS for AFI registration (~30-60 min)
#   4. Returns afi-id for loading onto f2 instance
#
# Usage: ./build_afi.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FPGA_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
RTL_DIR="$FPGA_ROOT/rtl"

# AWS FPGA HDK path (clone if not present)
HDK_DIR="${AWS_FPGA_HDK:-/home/centos/aws-fpga/hdk}"

# S3 bucket for AFI artifacts (CHANGE THIS)
S3_BUCKET="${AFI_S3_BUCKET:-iicpc-fpga-artifacts}"
S3_PREFIX="sequencer"

# Output
BUILD_DIR="$FPGA_ROOT/build/aws_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$BUILD_DIR"

echo "=== IICPC FPGA AFI Builder ==="
echo "  RTL:      $RTL_DIR"
echo "  HDK:      $HDK_DIR"
echo "  Output:   $BUILD_DIR"
echo ""

# Step 1: Set up HDK environment
echo "[1/4] Setting up AWS FPGA HDK..."
if [[ -f "$HDK_DIR/../hdk_setup.sh" ]]; then
    source "$HDK_DIR/../hdk_setup.sh" 2>/dev/null || true
else
    echo "WARNING: HDK setup script not found. Ensure Vivado is in PATH."
fi

# Step 2: Create Vivado project and run synthesis
echo "[2/4] Running Vivado synthesis (this takes 2-4 hours)..."

cat > "$BUILD_DIR/synth.tcl" << 'TCL_EOF'
# Vivado synthesis script for IICPC sequencer
create_project iicpc_sequencer ./vivado_project -part xcvu9p-flgb2104-2-i -force

# Add RTL sources
set rtl_dir [lindex $argv 0]
add_files -fileset sources_1 [glob $rtl_dir/*.sv]

# Set top module
set_property top sequencer_top [current_fileset]

# Constraints
create_clock -period 4.000 -name clk_250mhz [get_ports clk_250mhz]

# Synthesis
synth_design -top sequencer_top -flatten_hierarchy rebuilt
opt_design
place_design
route_design

# Reports
report_timing_summary -file timing_summary.rpt
report_utilization -file utilization.rpt
report_power -file power.rpt

# Generate DCP
write_checkpoint -force design.dcp

puts "=== Synthesis Complete ==="
puts "  Timing: timing_summary.rpt"
puts "  Utilization: utilization.rpt"
puts "  DCP: design.dcp"
TCL_EOF

cd "$BUILD_DIR"
vivado -mode batch -source synth.tcl -tclargs "$RTL_DIR" 2>&1 | tee synth.log

# Step 3: Create tarball for AFI submission
echo "[3/4] Packaging DCP for AFI submission..."

tar -czf "$BUILD_DIR/sequencer.tar.gz" -C "$BUILD_DIR" design.dcp

# Upload to S3
aws s3 cp "$BUILD_DIR/sequencer.tar.gz" \
    "s3://${S3_BUCKET}/${S3_PREFIX}/sequencer.tar.gz"

# Step 4: Submit AFI creation request
echo "[4/4] Submitting AFI creation request..."

AFI_RESULT=$(aws ec2 create-fpga-image \
    --name "iicpc-sequencer-$(date +%Y%m%d)" \
    --description "IICPC Order Sequencer - SystemVerilog" \
    --input-storage-location "Bucket=${S3_BUCKET},Key=${S3_PREFIX}/sequencer.tar.gz" \
    --logs-storage-location "Bucket=${S3_BUCKET},Key=${S3_PREFIX}/logs/" \
    --output text)

AFI_ID=$(echo "$AFI_RESULT" | awk '{print $1}')
AGFI_ID=$(echo "$AFI_RESULT" | awk '{print $2}')

echo ""
echo "--- AFI Submission Complete ---"
echo "  AFI Submission Complete                           "
echo "  AFI ID:  $AFI_ID"
echo "  AGFI ID: $AGFI_ID"
echo "                                                    "
echo "  Check status:                                     "
echo "  aws ec2 describe-fpga-images --fpga-image-ids $AFI_ID"
echo "                                                    "
echo "  Load onto F2 instance:                            "
echo "  sudo fpga-load-local-image -S 0 -I $AGFI_ID"

# Save IDs
echo "$AFI_ID" > "$BUILD_DIR/afi_id.txt"
echo "$AGFI_ID" > "$BUILD_DIR/agfi_id.txt"

echo "Done. AFI generation takes ~30-60 minutes."
echo "Build artifacts: $BUILD_DIR"
