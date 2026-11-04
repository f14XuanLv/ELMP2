"""Start or inspect the configured headless IDA MCP without touching the original IDB.

  python tools/ida_service.py status
  python tools/ida_service.py start

Start reuses an existing loopback service and database session. A missing working
database is copied once; existing working databases are never replaced. Status is
read-only. There is deliberately no stop command for shared IDA services.
"""

import argparse
from datetime import datetime, timezone
import ipaddress
import json
import math
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from urllib.parse import urlsplit

import devconfig
from mcp_client import Client, McpError, text_result


def configured_path(value: str) -> Path:
    path = Path(value)
    return (path if path.is_absolute() else devconfig.REPO / path).resolve()


def same_path(left: Path, right: Path) -> bool:
    if left.exists() and right.exists() and left.samefile(right):
        return True
    return os.path.normcase(str(left.resolve())) == os.path.normcase(str(right.resolve()))


def settings() -> dict:
    cfg = devconfig.load()["toolchain"]
    ida = cfg["ida"]
    endpoint = cfg["mcp"]["ida"]
    url = urlsplit(endpoint["url"])
    host = url.hostname or ""
    if host != "localhost":
        try:
            local = ipaddress.ip_address(host).is_loopback
        except ValueError:
            local = False
        if not local:
            raise ValueError("IDA MCP must use a literal loopback address or localhost")
    if url.scheme != "http" or url.username or url.password or url.query or url.fragment or url.path != "/mcp":
        raise ValueError("IDA MCP URL must be http://<loopback>:<port>/mcp")
    source = configured_path(ida["sourceDatabase"])
    work = configured_path(ida["workDatabase"])
    if same_path(source, work):
        raise ValueError("sourceDatabase and workDatabase must be different files")
    if not source.is_file():
        raise FileNotFoundError(f"sourceDatabase does not exist: {source}")
    session = ida["session"]
    if not isinstance(session, str) or not session:
        raise ValueError("toolchain.ida.session must be a nonempty string")
    return {
        "endpoint": endpoint, "host": "127.0.0.1" if host == "localhost" else host,
        "port": url.port or 80, "source": source, "work": work,
        "python": configured_path(ida["python"]), "cwd": configured_path(ida["cwd"]),
        "session": session, "output": devconfig.REPO / "build/toolchain-smoke/ida",
    }


def payload(client: Client, name: str, arguments=None) -> dict:
    result = text_result(client.call(name, arguments))
    if not isinstance(result, dict):
        raise McpError(f"{name}: expected an object, got {result!r}")
    if result.get("error"):
        raise McpError(f"{name}: {result['error']}")
    return result


def inspect_session(client: Client, cfg: dict) -> dict:
    listed = payload(client, "idb_list")
    session = next((item for item in listed.get("sessions", [])
                    if item.get("session_id") == cfg["session"]), None)
    result = {
        "url": cfg["endpoint"]["url"], "server": client.info,
        "source_database": str(cfg["source"]), "work_database": str(cfg["work"]),
        "session_id": cfg["session"], "ready": False,
        "state": "session_missing", "session": session,
    }
    if session is None:
        return result
    actual = session.get("input_path")
    if not actual or not same_path(configured_path(actual), cfg["work"]):
        raise McpError(f"Session {cfg['session']!r} belongs to {actual!r}, not {cfg['work']}; refusing to reuse it")
    if not session.get("is_active"):
        result["state"] = "session_inactive"
        return result
    health = payload(client, "server_health", {"database": cfg["session"]})
    actual = health.get("idb_path")
    if not actual or not same_path(configured_path(actual), cfg["work"]):
        raise McpError(f"IDA reports a different active IDB: {actual!r}; expected {cfg['work']}")
    result["health"] = health
    result["ready"] = (health.get("status") == "ok"
                       and health.get("auto_analysis_ready") is True
                       and health.get("hexrays_ready") is True
                       and int(str(health.get("imagebase", "0")), 0) == 0x140000000)
    result["state"] = "ready" if result["ready"] else "database_not_ready"
    return result


def ensure_work_database(cfg: dict) -> None:
    source, work = cfg["source"], cfg["work"]
    if same_path(source, work):
        raise ValueError("Working database resolves to the original database")
    if work.exists():
        if not work.is_file() or work.stat().st_size == 0:
            raise ValueError(f"Existing workDatabase is not a nonempty file: {work}")
        return
    work.parent.mkdir(parents=True, exist_ok=True)
    # Publish only a complete copy. A concurrent start must never see a partial IDB.
    target = tempfile.NamedTemporaryFile(mode="wb", dir=work.parent,
                                         prefix=work.name + ".copy-", suffix=".tmp", delete=False)
    temporary = Path(target.name)
    try:
        with target, source.open("rb") as original:
            shutil.copyfileobj(original, target, length=1024 * 1024)
        shutil.copystat(source, temporary)
        try:
            os.link(temporary, work)  # Atomic, same volume, and never replaces a winner.
        except FileExistsError:
            if not work.is_file() or work.stat().st_size == 0:
                raise ValueError(f"Concurrent workDatabase is invalid: {work}") from None
    finally:
        temporary.unlink(missing_ok=True)


def connect(cfg: dict, timeout: float) -> Client:
    return Client(cfg["endpoint"], timeout).__enter__()


def start_service(cfg: dict, timeout: float) -> tuple[Client, bool]:
    try:
        client = connect(cfg, min(timeout, 5))
        client.timeout = timeout
        return client, False
    except (McpError, OSError, ValueError) as unavailable:
        # An occupied port with an unhealthy or unrelated service is not ours to replace.
        try:
            with socket.create_connection((cfg["host"], cfg["port"]), timeout=2):
                pass
        except ConnectionRefusedError:
            pass
        except OSError as exc:
            raise McpError(f"Cannot safely determine whether the IDA port is free: {exc}") from exc
        else:
            raise McpError(f"IDA endpoint is occupied but did not initialize: {unavailable}") from unavailable
    if not cfg["python"].is_file() or not cfg["cwd"].is_dir():
        raise FileNotFoundError("Configured IDA Python executable or MCP source directory is missing")
    cfg["output"].mkdir(parents=True, exist_ok=True)
    command = [str(cfg["python"]), "-m", "ida_pro_mcp.idalib_supervisor",
               "--host", cfg["host"], "--port", str(cfg["port"]), "--max-workers", "2"]
    env = dict(os.environ, PYTHONUTF8="1", PYTHONIOENCODING="utf-8")
    with (cfg["output"] / "http-stdout.log").open("a", encoding="utf-8") as stdout, \
            (cfg["output"] / "http-stderr.log").open("a", encoding="utf-8") as stderr:
        process = subprocess.Popen(command, cwd=cfg["cwd"], env=env, stdin=subprocess.DEVNULL,
                                   stdout=stdout, stderr=stderr,
                                   creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0)
                                   | getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0))
    service = {"pid": process.pid, "command": command, "cwd": str(cfg["cwd"]),
               "url": cfg["endpoint"]["url"], "owned_by": "tools/ida_service.py",
               "started_at": datetime.now(timezone.utc).isoformat()}
    (cfg["output"] / "http-service.json").write_text(json.dumps(service, indent=2) + "\n", encoding="utf-8")
    deadline = time.monotonic() + min(timeout, 30)
    last_error = "not listening yet"
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise McpError(f"IDA supervisor exited with {process.returncode}; see {cfg['output'] / 'http-stderr.log'}")
        try:
            client = connect(cfg, 2)
            client.timeout = timeout
            return client, True
        except (McpError, OSError, ValueError) as exc:
            last_error = str(exc)
            time.sleep(0.2)
    raise McpError(f"IDA startup timed out (PID {process.pid}): {last_error}")


def start(cfg: dict, timeout: float) -> dict:
    client, launched = start_service(cfg, timeout)
    try:
        result = inspect_session(client, cfg)
        if not result["ready"]:
            ensure_work_database(cfg)
            opened = payload(client, "idb_open", {
                "input_path": str(cfg["work"]), "preferred_session_id": cfg["session"],
                "mode": "force_headless", "run_auto_analysis": False,
                "build_caches": False, "init_hexrays": True, "idle_ttl_sec": 3600,
            })
            actual = opened.get("session", {}).get("session_id")
            if actual != cfg["session"]:
                raise McpError(f"Working database is already open as {actual!r}; set toolchain.ida.session to that ID before retrying")
            result = inspect_session(client, cfg)
        result["service_started"] = launched
        cfg["output"].mkdir(parents=True, exist_ok=True)
        (cfg["output"] / "service-status.json").write_text(
            json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        return result
    finally:
        client.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("action", choices=("status", "start"))
    parser.add_argument("--timeout", type=float, default=120)
    args = parser.parse_args()
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be a finite positive number")
    try:
        cfg = settings()
        if args.action == "start":
            result = start(cfg, args.timeout)
        else:
            with Client(cfg["endpoint"], min(args.timeout, 10)) as client:
                result = inspect_session(client, cfg)
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return 0 if result["ready"] else 1
    except (McpError, OSError, ValueError, KeyError, RuntimeError) as exc:
        print(f"IDA service error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
