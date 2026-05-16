# =============================================================================
# main.py — IICPC Competition API Server
# =============================================================================
# FastAPI application with:
#   - File upload (streamed to disk, not buffered in RAM)
#   - Redis job queue (LPUSH/RPOP FIFO)
#   - JWT authentication
#   - WebSocket live leaderboard updates
#   - Background worker for contest execution
# =============================================================================

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

# =============================================================================
# Configuration
# =============================================================================
REDIS_URL = os.getenv("REDIS_URL", "redis://localhost:6379")
JWT_SECRET = os.getenv("JWT_SECRET", "iicpc-dev-secret-change-in-production")
JWT_ALGORITHM = "HS256"
JWT_EXPIRY_HOURS = 24
UPLOAD_DIR = Path(os.getenv("UPLOAD_DIR", "/tmp/iicpc/uploads"))
RESULTS_DIR = Path(os.getenv("RESULTS_DIR", "/tmp/iicpc/results"))
ENGINE_BINARY = os.getenv("ENGINE_BINARY", "/opt/iicpc/build/run_contest")
MAX_UPLOAD_SIZE = 50 * 1024 * 1024  # 50MB
CHUNK_SIZE = 8192  # Stream uploads in 8KB chunks

# Rate limiting
SUBMISSION_COOLDOWN_SECS = int(os.getenv("SUBMISSION_COOLDOWN", "30"))
MAX_CONCURRENT_JOBS = int(os.getenv("MAX_CONCURRENT_JOBS", "1"))
MAX_QUEUE_DEPTH = int(os.getenv("MAX_QUEUE_DEPTH", "50"))
JOB_RETENTION_HOURS = int(os.getenv("JOB_RETENTION_HOURS", "24"))

UPLOAD_DIR.mkdir(parents=True, exist_ok=True)
RESULTS_DIR.mkdir(parents=True, exist_ok=True)

# In-memory rate limiting state
_last_submission: dict[str, float] = {}
_active_jobs: int = 0
_server_start_time = time.time()
_total_submissions = 0
_total_scored = 0
_total_failed = 0

# =============================================================================
# Redis Keys
# =============================================================================
QUEUE_KEY = "iicpc:submission_queue"        # FIFO queue (list)
JOB_PREFIX = "iicpc:job:"                   # Per-job state hash
LEADERBOARD_KEY = "iicpc:leaderboard"       # Sorted set (score → team)
LIVE_CHANNEL = "iicpc:live"                 # Pub/sub for WebSocket updates


# =============================================================================
# Models
# =============================================================================
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


# =============================================================================
# App Lifecycle
# =============================================================================
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
    logger.info("Background workers started")
    yield
    worker_task.cancel()
    cleanup_task.cancel()
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


# =============================================================================
# JWT Auth
# =============================================================================
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


# =============================================================================
# Auth Endpoints
# =============================================================================
@app.post("/api/auth/register")
async def register(req: RegisterRequest):
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
async def login(req: LoginRequest):
    key = f"iicpc:team:{req.team_name}"
    data = await redis_pool.hgetall(key)
    if not data:
        raise HTTPException(401, "Team not found")

    pw_hash = hashlib.sha256(req.password.encode()).hexdigest()
    if data.get("password_hash") != pw_hash:
        raise HTTPException(401, "Invalid password")

    token = create_token(req.team_name)
    return {"token": token, "team_name": req.team_name}


# =============================================================================
# Submission Endpoint (streamed file upload)
# =============================================================================
ALLOWED_EXTENSIONS = {".cpp", ".cc", ".cxx", ".c", ".h", ".hpp"}


@app.post("/api/submit")
async def submit_code(
    file: UploadFile = File(...),
    team_name: str = Depends(get_current_team)
):
    global _total_submissions

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


# =============================================================================
# Job Status
# =============================================================================
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


# =============================================================================
# Leaderboard
# =============================================================================
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


# =============================================================================
# WebSocket — Live Updates
# =============================================================================
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


# =============================================================================
# Background Worker — Processes Submission Queue
# =============================================================================
async def submission_worker():
    """
    FIFO worker: pops one job at a time from Redis queue,
    runs the contest pipeline, updates score + leaderboard.
    Only ONE contest runs at a time (Firecracker isolation).
    """
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
    """Execute the sandbox runner. Single authoritative results path: results.json."""
    try:
        project_root = Path(__file__).resolve().parent.parent.parent
        fc_sandbox = project_root / "scripts" / "firecracker_sandbox.sh"
        cg_sandbox = project_root / "scripts" / "sandbox_run.sh"
        output_dir = RESULTS_DIR / job_id
        output_dir.mkdir(parents=True, exist_ok=True)

        # Detect isolation mode (single decision point)
        kvm_available = Path("/dev/kvm").exists()
        fc_assets = (project_root / "infra" / "firecracker" / "vmlinux.bin").exists() and \
                    (project_root / "infra" / "firecracker" / "base_rootfs.ext4").exists()

        if fc_sandbox.exists() and kvm_available and fc_assets:
            runner_name = "firecracker"
            cmd = ["bash", str(fc_sandbox), source_path, str(output_dir), job_id, "120"]
        elif cg_sandbox.exists():
            runner_name = "cgroups"
            cmd = ["bash", str(cg_sandbox), source_path, str(output_dir), job_id, "120"]
        else:
            return {"success": False, "error": "No sandbox runner available"}

        logger.info("Pipeline start: job=%s runner=%s", job_id[:8], runner_name)

        proc = await asyncio.create_subprocess_exec(
            *cmd,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
        )

        stdout, stderr = await asyncio.wait_for(
            proc.communicate(), timeout=180
        )

        # Single authoritative path: results.json written by sandbox script
        results_file = output_dir / "results.json"
        if not results_file.exists():
            output = (stderr.decode() + stdout.decode())[-2000:]
            logger.error("Pipeline produced no results.json: job=%s rc=%s output=%s",
                         job_id[:8], proc.returncode, output[:500])
            return {
                "success": False,
                "error": f"Pipeline produced no results (exit={proc.returncode}): {output}",
            }

        with open(results_file) as f:
            result_data = json.load(f)

        status = result_data.get("status", "unknown")

        if status == "scored":
            return {
                "success": True,
                "score": float(result_data.get("score", 0)),
                "throughput": float(result_data.get("throughput", 0)),
                "correctness": float(result_data.get("correctness", 0)),
                "p99_latency_ns": int(result_data.get("p99_latency_ns", 0)),
                "compile_time_ms": int(result_data.get("compile_time_ms", 0)),
                "runtime_ms": int(result_data.get("runtime_ms", 0)),
                "isolation": runner_name,
            }
        elif status == "timeout":
            return {
                "success": False,
                "error": f"Execution timed out ({result_data.get('runtime_ms', 0)}ms)",
            }
        else:
            return {
                "success": False,
                "error": result_data.get("error", f"Pipeline status: {status}"),
            }

    except json.JSONDecodeError as e:
        logger.error("Corrupt results.json for job %s: %s", job_id[:8], e)
        return {"success": False, "error": f"Corrupt results file: {e}"}
    except asyncio.TimeoutError:
        logger.error("Pipeline timeout for job %s (180s hard limit)", job_id[:8])
        return {"success": False, "error": "Contest execution timed out (180s)"}
    except Exception as e:
        logger.exception("Pipeline error for job %s", job_id[:8])
        return {"success": False, "error": str(e)}


# =============================================================================
# Health check
# =============================================================================
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


# =============================================================================
# Metrics endpoint — for monitoring dashboards
# =============================================================================
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


# =============================================================================
# System status — Firecracker / KVM / infra readiness
# =============================================================================
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


# =============================================================================
# Job cleanup worker — removes old uploads and results
# =============================================================================
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
