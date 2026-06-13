# main.py — IICPC Competition API Server
# FastAPI application with:
#   - File upload (streamed to disk, not buffered in RAM)
#   - Redis job queue (LPUSH/RPOP FIFO)
#   - JWT authentication
#   - WebSocket live leaderboard updates
#   - Background worker for contest execution

import asyncio
import hashlib
import json
import logging
import os
import re
import shutil
import time
import uuid
from collections import defaultdict
from contextlib import asynccontextmanager
from datetime import datetime, timedelta, timezone
from pathlib import Path, PurePath
from typing import Optional

import aiofiles
import jwt
import redis.asyncio as aioredis
from fastapi import (
    FastAPI, File, UploadFile, HTTPException,
    Depends, WebSocket, WebSocketDisconnect, Query, Request
)
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse
from pydantic import BaseModel

# Structured logging
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    datefmt="%Y-%m-%d %H:%M:%S",
)
logger = logging.getLogger("iicpc")

# Configuration
REDIS_URL = os.getenv("REDIS_URL", "redis://localhost:6379")
JWT_SECRET = os.getenv("JWT_SECRET", "iicpc-dev-secret-change-in-production")
JWT_ALGORITHM = "HS256"
JWT_EXPIRY_HOURS = 24
UPLOAD_DIR = Path(os.getenv("UPLOAD_DIR", "/tmp/iicpc/uploads"))
RESULTS_DIR = Path(os.getenv("RESULTS_DIR", "/tmp/iicpc/results"))
ENGINE_BINARY = os.getenv("ENGINE_BINARY", "/opt/iicpc/build/run_contest")
MAX_UPLOAD_SIZE = 50 * 1024 * 1024  # 50MB
CHUNK_SIZE = 8192  # Stream uploads in 8KB chunks

# Redpanda/Kafka config
REDPANDA_BROKER = os.getenv("REDPANDA_BROKER", "localhost:19092")
REDPANDA_TOPIC = os.getenv("REDPANDA_TOPIC", "iicpc.results")

# Rate limiting
SUBMISSION_COOLDOWN_SECS = int(os.getenv("SUBMISSION_COOLDOWN", "180"))
MAX_CONCURRENT_JOBS = int(os.getenv("MAX_CONCURRENT_JOBS", "1"))
MAX_QUEUE_DEPTH = int(os.getenv("MAX_QUEUE_DEPTH", "50"))
JOB_RETENTION_HOURS = int(os.getenv("JOB_RETENTION_HOURS", "24"))

# Admin
ADMIN_PASSWORD = os.getenv("ADMIN_PASSWORD", "iicpc-admin-2026")

# Competition state keys (stored in Redis)
COMP_KEY_STATE = "iicpc:competition:state"      # running / stopped / finished
COMP_KEY_END_TIME = "iicpc:competition:end_time"  # ISO timestamp
COMP_KEY_START_TIME = "iicpc:competition:start_time"
COMP_KEY_DURATION = "iicpc:competition:duration_mins"

UPLOAD_DIR.mkdir(parents=True, exist_ok=True)
RESULTS_DIR.mkdir(parents=True, exist_ok=True)

# In-memory rate limiting state
_last_submission: dict[str, float] = {}
_active_jobs: int = 0
_server_start_time = time.time()
_total_submissions = 0
_total_scored = 0
_total_failed = 0

# Redis Keys
QUEUE_KEY = "iicpc:submission_queue"        # FIFO queue (list)
JOB_PREFIX = "iicpc:job:"                   # Per-job state hash
LEADERBOARD_KEY = "iicpc:leaderboard"       # Sorted set (score → team)
LIVE_CHANNEL = "iicpc:live"                 # Pub/sub for WebSocket updates


# Models
class RegisterRequest(BaseModel):
    team_name: str
    password: str

class LoginRequest(BaseModel):
    team_name: str
    password: str

class JobStatus(BaseModel):
    job_id: str
    status: str  # queued, compiling, running, scored, failed
    team_name: str
    submitted_at: str
    score: Optional[float] = None
    throughput: Optional[float] = None
    correctness: Optional[float] = None
    p99_latency_ns: Optional[int] = None
    error: Optional[str] = None

class LeaderboardEntry(BaseModel):
    rank: int
    team_name: str
    score: float
    throughput: float
    correctness: float
    p99_latency_ns: int
    submissions: int
    last_submitted: str


# App Lifecycle
redis_pool: Optional[aioredis.Redis] = None
ws_connections: list[WebSocket] = []

@asynccontextmanager
async def lifespan(app: FastAPI):
    global redis_pool
    try:
        redis_pool = aioredis.from_url(REDIS_URL, decode_responses=True)
        await redis_pool.ping()
        logger.info("Redis connected: %s", REDIS_URL)
    except Exception as e:
        logger.error("Redis connection failed: %s", e)
        raise

    # Start background workers
    worker_task = asyncio.create_task(submission_worker())
    cleanup_task = asyncio.create_task(job_cleanup_worker())
    redpanda_task = asyncio.create_task(redpanda_consumer())
    logger.info("Background workers started (submission + cleanup + redpanda)")
    yield
    worker_task.cancel()
    cleanup_task.cancel()
    redpanda_task.cancel()
    await redis_pool.close()
    logger.info("Shutdown complete")

app = FastAPI(
    title="IICPC Competition Platform",
    description="High-Performance Orderbook Testing Arena",
    version="1.0.0",
    lifespan=lifespan,
)

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)


# JWT Auth
def create_token(team_name: str) -> str:
    payload = {
        "sub": team_name,
        "exp": datetime.now(timezone.utc) + timedelta(hours=JWT_EXPIRY_HOURS),
        "iat": datetime.now(timezone.utc),
    }
    return jwt.encode(payload, JWT_SECRET, algorithm=JWT_ALGORITHM)

def verify_token(token: str) -> str:
    try:
        payload = jwt.decode(token, JWT_SECRET, algorithms=[JWT_ALGORITHM])
        return payload["sub"]
    except jwt.ExpiredSignatureError:
        raise HTTPException(401, "Token expired")
    except jwt.InvalidTokenError:
        raise HTTPException(401, "Invalid token")

async def get_current_team(request: Request, authorization: str = Query(None)) -> str:
    """Extract team from JWT — accepts both Authorization header and query param."""
    token = authorization
    header_auth = request.headers.get("authorization")
    if header_auth:
        token = header_auth
    if not token:
        raise HTTPException(401, "Authorization required")
    if token.startswith("Bearer "):
        token = token[7:]
    return verify_token(token)


# Auth Endpoints
# Rate limiting for auth endpoints (per-IP, sliding window)
_auth_attempts: dict[str, list[float]] = {}
AUTH_RATE_LIMIT = 10       # max attempts
AUTH_RATE_WINDOW = 300.0   # per 5 minutes

def _check_auth_rate_limit(request: Request):
    """Raise 429 if IP exceeds auth rate limit."""
    ip = request.client.host if request.client else "unknown"
    now = time.time()
    attempts = _auth_attempts.get(ip, [])
    # Prune old attempts
    attempts = [t for t in attempts if now - t < AUTH_RATE_WINDOW]
    if len(attempts) >= AUTH_RATE_LIMIT:
        raise HTTPException(429, "Too many auth attempts. Try again later.")
    attempts.append(now)
    _auth_attempts[ip] = attempts
@app.post("/api/auth/register")
async def register(req: RegisterRequest, request: Request):
    _check_auth_rate_limit(request)
    # Input validation
    if not req.team_name or len(req.team_name) < 2 or len(req.team_name) > 32:
        raise HTTPException(400, "Team name must be 2-32 characters")
    if not re.match(r'^[a-zA-Z0-9_\-]+$', req.team_name):
        raise HTTPException(400, "Team name may only contain alphanumeric, underscore, hyphen")
    if not req.password or len(req.password) < 6:
        raise HTTPException(400, "Password must be at least 6 characters")

    key = f"iicpc:team:{req.team_name}"
    exists = await redis_pool.exists(key)
    if exists:
        raise HTTPException(409, "Team name already taken")

    pw_hash = hashlib.sha256(req.password.encode()).hexdigest()
    await redis_pool.hset(key, mapping={
        "password_hash": pw_hash,
        "registered_at": datetime.now(timezone.utc).isoformat(),
        "submissions": "0",
    })
    token = create_token(req.team_name)
    logger.info("Team registered: %s", req.team_name)
    return {"token": token, "team_name": req.team_name}


@app.post("/api/auth/login")
async def login(req: LoginRequest, request: Request):
    _check_auth_rate_limit(request)
    key = f"iicpc:team:{req.team_name}"
    data = await redis_pool.hgetall(key)
    if not data:
        raise HTTPException(401, "Team not found")

    pw_hash = hashlib.sha256(req.password.encode()).hexdigest()
    if data.get("password_hash") != pw_hash:
        raise HTTPException(401, "Invalid password")

    token = create_token(req.team_name)
    return {"token": token, "team_name": req.team_name}


# Submission Endpoint (streamed file upload)
ALLOWED_EXTENSIONS = {".cpp", ".cc", ".cxx", ".c", ".h", ".hpp"}


@app.post("/api/submit")
async def submit_code(
    file: UploadFile = File(...),
    team_name: str = Depends(get_current_team)
):
    global _total_submissions

    # Competition state guard
    comp_state = await redis_pool.get(COMP_KEY_STATE)
    if comp_state and comp_state not in ("running", ""):
        raise HTTPException(403, "Competition is not active. Submissions are closed.")
    comp_end = await redis_pool.get(COMP_KEY_END_TIME)
    if comp_end:
        try:
            end_dt = datetime.fromisoformat(comp_end)
            if datetime.now(timezone.utc) > end_dt:
                await redis_pool.set(COMP_KEY_STATE, "finished")
                raise HTTPException(403, "Competition has ended. Submissions are closed.")
        except (ValueError, TypeError):
            pass

    # Rate limiting: per-team cooldown
    now_ts = time.time()
    last_sub = _last_submission.get(team_name, 0)
    if now_ts - last_sub < SUBMISSION_COOLDOWN_SECS:
        remaining = int(SUBMISSION_COOLDOWN_SECS - (now_ts - last_sub))
        raise HTTPException(
            429,
            f"Submission cooldown: wait {remaining}s before next submission"
        )

    # Queue depth guard
    queue_depth = await redis_pool.llen(QUEUE_KEY)
    if queue_depth >= MAX_QUEUE_DEPTH:
        raise HTTPException(
            503,
            f"Queue full ({queue_depth}/{MAX_QUEUE_DEPTH}). Try again later."
        )

    # Validate file extension
    raw_filename = file.filename or "unknown.cpp"
    # Sanitize filename: strip path components (path traversal protection)
    safe_filename = PurePath(raw_filename).name
    safe_filename = re.sub(r'[^a-zA-Z0-9._\-]', '_', safe_filename)
    ext = PurePath(safe_filename).suffix.lower()
    if ext not in ALLOWED_EXTENSIONS:
        raise HTTPException(
            400,
            f"Invalid file type '{ext}'. Only C/C++ source files accepted "
            f"({', '.join(sorted(ALLOWED_EXTENSIONS))}). "
            f"Please upload your orderbook implementation as a .cpp file."
        )

    # Generate job ID
    job_id = str(uuid.uuid4())
    job_dir = UPLOAD_DIR / job_id
    job_dir.mkdir(parents=True, exist_ok=True)
    source_path = job_dir / "contestant.cpp"

    # Verify path is within UPLOAD_DIR (defense in depth)
    if not source_path.resolve().is_relative_to(UPLOAD_DIR.resolve()):
        shutil.rmtree(job_dir, ignore_errors=True)
        raise HTTPException(400, "Invalid file path")

    # Stream file to disk in chunks (no RAM buffering)
    total_size = 0
    try:
        async with aiofiles.open(source_path, "wb") as f:
            while chunk := await file.read(CHUNK_SIZE):
                total_size += len(chunk)
                if total_size > MAX_UPLOAD_SIZE:
                    raise HTTPException(413, "File too large (max 50MB)")
                await f.write(chunk)
    except HTTPException:
        shutil.rmtree(job_dir, ignore_errors=True)
        raise
    except Exception as e:
        shutil.rmtree(job_dir, ignore_errors=True)
        logger.error("Upload I/O error for %s: %s", team_name, e)
        raise HTTPException(500, "Upload failed")

    # Record submission time for rate limiting
    _last_submission[team_name] = now_ts
    _total_submissions += 1

    # Create job record in Redis
    now = datetime.now(timezone.utc).isoformat()
    await redis_pool.hset(f"{JOB_PREFIX}{job_id}", mapping={
        "status": "queued",
        "team_name": team_name,
        "submitted_at": now,
        "source_path": str(source_path),
        "file_size": str(total_size),
        "filename": safe_filename,
    })

    # Push to queue
    await redis_pool.lpush(QUEUE_KEY, job_id)

    # Increment team submission count
    await redis_pool.hincrby(f"iicpc:team:{team_name}", "submissions", 1)

    # Broadcast queue update
    await broadcast_update({
        "type": "submission",
        "job_id": job_id,
        "team_name": team_name,
        "status": "queued",
    })

    logger.info("Submission queued: job=%s team=%s file=%s size=%d",
                job_id[:8], team_name, safe_filename, total_size)
    return {"job_id": job_id, "status": "queued", "position": queue_depth + 1}


# Job Status
@app.get("/api/job/{job_id}")
async def get_job_status(job_id: str, team_name: str = Depends(get_current_team)):
    data = await redis_pool.hgetall(f"{JOB_PREFIX}{job_id}")
    if not data:
        raise HTTPException(404, "Job not found")
    if data.get("team_name") != team_name:
        raise HTTPException(403, "Not your submission")

    return JobStatus(
        job_id=job_id,
        status=data.get("status", "unknown"),
        team_name=data.get("team_name", ""),
        submitted_at=data.get("submitted_at", ""),
        score=float(data["score"]) if "score" in data else None,
        throughput=float(data["throughput"]) if "throughput" in data else None,
        correctness=float(data["correctness"]) if "correctness" in data else None,
        p99_latency_ns=int(data["p99_latency_ns"]) if "p99_latency_ns" in data else None,
        error=data.get("error"),
    )


# Leaderboard
@app.get("/api/leaderboard")
async def get_leaderboard():
    # Get all teams sorted by score (descending)
    entries = await redis_pool.zrevrange(LEADERBOARD_KEY, 0, 49, withscores=True)
    result = []
    for rank, (team_name, score) in enumerate(entries, 1):
        team_data = await redis_pool.hgetall(f"iicpc:team:{team_name}")
        lb_data = await redis_pool.hgetall(f"iicpc:lb:{team_name}")
        result.append(LeaderboardEntry(
            rank=rank,
            team_name=team_name,
            score=score,
            throughput=float(lb_data.get("throughput", 0)),
            correctness=float(lb_data.get("correctness", 0)),
            p99_latency_ns=int(lb_data.get("p99_latency_ns", 0)),
            submissions=int(team_data.get("submissions", 0)),
            last_submitted=lb_data.get("last_submitted", ""),
        ))
    return result


# Team Submissions History
@app.get("/api/submissions")
async def get_submissions(team_name: str = Depends(get_current_team)):
    """Get all submissions for the authenticated team, newest first."""
    # Scan for all job keys belonging to this team
    submissions = []
    cursor = 0
    while True:
        cursor, keys = await redis_pool.scan(cursor, match=f"{JOB_PREFIX}*", count=100)
        for key in keys:
            data = await redis_pool.hgetall(key)
            if data.get("team_name") == team_name:
                job_id = key.replace(JOB_PREFIX, "")
                submissions.append({
                    "job_id": job_id,
                    "status": data.get("status", "unknown"),
                    "filename": data.get("filename", "unknown.cpp"),
                    "submitted_at": data.get("submitted_at", ""),
                    "score": float(data["score"]) if "score" in data else None,
                    "throughput": float(data["throughput"]) if "throughput" in data else None,
                    "correctness": float(data["correctness"]) if "correctness" in data else None,
                    "p99_latency_ns": int(data["p99_latency_ns"]) if "p99_latency_ns" in data else None,
                    "error": data.get("error"),
                    "file_size": int(data.get("file_size", 0)),
                })
        if cursor == 0:
            break
    # Sort by submitted_at descending
    submissions.sort(key=lambda x: x.get("submitted_at", ""), reverse=True)
    return submissions[:50]  # Cap at 50


# Recent Activity — Public feed of recent events
@app.get("/api/activity")
async def get_activity():
    """Get recent submission activity across all teams (public, no auth)."""
    activity = []
    cursor = 0
    while True:
        cursor, keys = await redis_pool.scan(cursor, match=f"{JOB_PREFIX}*", count=200)
        for key in keys:
            data = await redis_pool.hgetall(key)
            if data:
                job_id = key.replace(JOB_PREFIX, "")
                status = data.get("status", "unknown")
                action = status
                if status == "scored" and "score" in data:
                    action = f"Scored {float(data['score']):.3f}"
                elif status == "failed":
                    action = "Build failed"
                elif status == "compiling":
                    action = "Running benchmark"
                elif status == "queued":
                    action = "Queued"

                activity.append({
                    "id": job_id[:8],
                    "team": data.get("team_name", "unknown"),
                    "action": action,
                    "status": status,
                    "submitted_at": data.get("submitted_at", ""),
                })
        if cursor == 0:
            break
    activity.sort(key=lambda x: x.get("submitted_at", ""), reverse=True)
    return activity[:20]


# WebSocket — Live Updates
@app.websocket("/ws/live")
async def websocket_live(ws: WebSocket):
    await ws.accept()
    ws_connections.append(ws)
    try:
        # Send current leaderboard on connect
        leaderboard = await get_leaderboard()
        await ws.send_json({
            "type": "leaderboard",
            "data": [e.model_dump() for e in leaderboard],
        })
        # Keep alive, receive pings
        while True:
            try:
                await asyncio.wait_for(ws.receive_text(), timeout=30)
            except asyncio.TimeoutError:
                await ws.send_json({"type": "ping"})
    except WebSocketDisconnect:
        pass
    finally:
        ws_connections.remove(ws)


async def broadcast_update(data: dict):
    """Broadcast update to all connected WebSocket clients."""
    dead = []
    for ws in ws_connections:
        try:
            await ws.send_json(data)
        except Exception:
            dead.append(ws)
    for ws in dead:
        ws_connections.remove(ws)


# Background Worker — Processes Submission Queue
async def submission_worker():
    """
    FIFO worker: pops one job at a time from Redis queue,
    runs the contest pipeline, updates score + leaderboard.
    Only ONE contest runs at a time (Firecracker isolation).
    """
    global _active_jobs, _total_scored, _total_failed
    while True:
        try:
            # Block-pop from queue (timeout 5s to allow cancellation checks)
            result = await redis_pool.brpop(QUEUE_KEY, timeout=5)
            if result is None:
                continue

            _, job_id = result
            job_key = f"{JOB_PREFIX}{job_id}"
            job_data = await redis_pool.hgetall(job_key)

            if not job_data:
                continue

            team_name = job_data["team_name"]
            source_path = job_data["source_path"]

            # Update status: compiling
            await redis_pool.hset(job_key, "status", "compiling")
            await broadcast_update({
                "type": "status_update",
                "job_id": job_id,
                "team_name": team_name,
                "status": "compiling",
            })

            _active_jobs += 1

            # Run the contest pipeline via subprocess
            await redis_pool.hset(job_key, "status", "running")
            await broadcast_update({
                "type": "status_update",
                "job_id": job_id,
                "team_name": team_name,
                "status": "running",
            })

            result_data = await run_contest_pipeline(job_id, source_path)
            _active_jobs -= 1

            if result_data.get("success"):
                score = result_data["score"]
                await redis_pool.hset(job_key, mapping={
                    "status": "scored",
                    "score": str(score),
                    "throughput": str(result_data.get("throughput", 0)),
                    "correctness": str(result_data.get("correctness", 0)),
                    "p99_latency_ns": str(result_data.get("p99_latency_ns", 0)),
                })

                # Update leaderboard (keep best score)
                current_best = await redis_pool.zscore(LEADERBOARD_KEY, team_name)
                if current_best is None or score > current_best:
                    await redis_pool.zadd(LEADERBOARD_KEY, {team_name: score})
                    await redis_pool.hset(f"iicpc:lb:{team_name}", mapping={
                        "throughput": str(result_data.get("throughput", 0)),
                        "correctness": str(result_data.get("correctness", 0)),
                        "p99_latency_ns": str(result_data.get("p99_latency_ns", 0)),
                        "last_submitted": datetime.now(timezone.utc).isoformat(),
                    })

                await broadcast_update({
                    "type": "scored",
                    "job_id": job_id,
                    "team_name": team_name,
                    "score": score,
                })

                # Refresh full leaderboard for all clients
                leaderboard = await get_leaderboard()
                await broadcast_update({
                    "type": "leaderboard",
                    "data": [e.model_dump() for e in leaderboard],
                })

                _total_scored += 1
                logger.info("Job scored: job=%s team=%s score=%.4f throughput=%.0f",
                            job_id[:8], team_name, score,
                            result_data.get("throughput", 0))
            else:
                await redis_pool.hset(job_key, mapping={
                    "status": "failed",
                    "error": result_data.get("error", "Unknown error"),
                })
                await broadcast_update({
                    "type": "failed",
                    "job_id": job_id,
                    "team_name": team_name,
                    "error": result_data.get("error", ""),
                })

                _total_failed += 1
                logger.warning("Job failed: job=%s team=%s error=%s",
                               job_id[:8], team_name,
                               result_data.get("error", "unknown")[:200])
        except asyncio.CancelledError:
            break
        except Exception as e:
            print(f"[worker] Error: {e}")
            _active_jobs = max(0, _active_jobs - 1)
            await asyncio.sleep(1)


async def run_contest_pipeline(job_id: str, source_path: str) -> dict:
    """
    Dev-mode pipeline: compile locally with g++, then run contest engine.
    Steps:
      1. Compile with g++ (catches syntax errors, missing headers, etc.)
      2. Run run_contest --source FILE --no-firecracker (real benchmarking)
      3. Parse stderr for MATCH RESULT table
    """
    try:
        project_root = Path(__file__).resolve().parent.parent.parent
        engine_binary = project_root / "build" / "run_contest"
        output_dir = RESULTS_DIR / job_id
        output_dir.mkdir(parents=True, exist_ok=True)
        sdk_include = project_root / "sdk" / "include"

        # Step 1: Syntax check with g++ (fast fail for bad code)
        compile_cmd = [
            "g++", "-std=c++23", "-fsyntax-only", "-Wall", "-Wextra",
            f"-I{sdk_include}",
            str(source_path),
        ]

        logger.info("Pipeline syntax check: job=%s", job_id[:8])

        try:
            compile_proc = await asyncio.create_subprocess_exec(
                *compile_cmd,
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.PIPE,
            )
            compile_stdout, compile_stderr = await asyncio.wait_for(
                compile_proc.communicate(), timeout=30
            )
        except asyncio.TimeoutError:
            return {"success": False, "error": "Compilation timed out (30s limit)"}

        if compile_proc.returncode != 0:
            error_text = compile_stderr.decode("utf-8", errors="replace")[-2000:]
            # Clean up paths from error messages for readability
            error_text = error_text.replace(str(source_path), "contestant.cpp")
            logger.warning("Compile failed: job=%s rc=%d", job_id[:8],
                           compile_proc.returncode)
            return {
                "success": False,
                "error": f"Compilation failed:\n{error_text}",
            }

        logger.info("Pipeline compiled: job=%s", job_id[:8])

        # Step 2: Run contest engine (if binary exists)
        if engine_binary.exists():
            contest_cmd = [
                str(engine_binary),
                "--source", str(source_path),
                "--no-firecracker",
                "--contestant", job_id[:8],
                "--duration", "10",  # 10s for dev mode
            ]

            logger.info("Pipeline run: job=%s engine=%s", job_id[:8],
                         str(engine_binary))

            try:
                contest_proc = await asyncio.create_subprocess_exec(
                    *contest_cmd,
                    stdout=asyncio.subprocess.PIPE,
                    stderr=asyncio.subprocess.PIPE,
                )
                contest_stdout, contest_stderr = await asyncio.wait_for(
                    contest_proc.communicate(), timeout=120
                )
            except asyncio.TimeoutError:
                return {
                    "success": False,
                    "error": "Benchmark timed out (120s limit). "
                             "Your engine may have an infinite loop or deadlock.",
                }

            stderr_text = contest_stderr.decode("utf-8", errors="replace")

            # Check for crashes (segfault, abort, etc.)
            if contest_proc.returncode != 0 and contest_proc.returncode != 1:
                # Negative return codes indicate signals
                signal_names = {
                    -6: "SIGABRT (abort/assertion failed)",
                    -11: "SIGSEGV (segmentation fault)",
                    -8: "SIGFPE (floating point exception)",
                    -4: "SIGILL (illegal instruction)",
                    -9: "SIGKILL (killed)",
                }
                sig_name = signal_names.get(
                    contest_proc.returncode,
                    f"exit code {contest_proc.returncode}"
                )
                return {
                    "success": False,
                    "error": f"Engine crashed: {sig_name}\n"
                             f"{stderr_text[-500:]}",
                }

            # Parse the MATCH RESULT table from stderr
            result_data = _parse_match_result(stderr_text)

            if result_data:
                logger.info(
                    "Pipeline scored: job=%s score=%.4f throughput=%.0f",
                    job_id[:8], result_data["score"], result_data["throughput"]
                )
                return {
                    "success": True,
                    "score": result_data["score"],
                    "throughput": result_data["throughput"],
                    "correctness": result_data["correctness"],
                    "p99_latency_ns": result_data["p99_latency_ns"],
                }
            else:
                # Compiled but runner didn't produce results
                # Check if compilation itself failed within run_contest
                if "Compilation FAILED" in stderr_text:
                    # Extract compile error from run_contest output
                    err_start = stderr_text.find("Compilation FAILED")
                    err_snippet = stderr_text[err_start:err_start + 500]
                    return {
                        "success": False,
                        "error": f"Engine compilation failed:\n{err_snippet}",
                    }
                elif "contestant did not connect" in stderr_text:
                    return {
                        "success": False,
                        "error": "Engine failed to connect to gateway. "
                                 "Ensure your code implements the IStrategy "
                                 "interface and links the SDK.",
                    }
                else:
                    return {
                        "success": False,
                        "error": f"Contest runner finished but produced no score. "
                                 f"Output:\n{stderr_text[-1000:]}",
                    }
        else:
            # No engine binary — just verify compilation success
            logger.info("Pipeline: no engine binary, compile-only mode")
            return {
                "success": False,
                "error": "Compilation succeeded, but contest engine "
                         "(build/run_contest) not found. "
                         "Run 'cmake --build build' first.",
            }

    except Exception as e:
        logger.exception("Pipeline error for job %s", job_id[:8])
        return {"success": False, "error": str(e)}


def _parse_match_result(stderr_text: str) -> dict | None:
    """Parse the MATCH RESULT table printed by run_contest to stderr."""
    try:
        lines = stderr_text.split("\n")

        result = {
            "score": 0.0,
            "throughput": 0.0,
            "correctness": 0.0,
            "p99_latency_ns": 0,
        }

        for line in lines:
            line_stripped = line.strip().strip("║").strip()

            if "FINAL SCORE:" in line:
                parts = line_stripped.split("FINAL SCORE:")
                if len(parts) == 2:
                    result["score"] = float(parts[1].strip().split()[0])

            elif "Throughput:" in line and "OPS" in line:
                parts = line_stripped.split("Throughput:")
                if len(parts) == 2:
                    val = parts[1].strip().split()[0]
                    result["throughput"] = float(val)

            elif "Correctness:" in line:
                parts = line_stripped.split("Correctness:")
                if len(parts) == 2:
                    result["correctness"] = float(parts[1].strip().split()[0])

            elif "p99 Latency:" in line:
                parts = line_stripped.split("p99 Latency:")
                if len(parts) == 2:
                    val = parts[1].strip().split()[0]
                    result["p99_latency_ns"] = int(val)

        # Only return if we actually found a score
        if result["score"] > 0 or result["throughput"] > 0:
            return result
        return None

    except (ValueError, IndexError):
        return None


# Health check
@app.get("/api/health")
async def health():
    redis_ok = False
    try:
        await redis_pool.ping()
        redis_ok = True
    except Exception:
        pass

    return {
        "status": "healthy" if redis_ok else "degraded",
        "redis": redis_ok,
        "version": "1.0.0",
        "timestamp": datetime.now(timezone.utc).isoformat(),
    }


# SDK Download — zip of sdk/ for contestants
@app.get("/api/sdk/download")
async def download_sdk():
    """Package sdk/include + sdk/examples + sdk/src into a downloadable zip."""
    import zipfile
    import io

    project_root = Path(__file__).resolve().parent.parent.parent
    sdk_dir = project_root / "sdk"

    if not sdk_dir.exists():
        raise HTTPException(404, "SDK directory not found")

    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as zf:
        for subdir in ["include", "examples", "src"]:
            target = sdk_dir / subdir
            if target.exists():
                for fpath in target.rglob("*"):
                    if fpath.is_file() and fpath.suffix in (
                        ".hpp", ".h", ".cpp", ".cc", ".cxx"
                    ):
                        arcname = f"iicpc-sdk/{subdir}/{fpath.relative_to(target)}"
                        zf.write(fpath, arcname)

        # Add a README
        readme = (
            "# IICPC SDK\n\n"
            "## Quick Start\n"
            "1. Copy `examples/example_mm.cpp` as your starting point\n"
            "2. Implement the IStrategy interface callbacks\n"
            "3. Include `sdk/strategy_sdk.hpp` for all types\n"
            "4. Upload your .cpp file on the Submit page\n\n"
            "## Files\n"
            "- `include/sdk/` — Protocol headers, strategy interface\n"
            "- `examples/example_mm.cpp` — Example market-maker strategy\n"
            "- `src/strategy_main.cpp` — Entry point (linked automatically)\n\n"
            "## Compile locally\n"
            "```\n"
            "g++ -std=c++23 -O2 -I include/ examples/example_mm.cpp "
            "src/strategy_main.cpp -o my_strategy -lpthread\n"
            "```\n"
        )
        zf.writestr("iicpc-sdk/README.md", readme)

    buf.seek(0)
    from starlette.responses import Response
    return Response(
        content=buf.getvalue(),
        media_type="application/zip",
        headers={"Content-Disposition": "attachment; filename=iicpc-sdk.zip"},
    )


# Metrics endpoint — for monitoring dashboards
@app.get("/api/metrics")
async def metrics():
    """Prometheus-style metrics for monitoring."""
    queue_depth = await redis_pool.llen(QUEUE_KEY) if redis_pool else 0
    uptime = time.time() - _server_start_time

    return {
        "uptime_seconds": round(uptime, 1),
        "total_submissions": _total_submissions,
        "total_scored": _total_scored,
        "total_failed": _total_failed,
        "queue_depth": queue_depth,
        "active_jobs": _active_jobs,
        "max_concurrent_jobs": MAX_CONCURRENT_JOBS,
        "max_queue_depth": MAX_QUEUE_DEPTH,
        "submission_cooldown_secs": SUBMISSION_COOLDOWN_SECS,
        "connected_websockets": len(ws_connections),
    }


# System status — Firecracker / KVM / infra readiness
@app.get("/api/system/status")
async def system_status():
    """Full system readiness check for production deployment."""
    project_root = Path(__file__).resolve().parent.parent.parent

    kvm = Path("/dev/kvm").exists()
    fc_binary = shutil.which("firecracker") is not None
    kernel = (project_root / "infra" / "firecracker" / "vmlinux.bin").exists()
    rootfs = ((project_root / "infra" / "firecracker" / "base_rootfs.ext4").exists() or
              (project_root / "infra" / "firecracker" / "contestant_rootfs.ext4").exists())
    snapshots = (project_root / "infra" / "firecracker" / "snapshots" / "snapshot_state").exists()
    sandbox_sh = (project_root / "scripts" / "firecracker_sandbox.sh").exists()
    cg_sandbox = (project_root / "scripts" / "sandbox_run.sh").exists()
    engine = Path(ENGINE_BINARY).exists() or (project_root / "build" / "run_contest").exists()

    # Determine isolation mode
    if sandbox_sh and kvm and fc_binary and kernel and rootfs:
        isolation = "firecracker"
    elif cg_sandbox:
        isolation = "cgroups_v2"
    elif engine:
        isolation = "direct"
    else:
        isolation = "none"

    redis_ok = False
    try:
        await redis_pool.ping()
        redis_ok = True
    except Exception:
        pass

    # HugePages
    hugepages = 0
    try:
        with open("/proc/meminfo") as f:
            for line in f:
                if "HugePages_Total" in line:
                    hugepages = int(line.split()[1])
                    break
    except Exception:
        pass

    return {
        "isolation_mode": isolation,
        "kvm_available": kvm,
        "firecracker_binary": fc_binary,
        "kernel_image": kernel,
        "rootfs_image": rootfs,
        "snapshots_ready": snapshots,
        "sandbox_script": sandbox_sh,
        "cgroups_fallback": cg_sandbox,
        "engine_binary": engine,
        "redis_connected": redis_ok,
        "hugepages_total": hugepages,
        "ready": isolation != "none" and redis_ok,
    }


# Admin authentication helper
async def verify_admin(request: Request):
    """Check admin password from Authorization header or query param."""
    auth_header = request.headers.get("x-admin-key", "")
    if auth_header == ADMIN_PASSWORD:
        return True
    raise HTTPException(403, "Invalid admin key")


# Competition timer endpoints
@app.get("/api/competition/status")
async def competition_status():
    """Public endpoint: get competition state + remaining time."""
    state = await redis_pool.get(COMP_KEY_STATE) or "stopped"
    end_time = await redis_pool.get(COMP_KEY_END_TIME)
    start_time = await redis_pool.get(COMP_KEY_START_TIME)
    duration = await redis_pool.get(COMP_KEY_DURATION)

    remaining_secs = 0
    if end_time:
        try:
            end_dt = datetime.fromisoformat(end_time)
            remaining = (end_dt - datetime.now(timezone.utc)).total_seconds()
            remaining_secs = max(0, int(remaining))
            if remaining_secs == 0 and state == "running":
                state = "finished"
                await redis_pool.set(COMP_KEY_STATE, "finished")
        except (ValueError, TypeError):
            pass

    return {
        "state": state,
        "start_time": start_time,
        "end_time": end_time,
        "duration_mins": int(duration) if duration else None,
        "remaining_secs": remaining_secs,
        "submissions_open": state == "running" and remaining_secs > 0,
    }


# Admin endpoints
@app.post("/api/admin/login")
async def admin_login(request: Request):
    """Verify admin password and return confirmation."""
    body = await request.json()
    pwd = body.get("password", "")
    if pwd != ADMIN_PASSWORD:
        raise HTTPException(403, "Invalid admin password")
    return {"authenticated": True}


@app.post("/api/admin/competition/start")
async def admin_start_competition(request: Request):
    """Start competition with a duration in minutes."""
    await verify_admin(request)
    body = await request.json()
    duration_mins = int(body.get("duration_mins", 120))

    now = datetime.now(timezone.utc)
    end_time = now + timedelta(minutes=duration_mins)

    await redis_pool.set(COMP_KEY_STATE, "running")
    await redis_pool.set(COMP_KEY_START_TIME, now.isoformat())
    await redis_pool.set(COMP_KEY_END_TIME, end_time.isoformat())
    await redis_pool.set(COMP_KEY_DURATION, str(duration_mins))

    logger.info("Competition started: duration=%dm end=%s", duration_mins, end_time)

    await broadcast_update({
        "type": "competition",
        "data": {"state": "running", "end_time": end_time.isoformat(),
                 "duration_mins": duration_mins},
    })

    return {
        "state": "running",
        "start_time": now.isoformat(),
        "end_time": end_time.isoformat(),
        "duration_mins": duration_mins,
    }


@app.post("/api/admin/competition/stop")
async def admin_stop_competition(request: Request):
    """Stop competition — no more submissions accepted."""
    await verify_admin(request)
    await redis_pool.set(COMP_KEY_STATE, "stopped")
    logger.info("Competition stopped by admin")

    await broadcast_update({
        "type": "competition",
        "data": {"state": "stopped"},
    })

    return {"state": "stopped"}


@app.post("/api/admin/competition/extend")
async def admin_extend_competition(request: Request):
    """Extend competition end time by N minutes."""
    await verify_admin(request)
    body = await request.json()
    extra_mins = int(body.get("minutes", 30))

    end_time = await redis_pool.get(COMP_KEY_END_TIME)
    if not end_time:
        raise HTTPException(400, "No competition running")

    end_dt = datetime.fromisoformat(end_time)
    new_end = end_dt + timedelta(minutes=extra_mins)
    await redis_pool.set(COMP_KEY_END_TIME, new_end.isoformat())
    await redis_pool.set(COMP_KEY_STATE, "running")

    logger.info("Competition extended by %dm, new end=%s", extra_mins, new_end)

    await broadcast_update({
        "type": "competition",
        "data": {"state": "running", "end_time": new_end.isoformat()},
    })

    return {"end_time": new_end.isoformat(), "extended_by_mins": extra_mins}


@app.post("/api/admin/leaderboard/reset")
async def admin_reset_leaderboard(request: Request):
    """Reset leaderboard — clears all scores."""
    await verify_admin(request)

    # Clear leaderboard sorted set
    await redis_pool.delete("iicpc:leaderboard")

    # Clear all team best scores
    keys = await redis_pool.keys("iicpc:team:*:best")
    if keys:
        await redis_pool.delete(*keys)

    logger.info("Leaderboard reset by admin")

    await broadcast_update({
        "type": "leaderboard_reset",
        "data": {"leaderboard": []},
    })

    return {"reset": True}


@app.get("/api/admin/jobs")
async def admin_all_jobs(request: Request):
    """List all jobs across all teams."""
    await verify_admin(request)

    keys = await redis_pool.keys("iicpc:job:*")
    jobs = []
    for key in keys[:200]:  # Cap at 200
        data = await redis_pool.hgetall(key)
        if data:
            jobs.append({
                "job_id": data.get("job_id", ""),
                "team_name": data.get("team_name", ""),
                "status": data.get("status", ""),
                "filename": data.get("filename", ""),
                "submitted_at": data.get("submitted_at", ""),
                "score": float(data["score"]) if data.get("score") else None,
                "error": data.get("error"),
            })

    jobs.sort(key=lambda j: j.get("submitted_at", ""), reverse=True)
    return jobs


@app.get("/api/admin/teams")
async def admin_all_teams(request: Request):
    """List all registered teams."""
    await verify_admin(request)

    keys = await redis_pool.keys("iicpc:team:*:password")
    teams = []
    for key in keys:
        team_name = key.replace("iicpc:team:", "").replace(":password", "")
        sub_count = await redis_pool.get(f"iicpc:team:{team_name}:submissions") or "0"
        best_score = await redis_pool.zscore("iicpc:leaderboard", team_name)
        teams.append({
            "team_name": team_name,
            "submissions": int(sub_count),
            "best_score": float(best_score) if best_score else None,
        })

    teams.sort(key=lambda t: t.get("best_score") or 0, reverse=True)
    return teams


# Post-Contest System Test (CF-Style Rejudge)
# After the contest ends, admin triggers system tests that re-run every
# scored submission against 10 adversarial stress scenarios. Results are
# blended: final = min(contest, 0.6 * contest + 0.4 * system_test).
# Contestants see ONLY aggregate pass/fail — no per-scenario breakdown.

SYSTEST_KEY_STATE = "iicpc:systest:state"       # idle / running / completed
SYSTEST_KEY_PROGRESS = "iicpc:systest:progress"  # "3/15" format
SYSTEST_KEY_RESULTS = "iicpc:systest:results"    # JSON blob of all results

_systest_running = False


@app.post("/api/admin/system-test")
async def admin_start_system_tests(request: Request):
    """Trigger post-contest system tests for all scored submissions."""
    global _systest_running
    await verify_admin(request)

    if _systest_running:
        raise HTTPException(409, "System tests already running")

    # Check competition is stopped/finished
    comp_state = await redis_pool.get(COMP_KEY_STATE)
    if comp_state == "running":
        raise HTTPException(
            400,
            "Competition still running. Stop or finish the competition first."
        )

    # Collect all teams with scores
    entries = await redis_pool.zrevrange(LEADERBOARD_KEY, 0, -1, withscores=True)
    if not entries:
        raise HTTPException(400, "No scored submissions to test")

    teams = [(team, score) for team, score in entries]

    # Set state
    await redis_pool.set(SYSTEST_KEY_STATE, "running")
    await redis_pool.set(SYSTEST_KEY_PROGRESS, f"0/{len(teams)}")

    # Broadcast system test start
    await broadcast_update({
        "type": "system_test",
        "data": {"state": "running", "total_teams": len(teams)},
    })

    # Launch background task
    asyncio.create_task(_run_system_tests(teams))

    logger.info("System tests started for %d teams", len(teams))
    return {
        "state": "running",
        "total_teams": len(teams),
        "teams": [t[0] for t in teams],
    }


@app.get("/api/admin/system-test/status")
async def admin_system_test_status(request: Request):
    """Get current system test progress."""
    await verify_admin(request)

    state = await redis_pool.get(SYSTEST_KEY_STATE) or "idle"
    progress = await redis_pool.get(SYSTEST_KEY_PROGRESS) or "0/0"
    results_json = await redis_pool.get(SYSTEST_KEY_RESULTS)

    results = []
    if results_json:
        try:
            results = json.loads(results_json)
        except json.JSONDecodeError:
            pass

    return {
        "state": state,
        "progress": progress,
        "results": results,
    }


# Public endpoint: contestants can check if system tests are running
@app.get("/api/system-test/status")
async def public_system_test_status():
    """Public: check if system tests are in progress."""
    state = await redis_pool.get(SYSTEST_KEY_STATE) or "idle"
    progress = await redis_pool.get(SYSTEST_KEY_PROGRESS) or "0/0"
    return {"state": state, "progress": progress}


async def _run_system_tests(teams: list[tuple[str, float]]):
    """Background task: run system tests for all teams sequentially."""
    global _systest_running
    _systest_running = True

    project_root = Path(__file__).resolve().parent.parent.parent
    engine_binary = project_root / "build" / "run_contest"
    all_results = []

    try:
        for idx, (team_name, original_score) in enumerate(teams):
            logger.info("System test %d/%d: team=%s score=%.4f",
                        idx + 1, len(teams), team_name, original_score)

            await redis_pool.set(SYSTEST_KEY_PROGRESS,
                                 f"{idx + 1}/{len(teams)}")

            # Broadcast progress
            await broadcast_update({
                "type": "system_test",
                "data": {
                    "state": "running",
                    "current_team": team_name,
                    "progress": f"{idx + 1}/{len(teams)}",
                },
            })

            # Find the team's best submission binary
            binary_path = await _find_team_binary(team_name, project_root)
            if not binary_path:
                logger.warning("No binary found for team %s, skipping", team_name)
                all_results.append({
                    "team": team_name,
                    "original_score": original_score,
                    "system_score": 0.0,
                    "final_score": 0.0,
                    "passed": 0,
                    "total": 10,
                    "error": "No binary found",
                })
                continue

            # Run system tests via run_contest --system-test
            result = await _run_system_test_for_team(
                engine_binary, binary_path, team_name, original_score)

            all_results.append(result)

            # Update leaderboard with blended score
            if result.get("final_score") is not None:
                final = result["final_score"]
                await redis_pool.zadd(LEADERBOARD_KEY, {team_name: final})

                # Update team details
                await redis_pool.hset(f"iicpc:lb:{team_name}", mapping={
                    "system_score": str(result.get("system_score", 0)),
                    "system_passed": str(result.get("passed", 0)),
                    "system_total": str(result.get("total", 10)),
                    "final_score": str(final),
                })

            logger.info("System test done: team=%s system=%.4f final=%.4f",
                        team_name,
                        result.get("system_score", 0),
                        result.get("final_score", 0))

        # Save all results
        await redis_pool.set(SYSTEST_KEY_RESULTS, json.dumps(all_results))
        await redis_pool.set(SYSTEST_KEY_STATE, "completed")

        # Broadcast completion + updated leaderboard
        leaderboard = await get_leaderboard()
        await broadcast_update({
            "type": "system_test",
            "data": {"state": "completed", "results_count": len(all_results)},
        })
        await broadcast_update({
            "type": "leaderboard",
            "data": [e.model_dump() for e in leaderboard],
        })

        logger.info("System tests completed: %d teams processed", len(teams))

    except Exception as e:
        logger.exception("System test worker error: %s", e)
        await redis_pool.set(SYSTEST_KEY_STATE, "failed")
    finally:
        _systest_running = False


async def _find_team_binary(team_name: str, project_root: Path) -> str | None:
    """Find the pre-compiled binary for a team's best submission."""
    # Scan jobs for this team's best scored submission
    cursor = 0
    best_job_id = None
    best_score = -1.0

    while True:
        cursor, keys = await redis_pool.scan(
            cursor, match=f"{JOB_PREFIX}*", count=100)
        for key in keys:
            data = await redis_pool.hgetall(key)
            if (data.get("team_name") == team_name and
                    data.get("status") == "scored"):
                score = float(data.get("score", 0))
                if score > best_score:
                    best_score = score
                    best_job_id = key.replace(JOB_PREFIX, "")
        if cursor == 0:
            break

    if not best_job_id:
        return None

    # Try to find the compiled binary
    # The pipeline compiles to /tmp/iicpc_contestant_bin or similar
    binary_candidates = [
        f"/tmp/iicpc_contestant_bin_{best_job_id[:8]}",
        "/tmp/iicpc_contestant_bin",
        str(UPLOAD_DIR / best_job_id / "contestant_bin"),
    ]

    # Also try to recompile from source if binary not found
    source_path = str(UPLOAD_DIR / best_job_id / "contestant.cpp")
    if Path(source_path).exists():
        output_bin = str(UPLOAD_DIR / best_job_id / "contestant_bin")
        sdk_include = project_root / "sdk" / "include"

        compile_cmd = [
            "g++", "-std=c++23", "-O2", "-march=native",
            f"-I{sdk_include}",
            str(source_path), "-o", output_bin,
            "-lpthread",
        ]

        try:
            proc = await asyncio.create_subprocess_exec(
                *compile_cmd,
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.PIPE,
            )
            _, stderr = await asyncio.wait_for(proc.communicate(), timeout=30)
            if proc.returncode == 0:
                return output_bin
            else:
                logger.warning("Recompile failed for %s: %s",
                               team_name, stderr.decode()[:200])
        except Exception as e:
            logger.warning("Recompile error for %s: %s", team_name, e)

    # Check existing candidates
    for candidate in binary_candidates:
        if Path(candidate).exists():
            return candidate

    return None


async def _run_system_test_for_team(
    engine_binary: Path,
    binary_path: str,
    team_name: str,
    original_score: float,
) -> dict:
    """Run system tests for a single team via run_contest --system-test."""
    try:
        if not engine_binary.exists():
            return {
                "team": team_name,
                "original_score": original_score,
                "system_score": 0.0,
                "final_score": original_score,
                "passed": 0,
                "total": 10,
                "error": "Engine binary not found",
            }

        cmd = [
            str(engine_binary),
            "--system-test",
            "--binary", binary_path,
            "--contestant", team_name,
            "--original-score", str(original_score),
            "--no-firecracker",  # Use direct process for speed
            "--scenarios", "all",
        ]

        proc = await asyncio.create_subprocess_exec(
            *cmd,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
        )

        # System tests can take a while (10 scenarios × ~8s each = ~80s)
        _, stderr_bytes = await asyncio.wait_for(
            proc.communicate(), timeout=300)

        stderr_text = stderr_bytes.decode("utf-8", errors="replace")

        # Parse system test results from stderr
        result = _parse_system_test_result(stderr_text, team_name, original_score)
        return result

    except asyncio.TimeoutError:
        return {
            "team": team_name,
            "original_score": original_score,
            "system_score": 0.0,
            "final_score": 0.0,
            "passed": 0,
            "total": 10,
            "error": "System test timed out (300s)",
        }
    except Exception as e:
        return {
            "team": team_name,
            "original_score": original_score,
            "system_score": 0.0,
            "final_score": original_score,
            "passed": 0,
            "total": 10,
            "error": str(e),
        }


def _parse_system_test_result(
    stderr_text: str,
    team_name: str,
    original_score: float,
) -> dict:
    """Parse the SYSTEM TEST RESULTS table from run_contest --system-test stderr."""
    result = {
        "team": team_name,
        "original_score": original_score,
        "system_score": 0.0,
        "final_score": original_score,
        "passed": 0,
        "total": 10,
        "error": None,
    }

    try:
        for line in stderr_text.split("\n"):
            stripped = line.strip().strip("║").strip()

            if "Scenarios Passed:" in stripped:
                parts = stripped.split("Scenarios Passed:")[1].strip()
                nums = parts.split("/")
                if len(nums) >= 2:
                    result["passed"] = int(nums[0].strip())
                    result["total"] = int(nums[1].strip().split()[0])

            elif "System Score:" in stripped:
                parts = stripped.split("System Score:")[1].strip()
                result["system_score"] = float(parts.split()[0])

            elif "Final Score:" in stripped:
                parts = stripped.split("Final Score:")[1].strip()
                result["final_score"] = float(parts.split()[0])

    except (ValueError, IndexError) as e:
        result["error"] = f"Parse error: {e}"

    return result


# Job cleanup worker — removes old uploads and results
async def job_cleanup_worker():
    """Periodically clean up old job files to prevent disk exhaustion."""
    while True:
        try:
            await asyncio.sleep(3600)  # Run every hour
            cutoff = time.time() - (JOB_RETENTION_HOURS * 3600)
            cleaned = 0

            for job_dir in UPLOAD_DIR.iterdir():
                if job_dir.is_dir():
                    try:
                        mtime = job_dir.stat().st_mtime
                        if mtime < cutoff:
                            shutil.rmtree(job_dir, ignore_errors=True)
                            cleaned += 1
                    except OSError:
                        continue

            for result_dir in RESULTS_DIR.iterdir():
                if result_dir.is_dir():
                    try:
                        mtime = result_dir.stat().st_mtime
                        if mtime < cutoff:
                            shutil.rmtree(result_dir, ignore_errors=True)
                            cleaned += 1
                    except OSError:
                        continue

            if cleaned > 0:
                logger.info("Cleanup: removed %d expired job directories", cleaned)

        except asyncio.CancelledError:
            break
        except Exception as e:
            logger.error("Cleanup worker error: %s", e)
            await asyncio.sleep(60)


# Redpanda Consumer — Event Streaming from C++ Engine
# Consumes contest results from the `iicpc.results` Kafka topic (Redpanda)
# and broadcasts them to WebSocket clients. This completes the streaming loop:
#   C++ Engine → Redpanda (iicpc.results) → FastAPI → WebSocket → Browser
async def redpanda_consumer():
    """
    Minimal Kafka fetch consumer (ApiKey=1, ApiVersion=0).
    No kafka-python dependency — raw socket protocol.
    Gracefully degrades if Redpanda is unavailable.
    """
    import socket
    import struct

    host, port_str = REDPANDA_BROKER.split(":")
    port = int(port_str)
    correlation_id = 0

    while True:
        sock = None
        try:
            # Connect to Redpanda broker
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(5.0)
            sock.connect((host, port))
            sock.setblocking(False)
            logger.info("Redpanda consumer connected to %s:%d", host, port)

            fetch_offset = 0

            while True:
                await asyncio.sleep(1.0)  # Poll interval

                try:
                    # Build Fetch request (ApiKey=1, ApiVersion=0)
                    topic = REDPANDA_TOPIC.encode()
                    client_id = b"iicpc-api"

                    correlation_id += 1

                    # Fetch request body
                    body = b""
                    body += struct.pack(">hh", 1, 0)  # api_key=Fetch, version=0
                    body += struct.pack(">i", correlation_id)
                    body += struct.pack(">h", len(client_id)) + client_id
                    body += struct.pack(">i", -1)  # replica_id = -1 (consumer)
                    body += struct.pack(">i", 5000)  # max_wait_ms
                    body += struct.pack(">i", 1024)  # min_bytes
                    body += struct.pack(">i", 1)  # topic_count
                    body += struct.pack(">h", len(topic)) + topic
                    body += struct.pack(">i", 1)  # partition_count
                    body += struct.pack(">i", 0)  # partition
                    body += struct.pack(">q", fetch_offset)  # offset
                    body += struct.pack(">i", 1048576)  # max_bytes

                    # Send with size prefix
                    request = struct.pack(">i", len(body)) + body
                    sock.setblocking(True)
                    sock.settimeout(6.0)
                    sock.sendall(request)

                    # Read response size
                    size_buf = b""
                    while len(size_buf) < 4:
                        size_buf += sock.recv(4 - len(size_buf))
                    resp_size = struct.unpack(">i", size_buf)[0]

                    if resp_size <= 0 or resp_size > 10 * 1024 * 1024:
                        continue

                    # Read full response
                    resp = b""
                    while len(resp) < resp_size:
                        chunk = sock.recv(min(resp_size - len(resp), 65536))
                        if not chunk:
                            break
                        resp += chunk

                    sock.setblocking(False)

                    # Parse response: skip header, find message values
                    # This is a simplified parser — we extract JSON payloads
                    # by scanning for { and } delimiters in the response bytes
                    text = resp.decode("utf-8", errors="ignore")
                    start = 0
                    while True:
                        idx = text.find('{"contestant":', start)
                        if idx < 0:
                            break
                        # Find matching closing brace
                        depth = 0
                        end = idx
                        for ci in range(idx, len(text)):
                            if text[ci] == '{':
                                depth += 1
                            elif text[ci] == '}':
                                depth -= 1
                                if depth == 0:
                                    end = ci + 1
                                    break
                        if end > idx:
                            try:
                                result = json.loads(text[idx:end])
                                logger.info("Redpanda event: contestant=%s score=%.4f tps=%.0f",
                                           result.get("contestant", "?"),
                                           result.get("score", 0),
                                           result.get("tps", 0))
                                # Broadcast to WebSocket clients
                                await broadcast_update({
                                    "type": "engine_result",
                                    "data": result,
                                })
                                fetch_offset += 1
                            except json.JSONDecodeError:
                                pass
                        start = end

                except (socket.timeout, BlockingIOError, OSError):
                    continue

        except asyncio.CancelledError:
            break
        except (ConnectionRefusedError, OSError) as e:
            logger.debug("Redpanda consumer: %s (will retry in 10s)", e)
            await asyncio.sleep(10)
        except Exception as e:
            logger.warning("Redpanda consumer error: %s (will retry in 10s)", e)
            await asyncio.sleep(10)
        finally:
            if sock:
                try:
                    sock.close()
                except Exception:
                    pass
