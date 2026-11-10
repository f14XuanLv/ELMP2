"""Isolated game instances from devconfig.json (F5.1): start / stop / status / log.

  python tools/instance.py start dev [--fps 60] [--res 1280x720] [--no-dpi-aware]
  python tools/instance.py stop dev          only ends the process whose EXE path is this instance's
  python tools/instance.py status [dev]
  python tools/instance.py log dev [--tail 40] [--grep ELMP]

Never touches a process outside the instances listed in devconfig.json.
"""

import argparse
import ctypes
from ctypes import wintypes
import os
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import devconfig  # noqa: E402


def processes(inst: dict) -> list[int]:
    exe = str(inst["exe"]).lower()
    out = subprocess.run(
        ["powershell", "-NoProfile", "-Command",
         f"Get-CimInstance Win32_Process -Filter \"Name='{devconfig.EXE}'\" | ForEach-Object {{ \"$($_.ProcessId)|$($_.ExecutablePath)\" }}"],
        capture_output=True, text=True, check=True).stdout
    pids = []
    for line in out.splitlines():
        pid, _, path = line.strip().partition("|")
        if path.lower() == exe:
            pids.append(int(pid))
    return pids


def placement(name: str, width: int, height: int, dpi_aware: bool) -> list[str]:
    if name not in ("host", "client1", "client2", "client3"):
        return []
    user = ctypes.WinDLL("user32", use_last_error=True)
    user.SetThreadDpiAwarenessContext.restype = ctypes.c_void_p
    previous = user.SetThreadDpiAwarenessContext(ctypes.c_void_p(-4 if dpi_aware else -1))
    area = wintypes.RECT()
    try:
        if not user.SystemParametersInfoW(0x0030, 0, ctypes.byref(area), 0):  # SPI_GETWORKAREA
            raise ctypes.WinError(ctypes.get_last_error())
    finally:
        if previous:
            user.SetThreadDpiAwarenessContext(ctypes.c_void_p(previous))
    x = max(area.left, area.right - width) if name in ("client1", "client3") else area.left
    y = max(area.top, area.bottom - height) if name in ("client2", "client3") else area.top
    return [f"-WinX={x}", f"-WinY={y}"]


def start(inst: dict, fps: int, res: str, dpi_aware: bool) -> None:
    if processes(inst):
        print(f"already running: {inst['name']} {processes(inst)}")
        return
    inst["userDir"].mkdir(parents=True, exist_ok=True)
    # A stopped process cannot finish an in-flight bridge request. Never replay it
    # in a new game session, and leave lastop.log intact for incident analysis.
    for name in ("request.json", "request.tmp", "response.json", "response.tmp"):
        (inst["bridge"] / name).unlink(missing_ok=True)
    w, h = map(int, res.lower().split("x"))
    args = [str(inst["exe"]), "-MultiprocessOSS", "-SaveToUserDir", f'-UserDir="{inst["userDir"]}"', "-windowed",
            f"-ResX={w}", f"-ResY={h}", *placement(inst["name"], w, h, dpi_aware), f'-ExecCmds="t.MaxFPS {fps}"']
    env = dict(os.environ)
    if dpi_aware:
        env["ELMP_DPI_AWARE"] = "1"
    flags = subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP
    proc = subprocess.Popen(" ".join(f'"{a}"' if " " in a and not a.startswith("-") else a for a in args), cwd=inst["root"], env=env,
                            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, creationflags=flags)
    print(f"started {inst['name']} pid {proc.pid} ({res}, t.MaxFPS {fps}, dpiAware={dpi_aware})")


def stop(inst: dict) -> None:
    pids = processes(inst)
    for pid in pids:
        subprocess.run(["taskkill", "/PID", str(pid), "/F"], capture_output=True, check=False)
    for _ in range(50):
        if not processes(inst):
            break
        time.sleep(0.2)
    print(f"stopped {inst['name']} {pids}" if pids else f"{inst['name']} not running")


def show_log(inst: dict, tail: int, grep: str | None) -> None:
    path = inst["root"] / "ue4ss" / "UE4SS.log"
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    if grep:
        lines = [l for l in lines if grep in l]
    print("\n".join(lines[-tail:]))


def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("action", choices=["start", "stop", "status", "log"])
    p.add_argument("instance", nargs="?")
    p.add_argument("--fps", type=int, default=60)
    p.add_argument("--res", default="1280x720")
    p.add_argument("--no-dpi-aware", action="store_true")
    p.add_argument("--tail", type=int, default=40)
    p.add_argument("--grep")
    a = p.parse_args()
    if a.action == "status":
        names = [a.instance] if a.instance else list(devconfig.load()["instances"])
        for n in names:
            print(f"{n}: {processes(devconfig.instance(n)) or 'not running'}")
        return
    if not a.instance:
        sys.exit("instance required")
    inst = devconfig.instance(a.instance)
    if a.action == "start":
        start(inst, a.fps, a.res, not a.no_dpi_aware)
    elif a.action == "stop":
        stop(inst)
    else:
        show_log(inst, a.tail, a.grep)


if __name__ == "__main__":
    main()
