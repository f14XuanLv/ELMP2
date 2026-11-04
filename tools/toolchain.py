"""Read-only dependency and research-tool diagnostics.

  python tools/toolchain.py doctor
  python tools/toolchain.py smoke --out build/toolchain-smoke/report.json

Smoke queries existing MCP services and running isolated instances. It never starts
IDA or a game, sends game input, changes maps, or captures/restores a window.
"""

import argparse
from datetime import datetime, timezone
import json
import locale
import os
from pathlib import Path
import re
import shutil
import subprocess
import time

import bridge
import devconfig
import instance
from mcp_client import Client, prepare, text_result


class Audit:
    def __init__(self, action):
        self.report = {"schema": 1, "action": action,
                       "startedUtc": datetime.now(timezone.utc).isoformat(), "items": []}

    def add(self, name, status, summary, **details):
        self.report["items"].append({"name": name, "status": status, "summary": summary, **details})
        brief = " ".join(str(summary).split())
        print(f"{status:4} {name}: {brief[:300]}", flush=True)

    def run(self, name, operation, describe=lambda value: str(value), require=None):
        started = time.monotonic()
        response = None
        try:
            response = operation()
            problem = require(response) if require else None
            self.add(name, "FAIL" if problem else "PASS", problem or describe(response),
                     response=response, elapsedMs=round((time.monotonic() - started) * 1000, 1))
            return None if problem else response
        except Exception as exc:
            self.add(name, "FAIL", f"{type(exc).__name__}: {exc}", response=response,
                     elapsedMs=round((time.monotonic() - started) * 1000, 1))
            return None

    def finish(self, output):
        items = self.report["items"]
        totals = {status: sum(x["status"] == status for x in items) for status in ("PASS", "FAIL", "SKIP")}
        self.report.update(finishedUtc=datetime.now(timezone.utc).isoformat(), totals=totals)
        if output:
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_text(json.dumps(self.report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
            print(f"Report: {output.resolve()}")
        print(" ".join(f"{key}={value}" for key, value in totals.items()))
        return int(totals["FAIL"] > 0)


def resolved(value):
    path = Path(value)
    return (path if path.is_absolute() else devconfig.REPO / path).resolve()


def path_info(value, directory=False):
    path = resolved(value)
    if not (path.is_dir() if directory else path.is_file()):
        raise FileNotFoundError(str(path))
    return {"path": str(path), "kind": "directory" if directory else "file"}


def run_command(argv, accepted=(0,)):
    result = subprocess.run([str(x) for x in argv], capture_output=True, timeout=30,
                            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    def decode(data):
        try:
            return data.decode("utf-8")
        except UnicodeDecodeError:
            return data.decode(locale.getpreferredencoding(False), errors="replace")
    data = {"command": [str(x) for x in argv], "exitCode": result.returncode,
            "stdout": decode(result.stdout).strip(), "stderr": decode(result.stderr).strip()}
    if result.returncode not in accepted:
        raise RuntimeError(json.dumps(data, ensure_ascii=False))
    return data


def version_line(value):
    return (value["stdout"] or value["stderr"]).splitlines()[0]


def executable(name):
    found = shutil.which(name)
    if not found:
        raise FileNotFoundError(f"{name} is not on PATH")
    return found


def msvc_info():
    vswhere = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
    installations = run_command([vswhere, "-latest", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                                 "-property", "installationPath"])
    if not installations["stdout"]:
        raise RuntimeError("vswhere found no MSVC x64 installation")
    prefix = re.search(r'MSVC_TOOLSET_VERSION=([^"\r\n]+)', (devconfig.REPO / "build.bat").read_text(encoding="utf-8"))[1]
    roots = sorted((Path(installations["stdout"]) / "VC/Tools/MSVC").glob(prefix + ".*"))
    compiler = next((root / "bin/Hostx64/x64/cl.exe" for root in reversed(roots)
                     if (root / "bin/Hostx64/x64/cl.exe").is_file()), None)
    if not compiler:
        raise FileNotFoundError(f"MSVC toolset {prefix} (required by build.bat)")
    return run_command([compiler], accepted=(0, 2))


def windows_sdk_info():
    import winreg
    with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Microsoft\Windows Kits\Installed Roots",
                        access=winreg.KEY_READ | winreg.KEY_WOW64_32KEY) as key:
        root = Path(winreg.QueryValueEx(key, "KitsRoot10")[0])
    versions = [p.name for p in (root / "Include").glob("10.*")
                if (p / "um/Windows.h").is_file() and (root / "Lib" / p.name / "um/x64/kernel32.lib").is_file()]
    if not versions:
        raise FileNotFoundError("Windows 10/11 SDK x64 headers/libraries")
    return {"root": str(root), "versions": sorted(versions)}


def doctor(audit, cfg):
    for key, directory in (("paks", True), ("usmap", False), ("cue4parse", True)):
        audit.run(f"path.{key}", lambda k=key, d=directory: path_info(cfg[k], d), lambda v: v["path"])
    for project in ("CUE4Parse", "CUE4Parse-Conversion"):
        audit.run(f"source.{project}", lambda p=project: path_info(Path(cfg["cue4parse"]) / p / (p + ".csproj")), lambda v: v["path"])
    if cfg.get("fetchCache"):
        audit.run("path.fetchCache", lambda: path_info(cfg["fetchCache"], True), lambda v: v["path"])
    else:
        audit.add("path.fetchCache", "SKIP", "not configured; build may fetch dependencies")
    audit.run("version.python", lambda: run_command([cfg["python"], "--version"]), version_line)
    for command in ("git", "cmake", "ninja", "cargo", "rustc", "dotnet"):
        audit.run(f"version.{command}", lambda c=command: run_command([executable(c), "--version"]), version_line)
    audit.run("sdk.dotnet10", lambda: run_command([executable("dotnet"), "--list-sdks"]),
              lambda v: "; ".join(v["stdout"].splitlines()),
              lambda v: None if re.search(r"(?m)^10\.", v["stdout"]) else ".NET 10 SDK is required by tools/catalog")
    audit.run("sdk.msvc", msvc_info, lambda v: v["stderr"].splitlines()[0] if v["stderr"] else version_line(v))
    audit.run("sdk.windows", windows_sdk_info, lambda v: ", ".join(v["versions"]))
    for name, getter, directory in (
            ("cue4parse.mcp", lambda: cfg["toolchain"]["mcp"]["cue4parse"]["command"], False),
            ("ida.python", lambda: cfg["toolchain"]["ida"]["python"], False),
            ("ida.cwd", lambda: cfg["toolchain"]["ida"]["cwd"], True),
            ("ida.sourceDatabase", lambda: cfg["toolchain"]["ida"]["sourceDatabase"], False),
            ("ida.workDatabase", lambda: cfg["toolchain"]["ida"]["workDatabase"], False)):
        audit.run(f"path.{name}", lambda g=getter, d=directory: path_info(g(), d), lambda v: v["path"])
    build = devconfig.REPO / "build" / os.environ.get("ELMP_BUILD_CONFIG", "Game__Shipping__Win64")
    for flavor in ("dev", "shipping", "frameprobe"):
        audit.run(f"artifact.{flavor}", lambda f=flavor: path_info(build / f / "dlls/main.dll"), lambda v: v["path"])


def tool_data(result):
    value = text_result(result)
    if not isinstance(value, dict):
        raise ValueError("expected a JSON object in the tool response")
    return value


def tool_catalog(client):
    return {"server": client.info, "tools": client.tools()}


def catalog_problem(value, expected):
    missing = set(expected) - {item["name"] for item in value["tools"]}
    return "missing tools: " + ", ".join(sorted(missing)) if missing else None


def smoke_cue(audit, cfg):
    try:
        with Client(cfg["toolchain"]["mcp"]["cue4parse"], 60) as client:
            audit.run("cue4parse.tools", lambda: tool_catalog(client), lambda v: f'{len(v["tools"])} tools',
                      lambda v: catalog_problem(v, ("init_provider", "search_assets", "get_package_summary")))
            mounted = audit.run("cue4parse.mount", lambda: prepare(client, "cue4parse", cfg),
                                lambda r: f'{tool_data(r).get("fileCount", 0)} files',
                                lambda r: None if tool_data(r).get("fileCount", 0) > 0 else "provider has no files")
            if mounted is None:
                for name in ("search_assets", "get_package_summary"):
                    audit.add(f"cue4parse.{name}", "SKIP", "provider did not mount")
                return
            session = tool_data(mounted).get("sessionId")
            audit.run("cue4parse.search_assets", lambda: client.call("search_assets", {
                "query": "BP_Interactable_RestingPoint", "limit": 8, "sessionId": session}),
                lambda r: f'{len(tool_data(r).get("items", []))} matching assets',
                lambda r: None if tool_data(r).get("items") else "resting-point asset was not found")
            audit.run("cue4parse.get_package_summary", lambda: client.call("get_package_summary", {
                "packagePath": "/Game/_Zenith/Gameplay/Interactables/BP_Interactable_RestingPoint",
                "exportLimit": 3, "sessionId": session}),
                lambda r: f'{tool_data(r).get("returnedExports")} of {tool_data(r).get("exportCount")} exports',
                lambda r: None if tool_data(r).get("exports") else "package has no returned exports")
    except Exception as exc:
        audit.add("cue4parse.connection", "FAIL", f"{type(exc).__name__}: {exc}")


def same_path(left, right):
    return os.path.normcase(str(resolved(left))) == os.path.normcase(str(resolved(right)))


def smoke_ida(audit, cfg):
    hint = "Run tools/ida_service.py start to prepare the configured service/session."
    try:
        settings = cfg["toolchain"]["ida"]
        session = settings["session"]
        database = resolved(settings["workDatabase"])
        with Client(cfg["toolchain"]["mcp"]["ida"], 60) as client:
            audit.run("ida.tools", lambda: tool_catalog(client), lambda v: f'{len(v["tools"])} tools',
                      lambda v: catalog_problem(v, ("idb_list", "server_health", "decompile", "xrefs_to")))
            def database_problem(result):
                opened = next((row for row in tool_data(result).get("sessions", []) if row.get("session_id") == session), None)
                if opened is None:
                    return f"session {session!r} is not open. {hint}"
                if not opened.get("input_path") or not same_path(opened["input_path"], database):
                    return f"session {session!r} points to {opened.get('input_path')!r}, expected {database}"
                return None
            matched = audit.run("ida.idb_list", lambda: client.call("idb_list"),
                                lambda _: f"{session}: {database}", database_problem)
            if matched is None:
                for name in ("server_health", "decompile", "xrefs_to"):
                    audit.add(f"ida.{name}", "SKIP", "database identity was not confirmed")
                return
            def health_problem(result):
                value = tool_data(result)
                if value.get("status") != "ok" or value.get("auto_analysis_ready") is not True or value.get("hexrays_ready") is not True:
                    return "IDA analysis/Hex-Rays is not ready"
                if int(str(value.get("imagebase", "0")), 0) != 0x140000000:
                    return f"unexpected imagebase {value.get('imagebase')}"
                if not value.get("idb_path") or not same_path(value["idb_path"], database):
                    return "server_health database path differs from configured workDatabase"
                return None
            healthy = audit.run("ida.server_health", lambda: client.call("server_health", {"database": session}),
                                lambda r: f'ready, Hex-Rays ready, imagebase={tool_data(r)["imagebase"]}', health_problem)
            if healthy is None:
                for name in ("decompile", "xrefs_to"):
                    audit.add(f"ida.{name}", "SKIP", "IDA readiness/identity was not confirmed")
                return
            audit.run("ida.decompile", lambda: client.call("decompile", {
                "database": session, "addr": "0x140CF3740", "max_lines": 80}),
                lambda r: f'0x140CF3740: {tool_data(r).get("line_count")} pseudocode lines',
                lambda r: None if tool_data(r).get("code") and not tool_data(r).get("error") else "decompilation returned no code or an error")
            audit.run("ida.xrefs_to", lambda: client.call("xrefs_to", {
                "database": session, "addrs": ["0x140CF3740"], "limit": 8}),
                lambda r: f'{sum(len(row.get("xrefs") or []) for row in tool_data(r).get("result", []))} xrefs (limit 8)',
                lambda r: None if tool_data(r).get("result") and all(not row.get("error") and row.get("xrefs") is not None
                               for row in tool_data(r)["result"]) else "xref query returned no result or an error")
    except Exception as exc:
        audit.add("ida.connection", "FAIL", f"{type(exc).__name__}: {exc}. {hint}")


def smoke_instances(audit, cfg):
    # UEBridge has one request/response pair per instance: never issue concurrent requests.
    for name in cfg.get("instances", {}):
        try:
            inst = devconfig.instance(name)
        except Exception as exc:
            audit.add(f"instance.{name}", "FAIL", f"{type(exc).__name__}: {exc}")
            continue
        pids = audit.run(f"instance.{name}", lambda: instance.processes(inst),
                         lambda value: f"PID {value}" if value else "stopped")
        if not pids:
            audit.add(f"bridge.{name}", "SKIP", "instance is stopped or its process query failed")
            continue
        audit.run(f"bridge.{name}.ping", lambda: bridge.request(inst, "ping", timeout=15),
                  lambda _: "pong", lambda r: None if r.get("ok") and isinstance(r.get("result"), dict)
                  and r["result"].get("pong") is True else "UEBridge did not return a successful pong")
        for command, marker in (("status", "preflight: ok"), ("lan status", "session:"), ("perf", "frame.")):
            audit.run(f"bridge.{name}.{command.replace(' ', '_')}", lambda c=command: bridge.request(
                inst, "eval", f"return ELMP_Command({bridge.lua_string('elmp ' + c)})", timeout=15),
                lambda r: str(r.get("result", "")).splitlines()[0],
                lambda r, m=marker: None if r.get("ok") and isinstance(r.get("result"), str) and m in r["result"]
                else f"UEBridge command failed or expected marker {m!r} was absent")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("action", choices=("doctor", "smoke"))
    parser.add_argument("--out", type=Path, help="Save complete responses as UTF-8 JSON")
    args = parser.parse_args()
    audit = Audit(args.action)
    cfg = audit.run("configuration", devconfig.load, lambda _: "devconfig.json and local overrides loaded")
    if cfg is not None:
        doctor(audit, cfg)
        if args.action == "smoke":
            smoke_cue(audit, cfg)
            smoke_ida(audit, cfg)
            smoke_instances(audit, cfg)
    try:
        return audit.finish(args.out)
    except OSError as exc:
        print(f"FAIL report: {exc}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
