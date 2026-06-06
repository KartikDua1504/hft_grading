# =============================================================================
# IICPC Terraform Variables
# =============================================================================
# Edit this file before running `terraform apply`
# =============================================================================

aws_region       = "us-east-1"
key_name         = "iicpc-arena-key"    # Must exist in your AWS account
allowed_ssh_cidr = "0.0.0.0/0"         # CHANGE to your IP: "x.x.x.x/32"
instance_type    = "c8i.metal-48xl"     # Bare metal for Firecracker
project_name     = "iicpc"

# FPGA — disabled by default (expensive)
# Set to true only when you're ready to test on real FPGA hardware
enable_fpga = false
