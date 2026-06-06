# =============================================================================
# fpga.tf — AWS FPGA Infrastructure (Optional — controlled by flag)
# =============================================================================
# Set enable_fpga = true in terraform.tfvars to provision FPGA instances.
# Default: false (avoids expensive instances during normal deployment).
#
# Cost: f2.2xlarge = ~$1.65/hr, m5.4xlarge = ~$0.77/hr
#       TERMINATE WHEN DONE!
# =============================================================================

variable "enable_fpga" {
  description = "Enable FPGA infrastructure (expensive — set true only when needed)"
  type        = bool
  default     = false
}

variable "fpga_instance_type" {
  description = "FPGA instance type"
  type        = string
  default     = "f2.2xlarge"
}

# Data source: latest FPGA Developer AMI
data "aws_ami" "fpga_dev" {
  count       = var.enable_fpga ? 1 : 0
  most_recent = true
  owners      = ["amazon"]

  filter {
    name   = "name"
    values = ["FPGA Developer AMI*"]
  }

  filter {
    name   = "architecture"
    values = ["x86_64"]
  }
}

# Security group for FPGA instance (reuses VPC from main.tf)
resource "aws_security_group" "fpga_sg" {
  count       = var.enable_fpga ? 1 : 0
  name_prefix = "${var.project_name}-fpga-"
  vpc_id      = aws_vpc.main.id

  ingress {
    from_port   = 22
    to_port     = 22
    protocol    = "tcp"
    cidr_blocks = [var.allowed_ssh_cidr]
    description = "SSH"
  }

  egress {
    from_port   = 0
    to_port     = 0
    protocol    = "-1"
    cidr_blocks = ["0.0.0.0/0"]
  }

  tags = { Name = "${var.project_name}-fpga-sg" }
}

# F2 instance — FPGA sequencer/matching engine testing
resource "aws_instance" "fpga_sequencer" {
  count         = var.enable_fpga ? 1 : 0
  ami           = data.aws_ami.fpga_dev[0].id
  instance_type = var.fpga_instance_type
  key_name      = var.key_name
  subnet_id     = aws_subnet.public.id

  vpc_security_group_ids = [aws_security_group.fpga_sg[0].id]

  root_block_device {
    volume_size = 100
    volume_type = "gp3"
  }

  user_data = <<-EOF
    #!/bin/bash
    set -e
    cd /home/centos
    [ -d aws-fpga ] || git clone https://github.com/aws/aws-fpga.git
    source aws-fpga/hdk_setup.sh
    yum install -y verilator 2>/dev/null || true
    echo "FPGA dev environment ready" > /tmp/fpga_setup_done
  EOF

  tags = {
    Name        = "${var.project_name}-fpga-sequencer"
    Purpose     = "FPGA order sequencer + matching engine"
    AutoStop    = "true"
    Environment = "dev"
  }
}

# Synthesis instance (cheaper than F2 — use for Vivado compilation)
resource "aws_instance" "fpga_build" {
  count         = var.enable_fpga ? 1 : 0
  ami           = data.aws_ami.fpga_dev[0].id
  instance_type = "m5.4xlarge"
  key_name      = var.key_name
  subnet_id     = aws_subnet.public.id

  vpc_security_group_ids = [aws_security_group.fpga_sg[0].id]

  root_block_device {
    volume_size = 200
    volume_type = "gp3"
  }

  user_data = <<-EOF
    #!/bin/bash
    cd /home/centos
    git clone https://github.com/aws/aws-fpga.git 2>/dev/null || true
    source aws-fpga/hdk_setup.sh
  EOF

  tags = {
    Name    = "${var.project_name}-fpga-build"
    Purpose = "FPGA synthesis (Vivado)"
  }
}

# =============================================================================
# Outputs (conditional)
# =============================================================================
output "fpga_instance_id" {
  description = "FPGA test instance ID"
  value       = var.enable_fpga ? aws_instance.fpga_sequencer[0].id : "N/A (disabled)"
}

output "fpga_public_ip" {
  description = "FPGA test instance public IP"
  value       = var.enable_fpga ? aws_instance.fpga_sequencer[0].public_ip : "N/A (disabled)"
}

output "fpga_build_instance_id" {
  description = "FPGA build instance ID"
  value       = var.enable_fpga ? aws_instance.fpga_build[0].id : "N/A (disabled)"
}

output "fpga_build_public_ip" {
  description = "FPGA build instance public IP"
  value       = var.enable_fpga ? aws_instance.fpga_build[0].public_ip : "N/A (disabled)"
}
