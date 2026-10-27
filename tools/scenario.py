"""Fixed coop scenarios on the isolated instances (P7 restart §3): backup → load slot 0 → lan host/join →
scripted actions (bridge-driven) → `elmp lan rec` + `elmp lan sync` checkpoints (tools/sync_diff.py) →
paired screenshots → client save-hash check → restore.

  python tools/scenario.py <name> [--host host] [--client client] [--client2 client2] [--slot 0] [--keep]
  python tools/scenario.py list

Scenarios: slime-fight, guest-fight, world-objects, death-respawn, map-change, join-reconnect.
Output: %TEMP%\\elmp_scn\\<name>-<time>\\{report.txt, <step>.json}; screenshots
<userDir>\\Saved\\ELMP\\pilot\\shots\\<name>-<step>-<instance>.png. The instances must be running (UEBridge).
Writes to the game world are test helpers (registered in docs/kb/sync-coverage.md): tp / kill / press via
`elmp`, `DoDamage` / `OnInteract` via eval. Saves the host may write (death / rest) are restored afterwards.
Exit code 1 when a correctness row fails (client settlement leak, client save files changed, host-dead
enemy alive on the client, client-local live enemy).
"""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bridge  # noqa: E402
import devconfig  # noqa: E402
import sync_diff  # noqa: E402

PY = sys.executable
TOOLS = Path(__file__).resolve().parent


class Run:
    def __init__(self, name: str, a):
        self.name = name
        self.a = a
        self.host = a.host
        self.clients = [c for c in (a.client, a.client2) if c]
        self.dir = Path(os.environ.get("TEMP", ".")) / "elmp_scn" / f"{name}-{time.strftime('%H%M%S')}"
        self.dir.mkdir(parents=True, exist_ok=True)
        self.report = []
        self.fail = []
        self.code = "scn" + name.replace("-", "")[:8]

    # --- plumbing
    def log(self, s: str) -> None:
        print(s, flush=True)
        self.report.append(s)

    def request(self, inst: str, code: str) -> dict:
        # UEBridge occasionally fails to replace response.json ("response write failed: File exists"): one retry
        for attempt in range(2):
            try:
                return bridge.request(devconfig.instance(inst), "eval", code, 20)
            except TimeoutError:
                if attempt:
                    raise
                self.log(f"  (UEBridge timeout on {inst}, retrying)")
        return {}

    def elmp(self, inst: str, cmd: str, quiet: bool = True) -> str:
        r = self.request(inst, f"return ELMP_Command({bridge.lua_string(cmd)})")
        out = r.get("result") if r.get("ok") else json.dumps(r, ensure_ascii=False)
        if not quiet:
            self.log(f"[{inst}] {cmd}: {str(out).strip()[:300]}")
        return str(out)

    def ev(self, inst: str, lua: str) -> str:
        r = self.request(inst, lua)
        return json.dumps(r.get("result"), ensure_ascii=False) if r.get("ok") else json.dumps(r, ensure_ascii=False)

    def press(self, inst: str, slot: int, cmd: str, n: int = 1, gap: float = 0.35) -> None:
        for _ in range(n):
            self.elmp(inst, f"elmp press {slot} {cmd}")
            time.sleep(gap)

    def shot(self, step: str) -> None:
        for inst in [self.host] + self.clients:
            subprocess.run([PY, str(TOOLS / "capture.py"), inst, f"{self.name}-{step}"], capture_output=True)
        self.log(f"  shot {self.name}-{step}-{{{','.join([self.host] + self.clients)}}}.png")

    def status_map(self, inst: str) -> str:
        for line in self.elmp(inst, "elmp status").splitlines():
            if line.startswith("world:"):
                return line
        return "?"

    # --- saves
    def saved(self, inst: str) -> Path:
        return Path(devconfig.instance(inst)["userDir"]) / "Saved"

    def files(self, inst: str) -> dict:
        out = {}
        for sub in ("SaveGames", "ELMP/ELMP"):
            d = self.saved(inst) / sub
            if d.exists():
                for f in d.iterdir():
                    if f.is_file():
                        out[f"{sub}/{f.name}"] = hashlib.sha256(f.read_bytes()).hexdigest()[:16]
        return out

    def backup(self) -> None:
        self.bak = self.dir / "backup"
        for inst in [self.host] + self.clients:
            for sub in ("SaveGames", "ELMP/ELMP"):
                src = self.saved(inst) / sub
                if src.exists():
                    shutil.copytree(src, self.bak / inst / sub)
        self.before = {inst: self.files(inst) for inst in self.clients}

    def restore(self) -> None:
        for inst in self.clients:
            after = self.files(inst)
            changed = sorted(k for k in set(after) | set(self.before[inst]) if after.get(k) != self.before[inst].get(k) and not k.endswith(".bak"))
            if changed:
                self.fail.append(f"C03 {inst} saved files changed: {changed}")
            self.log(f"C03 {inst} saved files changed: {changed or 'none'}")
        for inst in [self.host] + self.clients:
            for sub in ("SaveGames", "ELMP/ELMP"):
                src, dst = self.bak / inst / sub, self.saved(inst) / sub
                if src.exists():
                    for f in dst.iterdir() if dst.exists() else []:
                        if f.is_file() and not (src / f.name).exists():
                            f.unlink()
                    shutil.copytree(src, dst, dirs_exist_ok=True)
        self.log("restored saves of " + ", ".join([self.host] + self.clients))

    # --- world setup
    def load_slot(self, inst: str) -> None:
        self.ev(inst, "local gs=StaticFindObject('/Script/Engine.Default__GameplayStatics') gs:OpenLevel(FindFirstOf('GameInstance'), FName('TitleMap'), true, '') return 'ok'")
        time.sleep(8)
        self.ev(inst, f"local t=FindFirstOf('TitleMap_C') if t and t:IsValid() then t:OnSaveSlotSelected({self.a.slot}, 2) return 'loading' end return 'no title'")

    def wait_settled(self, inst: str, map_id: str = "", timeout: float = 60) -> bool:
        end = time.time() + timeout
        while time.time() < end:
            s = self.status_map(inst)
            if "settled" in s and (not map_id or map_id in s):
                return True
            time.sleep(1.5)
        self.log(f"  {inst} not settled{' in ' + map_id if map_id else ''}: {self.status_map(inst)}")
        return False

    def fresh(self, insts: list) -> None:
        for inst in insts:
            self.elmp(inst, "elmp lan leave")
        time.sleep(1)
        for inst in insts:
            self.load_slot(inst)
        for inst in insts:
            self.wait_settled(inst, "map_church_08")
        time.sleep(3)

    def host_session(self) -> None:
        self.elmp(self.host, f"elmp lan host {self.code} 7777", quiet=False)
        self.elmp(self.host, "elmp set recall 0")
        self.elmp(self.host, "elmp pad allow1 on")

    def join(self, inst: str) -> None:
        self.elmp(inst, f"elmp lan join 127.0.0.1 {self.code}", quiet=False)
        self.elmp(inst, "elmp pad allow1 on")
        time.sleep(5)

    def to_church07(self, slimes: bool = True) -> None:
        self.elmp(self.host, "elmp tp 1 -4273 170", quiet=False)
        self.wait_settled(self.host, "map_church_07")
        for c in self.clients:
            self.wait_settled(c, "map_church_07")
        time.sleep(4)
        if slimes:
            self.elmp(self.host, "elmp tp 1 -1600 -440", quiet=False)
            time.sleep(3)

    # --- evidence
    def rec_start(self, frames: int) -> None:
        for inst in [self.host] + self.clients:
            self.elmp(inst, f"elmp lan rec start {frames}")

    def rec_check(self, step: str, wait: float = 0) -> None:
        time.sleep(wait)
        for c in self.clients:
            h = self.elmp(self.host, "elmp lan rec dump")
            t = self.elmp(c, "elmp lan rec dump")
            self.log(f"== rec {step} host vs {c}")
            r = self._capture(lambda: sync_diff.rec_compare(h, t, False))
            (self.dir / f"{step}-rec-{c}.json").write_text(json.dumps(r, ensure_ascii=False, indent=1), encoding="utf-8")

    def checkpoint(self, step: str, cat: str = "all") -> dict:
        res = None
        for c in self.clients:
            out: dict = {}
            import threading
            ts = [threading.Thread(target=sync_diff.query, args=(n, cat, out)) for n in (self.host, c)]
            for t in ts:
                t.start()
            for t in ts:
                t.join()
            h, cl = sync_diff.parse(out.get(self.host) or ""), sync_diff.parse(out.get(c) or "")
            res = sync_diff.compare(h, cl)
            self.log(f"== sync {step} host vs {c}")
            self._capture(lambda: sync_diff.show(res, h, cl, False))
            for line in res["correctness"]:
                self.fail.append(f"{step} {c}: {line}")
            (self.dir / f"{step}-{c}.json").write_text(json.dumps({"host": h, "client": cl, "diff": res}, ensure_ascii=False, indent=1), encoding="utf-8")
        return res or {}

    def _capture(self, fn):
        import contextlib
        import io
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            r = fn()
        for line in buf.getvalue().splitlines():
            self.log(line)
        return r

    def perf(self, step: str) -> None:
        for inst in [self.host] + self.clients:
            for line in self.elmp(inst, "elmp perf").splitlines():
                if line.startswith(("frame.elmp_us", "frame.interval_us")):
                    self.log(f"  perf {step} {inst} {line}")

    def perf_reset(self) -> None:
        for inst in [self.host] + self.clients:
            self.elmp(inst, "elmp perf reset")


# --- scenarios ----------------------------------------------------------------------------------------

def slime_fight(r: Run) -> None:
    r.fresh([r.host] + r.clients)
    r.host_session()
    for c in r.clients:
        r.join(c)
    r.to_church07()
    r.elmp(r.host, "elmp vitals 1 restore")
    r.checkpoint("start")
    r.perf_reset()
    r.rec_start(900)
    r.press(r.host, 1, "ATTACK", 10)
    r.shot("combo")
    r.press(r.host, 1, "DODGE", 1, 0.7)
    r.press(r.host, 1, "JUMP", 1, 0.3)
    r.press(r.host, 1, "ATTACK", 1, 0.9)
    r.press(r.host, 1, "ATTACK", 6, 0.6)
    r.shot("end")
    r.rec_check("fight", wait=4)
    r.checkpoint("end")
    r.perf("fight")
    # the recorder costs ~80 us per frame on each side: the frame-time gate is measured on a burst without it
    r.elmp(r.host, "elmp vitals 1 restore")
    r.perf_reset()
    r.press(r.host, 1, "ATTACK", 12, 0.5)
    r.perf("fight-norec")


def guest_fight(r: Run) -> None:
    r.fresh([r.host] + r.clients)
    r.host_session()
    for c in r.clients:
        r.join(c)
    r.to_church07(slimes=False)
    r.elmp(r.host, "elmp tp 2 -1600 -444", quiet=False)
    time.sleep(3)
    r.checkpoint("start")
    r.perf_reset()
    r.rec_start(900)
    c = r.clients[0]
    for _ in range(10):
        r.elmp(c, "elmp press 1 ATTACK")
        time.sleep(0.4)
    r.shot("attack")
    r.elmp(c, "elmp press 1 JUMP")
    time.sleep(4)
    r.rec_check("attack")
    r.checkpoint("attack")
    r.perf("attack")
    r.perf_reset()
    for _ in range(12):
        r.elmp(c, "elmp press 1 ATTACK")
        time.sleep(0.5)
    r.perf("attack-norec")
    r.rec_start(600)
    r.elmp(r.host, "elmp kill 2 9999", quiet=False)
    time.sleep(1.5)
    r.shot("death")
    time.sleep(6)
    r.rec_check("death")
    r.checkpoint("respawn")


def world_objects(r: Run) -> None:
    r.fresh([r.host] + r.clients)
    r.host_session()
    for c in r.clients:
        r.join(c)
    r.to_church07()
    r.checkpoint("start", "world")
    # breakables: vanilla DoDamage on the host (test helper) → drops; enemies killed → experience orbs
    r.log("  breakables: " + r.ev(r.host, "local n=0 for _,b in ipairs(FindAllOf('Breakable') or {}) do if b:IsValid() and b.HPComponent:IsValid() and b.HPComponent:GetCurrHP()>0 then b.HPComponent:DoDamage(FindFirstOf('BP_p0000_Lily_C'), 9999, false, false) n=n+1 end end return n"))
    r.rec_start(600)
    r.press(r.host, 1, "ATTACK", 12, 0.4)
    time.sleep(1)
    r.shot("broken")
    r.checkpoint("broken", "world")
    r.rec_check("broken", wait=3)
    r.checkpoint("after")
    # chair: host P1 back to church_08, rest through the chair's vanilla interact (test helper; writes the host save)
    r.elmp(r.host, "elmp lan leave")
    for c in r.clients:
        r.wait_settled(c, "", 30)
    r.fresh([r.host] + r.clients)
    r.host_session()
    for c in r.clients:
        r.join(c)
    r.log("  rest: " + r.ev(r.host, "local c=FindFirstOf('BP_Interactable_RestingPoint_C') local pc=FindFirstOf('PC_Base_C') if not c or not c:IsValid() then return 'no chair' end local ok,e=pcall(function() c:OnInteract(pc) end) return tostring(ok)..' '..tostring(e)"))
    r.rec_start(480)
    time.sleep(6)
    r.shot("rest")
    r.rec_check("rest")
    r.checkpoint("rest")
    r.ev(r.host, "for _,w in ipairs(FindAllOf('WBP_RestMenu_C') or {}) do if w:IsValid() then w:RemoveFromParent() end end return 'ok'")


def death_respawn(r: Run) -> None:
    r.fresh([r.host] + r.clients)
    r.host_session()
    for c in r.clients:
        r.join(c)
    r.to_church07()
    r.checkpoint("start")
    r.rec_start(600)
    r.log("  host P1 death: " + r.ev(r.host, "local p=FindFirstOf('BP_p0000_Lily_C') p.HPComponent:DoDamage(p, 99999, false, false) return 'ok'"))
    time.sleep(2)
    r.shot("p1-dead")
    r.rec_check("p1-death", wait=6)
    r.wait_settled(r.host, "map_church_08", 40)
    for c in r.clients:
        r.wait_settled(c, "map_church_08", 40)
    time.sleep(4)
    r.shot("p1-respawn")
    r.checkpoint("p1-respawn")
    r.rec_start(600)
    r.elmp(r.host, "elmp kill 2 9999", quiet=False)
    time.sleep(1.5)
    r.shot("guest-dead")
    r.rec_check("guest-death", wait=6)
    r.checkpoint("guest-respawn")
    r.to_church07()
    r.checkpoint("reenter")


def map_change(r: Run) -> None:
    r.fresh([r.host] + r.clients)
    r.host_session()
    for c in r.clients:
        r.join(c)
    r.checkpoint("church08")
    r.to_church07(slimes=False)
    r.shot("follow")
    r.checkpoint("church07")
    # background map: host P1 back to church_08 first (fresh session), then the guest seat walks into church_07
    r.elmp(r.host, "elmp lan leave")
    r.fresh([r.host] + r.clients)
    r.host_session()
    for c in r.clients:
        r.join(c)
    r.elmp(r.host, "elmp perm 2 travel on", quiet=False)
    r.elmp(r.host, "elmp tp 2 -4273 170", quiet=False)
    time.sleep(3)
    for c in r.clients[:1]:
        r.wait_settled(c, "map_church_07", 60)
    time.sleep(5)
    r.shot("background")
    r.checkpoint("background")
    r.log("  maps: " + r.elmp(r.host, "elmp lan status").split("maps:")[-1][:200].replace("\n", " "))
    r.elmp(r.host, "elmp tp 1 -4273 170", quiet=False)
    r.wait_settled(r.host, "map_church_07")
    time.sleep(6)
    r.shot("handoff")
    r.checkpoint("handoff")


def join_reconnect(r: Run) -> None:
    c = r.clients[0]
    r.fresh([r.host] + r.clients)
    r.host_session()
    r.to_church07()
    r.join(c)
    r.wait_settled(c, "map_church_07")
    time.sleep(5)
    r.shot("late-join")
    r.checkpoint("late-join")
    # unexpected disconnect: end the client process (no F_CLOSE), restart, reload, join again
    subprocess.run([PY, str(TOOLS / "instance.py"), "stop", c], capture_output=True)
    time.sleep(10)
    host_lines = r.elmp(r.host, "elmp lan status").splitlines()
    r.log("  host after drop: " + " | ".join(l for l in host_lines if l.startswith(("host:", "session"))))
    subprocess.run([PY, str(TOOLS / "instance.py"), "start", c], capture_output=True)
    subprocess.run([PY, str(TOOLS / "bridge.py"), c, "wait-ready", "--timeout", "120"], capture_output=True)
    time.sleep(8)
    r.ev(c, f"local t=FindFirstOf('TitleMap_C') if t and t:IsValid() then t:OnSaveSlotSelected({r.a.slot}, 2) return 'loading' end return 'no title'")
    r.wait_settled(c, "map_church_08", 90)
    time.sleep(3)
    r.join(c)
    r.wait_settled(c, "map_church_07")
    time.sleep(5)
    r.shot("reconnect")
    r.checkpoint("reconnect")


SCENARIOS = {"slime-fight": slime_fight, "guest-fight": guest_fight, "world-objects": world_objects,
             "death-respawn": death_respawn, "map-change": map_change, "join-reconnect": join_reconnect}


def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("name")
    p.add_argument("--host", default="host")
    p.add_argument("--client", default="client")
    p.add_argument("--client2", default="")
    p.add_argument("--slot", type=int, default=0)
    p.add_argument("--keep", action="store_true", help="do not restore the saves (debugging)")
    a = p.parse_args()
    if a.name == "list":
        print("\n".join(SCENARIOS))
        return
    r = Run(a.name, a)
    r.log(f"scenario {a.name} → {r.dir}")
    r.backup()
    try:
        SCENARIOS[a.name](r)
    finally:
        for c in r.clients:
            try:
                r.log(f"final guard {c}: " + [l for l in r.elmp(c, "elmp lan sync cnt").splitlines() if '"guard"' in l][0])
            except Exception:  # noqa: BLE001
                pass
        if not a.keep:
            r.restore()
        r.log("RESULT: " + ("FAIL " + "; ".join(r.fail) if r.fail else "correctness rows green"))
        (r.dir / "report.txt").write_text("\n".join(r.report) + "\n", encoding="utf-8")
    sys.exit(1 if r.fail else 0)


if __name__ == "__main__":
    main()
