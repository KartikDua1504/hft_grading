# =============================================================================
# IICPC Competition Platform — Terraform Infrastructure
# =============================================================================
# One command deployment: `terraform apply`
#
# Provisions:
#   - c8i.metal-48xl bare-metal instance (hardware virt for Firecracker)
#   - VPC + Security Groups
#   - Automated bootstrap: Docker, Redis, QuestDB, Redpanda, C++ engine
#   - Alpine rootfs + Firecracker kernel download
#   - NGINX + FastAPI + SvelteKit deployment
#
# Prerequisites:
#   - AWS credentials configured (~/.aws/credentials or env vars)
#   - SSH key pair created in AWS console
# =============================================================================

terraform {
  required_version = ">= 1.5.0"
  required_providers {
    aws = {
      source  = "hashicorp/aws"
      version = "~> 5.0"
    }
  }
}

provider "aws" {
  region = var.aws_region
}

# =============================================================================
# Variables
# =============================================================================
variable "aws_region" {
  description = "AWS region to deploy in"
  type        = string
  default     = "us-east-1"
}

variable "key_name" {
  description = "Name of the SSH key pair in AWS"
  type        = string
}

variable "allowed_ssh_cidr" {
  description = "CIDR block allowed to SSH (your IP)"
  type        = string
  default     = "0.0.0.0/0"
}

variable "instance_type" {
  description = "EC2 instance type (must support hardware virtualization)"
  type        = string
  default     = "c8i.metal-48xl"
}

variable "project_name" {
  description = "Project name for resource tagging"
  type        = string
  default     = "iicpc"
}

# =============================================================================
# VPC + Networking
# =============================================================================
resource "aws_vpc" "main" {
  cidr_block           = "10.0.0.0/16"
  enable_dns_support   = true
  enable_dns_hostnames = true

  tags = {
    Name    = "${var.project_name}-vpc"
    Project = var.project_name
  }
}

resource "aws_internet_gateway" "gw" {
  vpc_id = aws_vpc.main.id
  tags   = { Name = "${var.project_name}-igw" }
}

resource "aws_subnet" "public" {
  vpc_id                  = aws_vpc.main.id
  cidr_block              = "10.0.1.0/24"
  availability_zone       = "${var.aws_region}a"
  map_public_ip_on_launch = true
  tags                    = { Name = "${var.project_name}-public" }
}

resource "aws_route_table" "public" {
  vpc_id = aws_vpc.main.id
  route {
    cidr_block = "0.0.0.0/0"
    gateway_id = aws_internet_gateway.gw.id
  }
  tags = { Name = "${var.project_name}-rt" }
}

resource "aws_route_table_association" "public" {
  subnet_id      = aws_subnet.public.id
  route_table_id = aws_route_table.public.id
}

# =============================================================================
# Security Group
# =============================================================================
resource "aws_security_group" "arena" {
  name        = "${var.project_name}-arena-sg"
  description = "IICPC Arena: SSH + HTTP + HTTPS + WebSocket"
  vpc_id      = aws_vpc.main.id

  # SSH
  ingress {
    from_port   = 22
    to_port     = 22
    protocol    = "tcp"
    cidr_blocks = [var.allowed_ssh_cidr]
    description = "SSH access"
  }

  # HTTP
  ingress {
    from_port   = 80
    to_port     = 80
    protocol    = "tcp"
    cidr_blocks = ["0.0.0.0/0"]
    description = "HTTP"
  }

  # HTTPS
  ingress {
    from_port   = 443
    to_port     = 443
    protocol    = "tcp"
    cidr_blocks = ["0.0.0.0/0"]
    description = "HTTPS"
  }

  # QuestDB console (development only)
  ingress {
    from_port   = 9000
    to_port     = 9000
    protocol    = "tcp"
    cidr_blocks = [var.allowed_ssh_cidr]
    description = "QuestDB console"
  }

  # All outbound
  egress {
    from_port   = 0
    to_port     = 0
    protocol    = "-1"
    cidr_blocks = ["0.0.0.0/0"]
  }

  tags = { Name = "${var.project_name}-sg" }
}

# =============================================================================
# EC2 Instance — Bare Metal (c8i.metal-48xl)
# =============================================================================
resource "aws_instance" "arena" {
  ami                    = data.aws_ami.ubuntu.id
  instance_type          = var.instance_type
  key_name               = var.key_name
  subnet_id              = aws_subnet.public.id
  vpc_security_group_ids = [aws_security_group.arena.id]

  # 500GB gp3 root volume (fast SSD)
  root_block_device {
    volume_size           = 500
    volume_type           = "gp3"
    iops                  = 16000
    throughput            = 1000
    delete_on_termination = true
  }

  # User data: full bootstrap script
  user_data = file("${path.module}/bootstrap.sh")

  tags = {
    Name    = "${var.project_name}-arena"
    Project = var.project_name
    Role    = "competition-arena"
  }
}

# =============================================================================
# AMI Lookup — Latest Ubuntu 24.04 LTS
# =============================================================================
data "aws_ami" "ubuntu" {
  most_recent = true
  owners      = ["099720109477"] # Canonical

  filter {
    name   = "name"
    values = ["ubuntu/images/hvm-ssd-gp3/ubuntu-noble-24.04-amd64-server-*"]
  }

  filter {
    name   = "virtualization-type"
    values = ["hvm"]
  }

  filter {
    name   = "architecture"
    values = ["x86_64"]
  }
}

# =============================================================================
# Elastic IP (stable public IP)
# =============================================================================
resource "aws_eip" "arena" {
  instance = aws_instance.arena.id
  domain   = "vpc"
  tags     = { Name = "${var.project_name}-eip" }
}

# =============================================================================
# Outputs
# =============================================================================
output "arena_public_ip" {
  description = "Public IP of the arena server"
  value       = aws_eip.arena.public_ip
}

output "arena_ssh" {
  description = "SSH command to connect"
  value       = "ssh -i ~/.ssh/${var.key_name}.pem ubuntu@${aws_eip.arena.public_ip}"
}

output "dashboard_url" {
  description = "Competition dashboard URL"
  value       = "http://${aws_eip.arena.public_ip}"
}

output "questdb_url" {
  description = "QuestDB console URL"
  value       = "http://${aws_eip.arena.public_ip}:9000"
}
