#!/bin/bash
# bootstrap.sh — Fully Automated Arena Provisioning
# Runs as EC2 user-data on first boot. Zero manual intervention.
# After this completes, the arena is ready to receive contestant code.
#
# Installs: Docker, Docker Compose, GCC 14, CMake, Python 3.12, Node 22,
#           Firecracker, NGINX, and builds the entire C++ engine.

set -euo pipefail
exec > /var/log/iicpc-bootstrap.log 2>&1
echo "=== IICPC Bootstrap started at $(date) ==="

export DEBIAN_FRONTEND=noninteractive
IICPC_HOME="/opt/iicpc"

# 1. System packages
echo "[1/10] Installing system packages..."
apt-get update -qq
apt-get install -y -qq \
    build-essential gcc-14 g++-14 cmake ninja-build \
    git curl wget unzip jq htop \
    python3.12 python3.12-venv python3-pip \
    nginx certbot python3-certbot-nginx \
    linux-tools-common linux-tools-generic \
    numactl cpufrequtils \
    ca-certificates gnupg lsb-release

# Set GCC 14 as default
update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-14 100
update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-14 100

# 2. Docker
echo "[2/10] Installing Docker..."
install -m 0755 -d /etc/apt/keyrings
curl -fsSL https://download.docker.com/linux/ubuntu/gpg | \
    gpg --dearmor -o /etc/apt/keyrings/docker.gpg
echo "deb [arch=amd64 signed-by=/etc/apt/keyrings/docker.gpg] \
    https://download.docker.com/linux/ubuntu noble stable" > \
    /etc/apt/sources.list.d/docker.list
apt-get update -qq
apt-get install -y -qq docker-ce docker-ce-cli containerd.io \
    docker-buildx-plugin docker-compose-plugin
usermod -aG docker ubuntu

# 3. Node.js 22 (for SvelteKit frontend)
echo "[3/10] Installing Node.js 22..."
curl -fsSL https://deb.nodesource.com/setup_22.x | bash -
apt-get install -y -qq nodejs

# 4. Performance tuning (bare metal)
echo "[4/10] Configuring performance mode..."

# Hugepages: 2GB reserved (1024 x 2MB pages)
sysctl -w vm.nr_hugepages=1024
echo 'vm.nr_hugepages=1024' >> /etc/sysctl.conf

# CPU performance governor
cpupower frequency-set -g performance 2>/dev/null || true

# Disable THP compaction (reduces latency spikes)
echo madvise > /sys/kernel/mm/transparent_hugepage/enabled
echo madvise > /sys/kernel/mm/transparent_hugepage/defrag

# Max socket buffers
sysctl -w net.core.rmem_max=16777216
sysctl -w net.core.wmem_max=16777216
sysctl -w net.core.rmem_default=1048576
sysctl -w net.core.wmem_default=1048576

# Isolate CPU cores 4-15 from OS scheduler (for deterministic benchmarking)
# Cores 4-7: Order Blaster, 8-11: Contestant, 12-15: Shadow Orderbook
GRUB_FILE="/etc/default/grub"
if ! grep -q "isolcpus" "$GRUB_FILE"; then
    sed -i 's/GRUB_CMDLINE_LINUX_DEFAULT="\(.*\)"/GRUB_CMDLINE_LINUX_DEFAULT="\1 isolcpus=4-15 nohz_full=4-15 rcu_nocbs=4-15"/' "$GRUB_FILE"
    update-grub
    echo "NOTE: isolcpus configured. Reboot required for effect."
fi

# 5. Firecracker
echo "[5/10] Installing Firecracker..."
FC_VERSION="1.7.0"
FC_ARCH="x86_64"
curl -fsSL -o /tmp/firecracker.tgz \
    "https://github.com/firecracker-microvm/firecracker/releases/download/v${FC_VERSION}/firecracker-v${FC_VERSION}-${FC_ARCH}.tgz"
tar xzf /tmp/firecracker.tgz -C /tmp
cp /tmp/release-v${FC_VERSION}-${FC_ARCH}/firecracker-v${FC_VERSION}-${FC_ARCH} /usr/local/bin/firecracker
cp /tmp/release-v${FC_VERSION}-${FC_ARCH}/jailer-v${FC_VERSION}-${FC_ARCH} /usr/local/bin/jailer
chmod +x /usr/local/bin/firecracker /usr/local/bin/jailer

# Download minimal kernel for Firecracker
mkdir -p /opt/firecracker
curl -fsSL -o /opt/firecracker/vmlinux.bin \
    "https://s3.amazonaws.com/spec.ccfc.min/ci-artifacts/kernels/${FC_ARCH}/vmlinux-5.10.217" || \
    echo "WARNING: Firecracker kernel download failed (manual download needed)"

# 6. Clone and build the C++ engine
echo "[6/10] Building IICPC C++ engine..."
mkdir -p ${IICPC_HOME}

# Copy project files (in production, this would be git clone)
# For now, assume files are synced via rsync/scp after terraform apply
cp -r /home/ubuntu/IICPC/* ${IICPC_HOME}/ 2>/dev/null || \
    echo "NOTE: Project files not yet synced. Run: rsync -avz ./ ubuntu@<IP>:~/IICPC/"

# Build if source exists
if [ -f "${IICPC_HOME}/CMakeLists.txt" ]; then
    mkdir -p ${IICPC_HOME}/build
    cd ${IICPC_HOME}/build
    cmake .. -DCMAKE_BUILD_TYPE=Release -DIICPC_USE_URING=OFF -G Ninja
    ninja -j$(nproc)
    echo "C++ engine built successfully"
else
    echo "C++ source not found yet — will build after sync"
fi

# 7. Build Alpine rootfs for contestants
echo "[7/10] Building Alpine rootfs..."
if [ -f "${IICPC_HOME}/scripts/build_rootfs.sh" ]; then
    chmod +x ${IICPC_HOME}/scripts/build_rootfs.sh
    bash ${IICPC_HOME}/scripts/build_rootfs.sh /opt/firecracker/contestant_rootfs.ext4
fi

# 8. Start Docker services (Redis, QuestDB, Redpanda)
echo "[8/10] Starting Docker services..."
if [ -f "${IICPC_HOME}/infra/docker/docker-compose.yml" ]; then
    cd ${IICPC_HOME}/infra/docker
    docker compose up -d
fi

# 9. Python backend
echo "[9/10] Setting up Python backend..."
python3.12 -m venv /opt/iicpc-venv
source /opt/iicpc-venv/bin/activate
pip install --quiet \
    fastapi uvicorn[standard] gunicorn \
    redis aiofiles python-multipart \
    pyjwt passlib[bcrypt] httpx \
    websockets

# 10. NGINX configuration
echo "[10/10] Configuring NGINX..."
cat > /etc/nginx/sites-available/iicpc << 'NGINX_CONF'
upstream api_backend {
    server 127.0.0.1:8000;
}

server {
    listen 80;
    server_name _;

    client_max_body_size 50M;

    # Brotli/gzip compression
    gzip on;
    gzip_types text/plain text/css application/json application/javascript;
    gzip_min_length 256;

    # Static frontend (SvelteKit build output)
    location / {
        root /opt/iicpc/web/frontend/build;
        try_files $uri $uri/ /index.html;

        # Cache static assets aggressively
        location ~* \.(js|css|png|jpg|ico|svg|woff2)$ {
            expires 30d;
            add_header Cache-Control "public, immutable";
        }
    }

    # API reverse proxy
    location /api/ {
        proxy_pass http://api_backend/api/;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto $scheme;

        # File upload streaming (don't buffer in NGINX)
        proxy_request_buffering off;
    }

    # WebSocket for live leaderboard
    location /ws/ {
        proxy_pass http://api_backend/ws/;
        proxy_http_version 1.1;
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection "upgrade";
        proxy_set_header Host $host;
        proxy_read_timeout 86400;
    }
}
NGINX_CONF

ln -sf /etc/nginx/sites-available/iicpc /etc/nginx/sites-enabled/
rm -f /etc/nginx/sites-enabled/default
nginx -t && systemctl restart nginx

# Create systemd service for FastAPI
cat > /etc/systemd/system/iicpc-api.service << 'SERVICE'
[Unit]
Description=IICPC Competition API
After=network.target docker.service

[Service]
Type=simple
User=ubuntu
WorkingDirectory=/opt/iicpc/web/backend
Environment="PATH=/opt/iicpc-venv/bin:/usr/bin"
ExecStart=/opt/iicpc-venv/bin/gunicorn main:app \
    --worker-class uvicorn.workers.UvicornWorker \
    --workers 4 \
    --bind 127.0.0.1:8000 \
    --timeout 300 \
    --access-logfile /var/log/iicpc-api-access.log \
    --error-logfile /var/log/iicpc-api-error.log
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
SERVICE

systemctl daemon-reload
# Don't start yet — backend code needs to be synced first
# systemctl enable --now iicpc-api

echo ""
echo "============================================"
echo "  IICPC Arena Bootstrap COMPLETE"
echo "  $(date)"
echo "============================================"
echo "  Instance:   $(curl -s http://169.254.169.254/latest/meta-data/instance-type)"
echo "  Public IP:  $(curl -s http://169.254.169.254/latest/meta-data/public-ipv4)"
echo "  Hugepages:  $(cat /proc/meminfo | grep HugePages_Total)"
echo "  Docker:     $(docker --version)"
echo "  GCC:        $(gcc --version | head -1)"
echo "  Node:       $(node --version)"
echo "============================================"
