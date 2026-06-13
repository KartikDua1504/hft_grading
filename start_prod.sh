#!/usr/bin/env bash
# IICPC Arena — Production Launcher
# Builds frontend and starts all services with process management.
# Usage:  bash start_prod.sh
set -e

PROJECT_ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$PROJECT_ROOT"

echo "--- IICPC Arena: Production Launcher ---"


# Cleanup on exit
cleanup() {
    echo ""
    echo "[*] Shutting down..."
    kill $BACKEND_PID $FRONTEND_PID 2>/dev/null
    redis-cli shutdown nosave 2>/dev/null || true
    echo "[✓] All processes stopped"
}
trap cleanup EXIT

# ━━━ [1/5] Redis ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
echo "━━━ [1/5] Redis ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
if redis-cli ping &>/dev/null; then
    echo "[✓] Redis already running"
else
    echo "[*] Starting Redis..."
    redis-server --daemonize yes --save "" --appendonly no
    echo "[✓] Redis started"
fi

# ━━━ [2/5] C++ Engine ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
echo "━━━ [2/5] C++ Engine ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
if [ -f "build/run_contest" ]; then
    echo "[✓] Engine binary found: build/run_contest"
else
    echo "[!] Engine binary not found. Building..."
    cmake -B build -DCMAKE_BUILD_TYPE=Release 2>/dev/null
    cmake --build build --target run_contest -j$(nproc)
    echo "[✓] Engine built"
fi

# ━━━ [3/5] Frontend Build ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
echo "━━━ [3/5] Frontend Build ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
cd web/frontend
if [ ! -d "node_modules" ]; then
    npm install
fi
echo "[*] Building production frontend..."
npm run build 2>&1 | tail -5
echo "[✓] Frontend built to build/"
cd "$PROJECT_ROOT"

# ━━━ [4/5] FastAPI Backend ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
echo "━━━ [4/5] FastAPI Backend ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
cd web/backend
pip install -q fastapi uvicorn python-multipart aiofiles redis pyjwt 2>/dev/null
echo "[*] Starting FastAPI (production mode)..."
uvicorn main:app --host 0.0.0.0 --port 8000 --workers 2 &
BACKEND_PID=$!
cd "$PROJECT_ROOT"
sleep 2
if curl -sf http://127.0.0.1:8000/api/health >/dev/null; then
    echo "[✓] Backend healthy (2 workers)"
else
    echo "[!] Backend may still be starting..."
fi

# ━━━ [5/5] SvelteKit Node Server ━━━━━━━━━━━━━━━━━━━━━━━━
echo "━━━ [5/5] SvelteKit Node Server ━━━━━━━━━━━━━━━━━━━━━━━━"
cd web/frontend
echo "[*] Starting SvelteKit on :3000..."
PORT=3000 ORIGIN=http://localhost:3000 node build &
FRONTEND_PID=$!
cd "$PROJECT_ROOT"
sleep 1

echo "--- IICPC Arena: Production Launcher ---"
echo "  IICPC Arena: Production Mode"
echo "  Frontend:    http://localhost:3000                     "
echo "  Backend API: http://localhost:8000/api/health          "
echo "  Admin Panel: http://localhost:3000/admin               "
echo "  For Nginx: sudo cp infra/nginx/iicpc.conf             "
echo "             /etc/nginx/sites-enabled/                   "
echo "  Then:      sudo nginx -t && sudo systemctl reload nginx"
echo "  Admin password: \$ADMIN_PASSWORD (default: iicpc-admin-2026)"
echo "    Press Ctrl+C to stop all services."

# Wait for signals
wait
