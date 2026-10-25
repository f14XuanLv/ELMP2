"""UEBridge file channel (Dev): run Lua on the game thread of an instance and print the result.

  python tools/bridge.py dev hello
  python tools/bridge.py dev eval "return 1+1"
  python tools/bridge.py dev elmp "elmp status"         (Dev build: ELMP_Command injected into UEBridge)
  python tools/bridge.py dev console "elmpprobe start 30 base"
  python tools/bridge.py dev wait-ready [--timeout 120]  waits until UEBridge answers

Protocol (UEBridge 1.2.1, protocol 3): write <ue4ss>\\bridge\\request.json {"id","op","code"}, the mod
polls it, runs the code via ExecuteInGameThread and writes response.json.
"""

import argparse
import json
import sys
import time
import uuid
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import devconfig  # noqa: E402

CONSOLE_LUA = r"""
local ksl = StaticFindObject("/Script/Engine.Default__KismetSystemLibrary")
local gs = StaticFindObject("/Script/Engine.Default__GameplayStatics")
local world = FindFirstOf("GameEngine").GameViewport.World
local pc = gs:GetPlayerController(world, 0)
ksl:ExecuteConsoleCommand(world, %s, pc)
return "sent"
"""


def request(inst: dict, op: str, code: str | None = None, timeout: float = 30.0) -> dict:
    d: Path = inst["bridge"]
    d.mkdir(parents=True, exist_ok=True)
    rid = uuid.uuid4().hex
    body = {"id": rid, "op": op}
    if code is not None:
        body["code"] = code
    resp = d / "response.json"
    tmp = d / "request.tmp"
    tmp.write_text(json.dumps(body), encoding="utf-8")
    tmp.replace(d / "request.json")
    end = time.time() + timeout
    while time.time() < end:
        try:
            data = json.loads(resp.read_text(encoding="utf-8"))
            if data.get("id") == rid:
                return data
        except (FileNotFoundError, json.JSONDecodeError, PermissionError):
            pass
        time.sleep(0.05)
    raise TimeoutError(f"no UEBridge response within {timeout:.0f} s")


def lua_string(s: str) -> str:
    return "'" + s.replace("\\", "\\\\").replace("'", "\\'") + "'"


def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("instance")
    p.add_argument("op", choices=["hello", "ping", "eval", "elmp", "console", "wait-ready"])
    p.add_argument("code", nargs="?")
    p.add_argument("--timeout", type=float, default=30.0)
    a = p.parse_args()
    inst = devconfig.instance(a.instance)
    if a.op == "wait-ready":
        end = time.time() + a.timeout
        while time.time() < end:
            try:
                r = request(inst, "ping", timeout=3)
                print(json.dumps(r.get("result"), ensure_ascii=False))
                return
            except TimeoutError:
                pass
        sys.exit("UEBridge not ready")
    if a.op in ("hello", "ping"):
        r = request(inst, a.op, timeout=a.timeout)
    elif a.op == "eval":
        r = request(inst, "eval", a.code, a.timeout)
    elif a.op == "elmp":
        r = request(inst, "eval", f"return ELMP_Command({lua_string(a.code or 'elmp status')})", a.timeout)
        if r.get("ok") and isinstance(r.get("result"), str):
            print(r["result"])
            return
    else:
        r = request(inst, "eval", CONSOLE_LUA % lua_string(a.code), a.timeout)
    if not r.get("ok"):
        print(json.dumps(r, ensure_ascii=False, indent=1))
        sys.exit(1)
    for line in r.get("output") or []:
        print(line)
    print(json.dumps(r.get("result"), ensure_ascii=False, indent=1))


if __name__ == "__main__":
    main()
