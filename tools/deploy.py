"""Deploy build output into an instance from devconfig.json: <root>/ue4ss/Mods/ELMP (+ ElmpFrameProbe).

  python tools/deploy.py dev                      Dev build of ELMP, enabled
  python tools/deploy.py dev --flavor shipping
  python tools/deploy.py dev --probe on           also deploy + enable ElmpFrameProbe
  python tools/deploy.py dev --elmp off --probe on  bare-UE4SS baseline (ELMP disabled in mods.txt)

Replaces Mods/ELMP wholesale (the v1 deployment included, AGENTS.md), writes only the ELMP /
ElmpFrameProbe / ElmpProbe lines of mods.txt, and refuses while the instance is running. v1's
dependent test mod ElmpProbe (Lua, needs the v1 Lua API) is switched off, not deleted.
The deployed ELMP folder is asserted to contain no .lua (X-7).
"""

import argparse
import hashlib
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import devconfig  # noqa: E402
import instance as instances  # noqa: E402

REPO = devconfig.REPO


def set_mod(mods: Path, name: str, enabled: bool) -> None:
    path = mods / "mods.txt"
    lines = path.read_text(encoding="utf-8").splitlines()
    lines = [l for l in lines if l.split(":")[0].strip() != name]
    at = next((i for i, l in enumerate(lines) if l.strip().startswith("; Built-in keybinds")), None)
    if at is None:
        at = next((i for i, l in enumerate(lines) if l.split(":")[0].strip() == "Keybinds"), len(lines))
    while at > 0 and not lines[at - 1].strip():
        at -= 1
    lines.insert(at, f"{name} : {1 if enabled else 0}")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def stage_elmp(target: Path, build: Path, flavor: str) -> None:
    dll = build / flavor / "dlls" / "main.dll"
    if not dll.exists():
        sys.exit(f"{dll} not found; run build.bat")
    if target.exists():
        shutil.rmtree(target)
    (target / "dlls").mkdir(parents=True)
    shutil.copy2(dll, target / "dlls" / "main.dll")
    if flavor == "dev" and dll.with_suffix(".pdb").exists():
        shutil.copy2(dll.with_suffix(".pdb"), target / "dlls" / "main.pdb")
    shutil.copytree(REPO / "data", target / "data")
    shutil.copy2(REPO / "elmp.json", target / "elmp.json")
    lua = [p for p in target.rglob("*.lua")]
    if lua:
        shutil.rmtree(target)
        sys.exit(f"refusing: .lua files in the ELMP deployment: {lua}")
    print(f"deployed ELMP {flavor} -> {target} (main.dll sha256 {sha256(target / 'dlls' / 'main.dll')[:16]}…)")


def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("instance")
    p.add_argument("--flavor", choices=["dev", "shipping"], default="dev")
    p.add_argument("--config", default="Game__Shipping__Win64")
    p.add_argument("--elmp", choices=["on", "off"], default="on")
    p.add_argument("--probe", choices=["on", "off"], default="off")
    a = p.parse_args()

    inst = devconfig.instance(a.instance)
    mods = inst["mods"]
    if not inst["exe"].exists() or not (mods / "mods.txt").exists():
        sys.exit(f"{inst['root']} is not a UE4SS game instance")
    if instances.processes(inst):
        sys.exit(f"instance '{a.instance}' is running; stop it first (DLLs are never replaced in a running instance)")
    build = REPO / "build" / a.config

    stage_elmp(mods / "ELMP", build, a.flavor)
    set_mod(mods, "ELMP", a.elmp == "on")
    if (mods / "ElmpProbe").exists():
        set_mod(mods, "ElmpProbe", False)
    probe = mods / "ElmpFrameProbe"
    if a.probe == "on":
        if probe.exists():
            shutil.rmtree(probe)
        (probe / "dlls").mkdir(parents=True)
        shutil.copy2(build / "frameprobe" / "dlls" / "main.dll", probe / "dlls" / "main.dll")
        print(f"deployed ElmpFrameProbe -> {probe}")
    if probe.exists():
        set_mod(mods, "ElmpFrameProbe", a.probe == "on")
    print(f"mods.txt: ELMP={a.elmp} ElmpFrameProbe={a.probe}")


if __name__ == "__main__":
    main()
