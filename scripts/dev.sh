#!/bin/bash
# dev.sh — Start full stack locally (FastAPI + SvelteKit)
set -e

IICPC_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

echo "  IICPC Arena — Dev Environment"

# Check Redis
if ! redis-cli ping &>/dev/null; then
    echo "[!] Redis not running. Starting Docker services..."
    cd "$IICPC_ROOT/infra/docker" && docker compose up -d
    sleep 2
fi

# Create Python venv if needed
VENV="$IICPC_ROOT/web/backend/.venv"
if [ ! -d "$VENV" ]; then
    echo "[*] Creating Python venv..."
    python3 -m venv "$VENV"
    source "$VENV/bin/activate"
    pip install -q -r "$IICPC_ROOT/web/backend/requirements.txt"
else
    source "$VENV/bin/activate"
fi

# Start FastAPI in background
echo "[*] Starting FastAPI on :8000..."
cd "$IICPC_ROOT/web/backend"
uvicorn main:app --host 127.0.0.1 --port 8000 --reload &
API_PID=$!

# Start SvelteKit
echo "[*] Starting SvelteKit on :5173..."
cd "$IICPC_ROOT/web/frontend"
npm run dev -- --port 5173 --host &
UI_PID=$!

echo ""
echo "  → Frontend:  http://localhost:5173"
echo "  → API:       http://localhost:8000/api/health"
echo "  → Press Ctrl+C to stop all"
echo ""

# Cleanup on exit
trap "kill $API_PID $UI_PID 2>/dev/null; exit" INT TERM
wait
