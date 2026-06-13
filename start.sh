#!/bin/bash
# start.sh — IICPC Arena: One-Command Full Stack Launch
# Starts Redis, FastAPI backend, and SvelteKit frontend.
# Usage: bash start.sh
set -e

IICPC_ROOT="$(cd "$(dirname "$0")" && pwd)"
PIDS=()

cleanup() {
    echo ""
    echo "--- Shutting down IICPC Arena ---"
    
    for pid in "${PIDS[@]}"; do
        kill "$pid" 2>/dev/null || true
    done
    # Stop Redis if we started it
    if [ "$STARTED_REDIS" = "1" ]; then
        redis-cli shutdown nosave 2>/dev/null || true
        echo "[✓] Redis stopped"
    fi
    exit 0
}
trap cleanup INT TERM

echo "--- Shutting down IICPC Arena ---"
echo "--- IICPC Arena: Full Stack Launcher ---"

STARTED_REDIS=0

# ─── Step 1: Redis ───────────────────────────────────────────────────────────
echo "━━━ [1/4] Redis ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
if redis-cli ping &>/dev/null; then
    echo "[✓] Redis already running"
else
    if command -v redis-server &>/dev/null; then
        echo "[*] Starting Redis..."
        redis-server --daemonize yes --loglevel warning --save "" --appendonly no
        STARTED_REDIS=1
        sleep 1
        if redis-cli ping &>/dev/null; then
            echo "[✓] Redis started"
        else
            echo "[✗] Redis failed to start. Install with: sudo apt install redis-server"
            exit 1
        fi
    else
        echo "[✗] redis-server not found. Install with: sudo apt install redis-server"
        exit 1
    fi
fi

# ─── Step 2: C++ Engine (optional — build if CMake present) ──────────────────
echo "━━━ [2/4] C++ Engine ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
if [ -f "$IICPC_ROOT/build/run_contest" ]; then
    echo "[✓] Engine binary found: build/run_contest"
elif [ -f "$IICPC_ROOT/CMakeLists.txt" ] && command -v cmake &>/dev/null; then
    echo "[*] Building C++ engine..."
    mkdir -p "$IICPC_ROOT/build"
    cd "$IICPC_ROOT/build"
    cmake -DCMAKE_BUILD_TYPE=Release .. 2>/dev/null && make -j$(nproc) 2>/dev/null
    if [ -f "$IICPC_ROOT/build/run_contest" ]; then
        echo "[✓] Engine built successfully"
    else
        echo "[~] Engine build skipped (non-critical for web stack)"
    fi
    cd "$IICPC_ROOT"
else
    echo "[~] No engine binary, build skipped (web stack will still work)"
fi

# ─── Step 3: FastAPI Backend ─────────────────────────────────────────────────
echo "━━━ [3/4] FastAPI Backend ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
VENV="$IICPC_ROOT/web/backend/.venv"
if [ ! -d "$VENV" ]; then
    echo "[*] Creating Python venv..."
    python3 -m venv "$VENV"
fi
source "$VENV/bin/activate"

# Install deps quietly
echo "[*] Installing Python dependencies..."
pip install -q -r "$IICPC_ROOT/web/backend/requirements.txt" 2>/dev/null

echo "[*] Starting FastAPI on :8000..."
cd "$IICPC_ROOT/web/backend"
uvicorn main:app --host 127.0.0.1 --port 8000 --reload --log-level info &
PIDS+=($!)
sleep 2

# Health check
if curl -sf http://127.0.0.1:8000/api/health > /dev/null 2>&1; then
    echo "[✓] Backend healthy"
else
    echo "[~] Backend starting (may take a moment)..."
fi

# ─── Step 4: SvelteKit Frontend ──────────────────────────────────────────────
echo "━━━ [4/4] SvelteKit Frontend ━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
cd "$IICPC_ROOT/web/frontend"
if [ ! -d "node_modules" ]; then
    echo "[*] Installing npm dependencies..."
    npm install --silent 2>/dev/null
fi

echo "[*] Starting SvelteKit on :5173..."
npm run dev -- --port 5173 --host &
PIDS+=($!)

echo "--- Shutting down IICPC Arena ---"
echo "  IICPC Arena is running."
echo "  Frontend:    http://localhost:5173                     "
echo "  Backend API: http://localhost:8000/api/health          "
echo "  API Docs:    http://localhost:8000/docs                "
echo "    Press Ctrl+C to stop all services."

wait
