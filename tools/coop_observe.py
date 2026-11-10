"""Record a live isolated party, including the period before a disconnect. Reads only."""

import argparse
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime
import hashlib
import json
import shutil
import time
from pathlib import Path

import bridge
import devconfig


def observe(name, output, deadline, interval):
    inst = devconfig.instance(name)
    log = inst["root"] / "ue4ss" / "UE4SS.log"
    dll = inst["mods"] / "ELMP" / "dlls" / "main.dll"
    folder = output / name
    folder.mkdir()
    signature = hashlib.sha256(dll.read_bytes()).hexdigest()
    cursor = log.stat().st_size if log.exists() else 0
    first = None
    sample = """
        return {world=ELMP_Command('elmp status'), session=ELMP_Command('elmp lan status'),
                causal=ELMP_Command('elmp causal'), commands=ELMP_Command('elmp commandctx')}
    """
    with (folder / "samples.jsonl").open("w", encoding="utf8", buffering=1) as samples:
        while time.monotonic() < deadline:
            start = time.monotonic()
            row = {"time": time.time(), "monotonic": start}
            if log.exists():
                with log.open("rb") as f:
                    if f.seek(0, 2) < cursor:
                        cursor = 0
                    f.seek(cursor)
                    lines = f.read().decode("utf8", errors="replace").splitlines()
                    cursor = f.tell()
                row["events"] = [line for line in lines if "[ELMP]" in line or "Fatal error" in line]
                if first is None:
                    first = next((line for line in lines if any(word in line for word in
                                  ("coop: leaving (", "coop: playback refresh", "Fatal error", "peer closed:"))), None)
                    if first:
                        shutil.copy2(log, folder / "first-event.log")
            try:
                row["reply"] = bridge.request(inst, "eval", sample, timeout=3)
            except (OSError, TimeoutError) as error:
                row["bridge_error"] = str(error)
            samples.write(json.dumps(row, ensure_ascii=False) + "\n")
            time.sleep(max(0, min(deadline - time.monotonic(), interval - (time.monotonic() - start))))
    if log.exists():
        shutil.copy2(log, folder / "UE4SS.log")
    return {"instance": name, "dll_sha256": signature, "first_event": first}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--instances", nargs="+", default=["host", "client1", "client2", "client3"])
    parser.add_argument("--seconds", type=float, default=120)
    parser.add_argument("--interval", type=float, default=1)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    if args.seconds <= 0 or args.interval <= 0 or len(set(args.instances)) != len(args.instances):
        parser.error("supply distinct instances and positive durations")
    folder = args.out or devconfig.REPO / "build" / ("coop-observe-" + datetime.now().strftime("%Y%m%d-%H%M%S"))
    folder.mkdir(parents=True, exist_ok=False)
    deadline = time.monotonic() + args.seconds
    with ThreadPoolExecutor(len(args.instances)) as workers:
        runs = [workers.submit(observe, name, folder, deadline, args.interval) for name in args.instances]
        summary = [run.result() for run in runs]
    (folder / "summary.json").write_text(json.dumps(summary, indent=2, ensure_ascii=False), encoding="utf8")
    print(folder)
    for row in summary:
        print(row["instance"], row["first_event"] or "no incident in captured log interval")


if __name__ == "__main__":
    main()
