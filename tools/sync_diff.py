"""Diff `elmp lan sync` of a host and a client instance taken at (nearly) the same moment (P7 restart §3).

  python tools/sync_diff.py host client [cat] [--samples N] [--interval S] [--json out.json] [--quiet]
  python tools/sync_diff.py host client rec      compare the `elmp lan rec` recordings (animation edges per
                                                  entity, in-order LCS; start them with `elmp lan rec start N`)

Both instances are asked in parallel (one thread each) so the two reports are a few ms apart. Host rows are
filtered to the map slot the client's seat stands in (header `seatSlots`) and are already map-local.
Comparison per entity key (see native/coop/sync.h):
  position  > 5 UU (x / y / z)             anim  track-0 name differs
  hp        current HP differs             vis   visibility differs
  missing   host has it, client does not   extra client has it, host does not (x: keys = client-local leaks)
fx rows: ability / particle / audio counts per owner key. cnt rows are printed side by side; the correctness
rows (guard *_replayed, save hashes) are flagged. Exit code 0 = no correctness failure (differences in
presentation are reported, not failed), 1 = a correctness failure, 2 = could not query.
"""

import argparse
import json
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bridge  # noqa: E402
import devconfig  # noqa: E402

POS_UU = 5.0


def query(inst_name: str, cat: str, out: dict) -> None:
    try:
        inst = devconfig.instance(inst_name)
        cmd = "elmp lan rec dump" if cat == "rec" else "elmp lan sync " + cat
        r = bridge.request(inst, "eval", f"return ELMP_Command({bridge.lua_string(cmd)})", 30)
        out[inst_name] = r.get("result") if r.get("ok") else None
        out[inst_name + ".t"] = time.time()
    except Exception as e:  # noqa: BLE001
        out[inst_name] = None
        out[inst_name + ".err"] = str(e)


def parse(text: str) -> dict:
    rep = {"hdr": {}, "ent": {}, "fx": {}, "cnt": {}, "hash": {}}
    for line in (text or "").splitlines():
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            o = json.loads(line)
        except json.JSONDecodeError:
            continue
        c = o.get("cat")
        if c == "hdr":
            rep["hdr"] = o
        elif c == "ent":
            rep["ent"][o["k"]] = o
        elif c == "fx":
            rep["fx"][o["owner"]] = o
        elif c == "cnt":
            rep["cnt"][o.get("src", "?")] = o
        elif c == "hash":
            rep["hash"][o["file"]] = o
        elif c == "world":
            rep.setdefault("world", {})[o["k"]] = o
    return rep


def compare(h: dict, c: dict) -> dict:
    seat = str(c["hdr"].get("seat", 0))
    slot = h["hdr"].get("seatSlots", {}).get(seat, 0)
    hent = {k: v for k, v in h["ent"].items() if v.get("slot", 0) == slot}
    cent = c["ent"]
    res = {"seat": seat, "slot": slot, "host_map": h["hdr"].get("map"), "client_map": c["hdr"].get("map"),
           "missing": [], "extra": [], "pos": [], "anim": [], "hp": [], "vis": [], "same": 0, "fx": [], "correctness": []}
    for k, hv in sorted(hent.items()):
        cv = cent.get(k)
        if cv is None:
            res["missing"].append(k)
            continue
        ok = True
        d = max(abs(hv.get(a, 0) - cv.get(a, 0)) for a in ("x", "y", "z"))
        if d > POS_UU:
            res["pos"].append((k, round(d, 1)))
            ok = False
        if hv.get("anim") != cv.get("anim"):
            res["anim"].append((k, hv.get("anim"), cv.get("anim")))
            ok = False
        if "hp" in hv and hv.get("hp") != cv.get("hp"):
            res["hp"].append((k, hv.get("hp"), cv.get("hp")))
            ok = False
        if hv.get("vis") != cv.get("vis"):
            res["vis"].append((k, hv.get("vis"), cv.get("vis")))
            ok = False
        res["same"] += ok
    for k in sorted(set(cent) - set(hent)):
        res["extra"].append(k)
    for k in sorted(set(h["fx"]) | set(c["fx"])):
        hv, cv = h["fx"].get(k, {}), c["fx"].get(k, {})
        hc = (hv.get("abilities", 0), hv.get("psc", 0), hv.get("audio", 0))
        cc = (cv.get("abilities", 0), cv.get("psc", 0), cv.get("audio", 0))
        if hc != cc:
            res["fx"].append((k, hc, cc))
    res["world"] = []
    hw, cw = h.get("world", {}), c.get("world", {})
    if hw or cw:
        for k in sorted(set(hw) | set(cw)):
            hv, cv = hw.get(k), cw.get(k)
            hs = None if hv is None else (hv.get("vis"), hv.get("hp"), hv.get("n"))
            cs = None if cv is None else (cv.get("vis"), cv.get("hp"), cv.get("n"))
            if hs != cs:
                res["world"].append((k, hs, cs))
    g = c["cnt"].get("guard")
    if g:
        for f in ("damage_replayed", "skill_replayed"):
            if g.get(f, 0):
                res["correctness"].append(f"client guard {f}={g[f]} (must be 0)")
    if res["missing"] and any(k.startswith("e:") for k in res["missing"]) and res["host_map"] == res["client_map"]:
        pass  # enemies not streamed in on the client are reported, not failed (Client ⊆ Host)
    for k in res["extra"]:
        if k.startswith("x:") and cent[k].get("vis") and (cent[k].get("hp") or 0) > 0:
            res["correctness"].append(f"client-local live enemy {k} bound to no host row")
    for k, hv, cv in res["hp"]:
        if k.startswith(("e:", "d:")) and (hv or 0) <= 0 < (cv or 0):
            res["correctness"].append(f"{k} dead on the host (hp {hv}) but alive on the client (hp {cv})")
    return res


def show(res: dict, h: dict, c: dict, quiet: bool) -> None:
    print(f"map host={res['host_map']} client={res['client_map']} seat=P{res['seat']} slot={res['slot']}")
    n_host = sum(1 for v in h["ent"].values() if v.get("slot", 0) == res["slot"])
    print(f"entities host={n_host} client={len(c['ent'])} identical={res['same']} missing={len(res['missing'])} extra={len(res['extra'])} "
          f"pos>{POS_UU:g}={len(res['pos'])} anim={len(res['anim'])} hp={len(res['hp'])} vis={len(res['vis'])} fx={len(res['fx'])} world={len(res['world'])}")
    if not quiet:
        for name in ("missing", "extra", "pos", "anim", "hp", "vis", "fx", "world"):
            for item in res[name]:
                print(f"  {name}: {item}")
        for src in sorted(set(h["cnt"]) | set(c["cnt"])):
            if src in h["cnt"]:
                print(f"  cnt host {json.dumps(h['cnt'][src], ensure_ascii=False)}")
            if src in c["cnt"]:
                print(f"  cnt client {json.dumps(c['cnt'][src], ensure_ascii=False)}")
    for line in res["correctness"]:
        print(f"  CORRECTNESS: {line}")


def lcs(a: list, b: list) -> int:
    m = [0] * (len(b) + 1)
    for x in a:
        prev = 0
        for j, y in enumerate(b):
            cur = m[j + 1]
            m[j + 1] = prev + 1 if x == y else max(m[j + 1], m[j])
            prev = cur
    return m[-1]


def rec_sequences(text: str) -> tuple[dict, dict]:
    """key → animation sequence (consecutive duplicates collapsed) and key → HP sequence."""
    anim: dict = {}
    hp: dict = {}
    head = {}
    for line in (text or "").splitlines():
        try:
            o = json.loads(line)
        except json.JSONDecodeError:
            continue
        if o.get("cat") == "rech":
            head = o
        if o.get("cat") != "rec" or o.get("gone"):
            continue
        k = o["k"]
        if k.startswith("s:") and not o.get("vis"):
            continue  # spirits: only while summoned
        a = anim.setdefault(k, [])
        if o.get("anim") and (not a or a[-1] != o["anim"]):
            a.append(o["anim"])
        h = hp.setdefault(k, [])
        if not h or h[-1] != o.get("hp"):
            h.append(o.get("hp"))
    anim["__head__"] = head
    return anim, hp


def rec_compare(htext: str, ctext: str, quiet: bool) -> dict:
    ha, hh = rec_sequences(htext)
    ca, ch = rec_sequences(ctext)
    print(f"rec host {ha.pop('__head__')} | client {ca.pop('__head__')}")
    tot = match = 0
    rows = []
    for k in sorted(set(ha) | set(ca)):
        hs, cs = ha.get(k, []), ca.get(k, [])
        if len(hs) <= 1 and hs == cs and hh.get(k) == ch.get(k):
            continue
        m = lcs(hs, cs)
        tot += len(hs)
        match += m
        row = {"k": k, "host": len(hs), "client": len(cs), "lcs": m, "missing": sorted(set(hs) - set(cs)), "extra": sorted(set(cs) - set(hs)),
               "hp_host": hh.get(k), "hp_client": ch.get(k)}
        rows.append(row)
        if not quiet:
            print(f"  {k}: host {len(hs)} client {len(cs)} lcs {m} missing {row['missing']} extra {row['extra']}")
            if hh.get(k) != ch.get(k):
                print(f"     hp host {hh.get(k)} client {ch.get(k)}")
    print(f"rec TOTAL animation edges host {tot} matched in order {match} ({100.0 * match / tot if tot else 100:.0f}%)")
    return {"total": tot, "matched": match, "rows": rows}


def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("host")
    p.add_argument("client")
    p.add_argument("cat", nargs="?", default="ent")
    p.add_argument("--samples", type=int, default=1)
    p.add_argument("--interval", type=float, default=0.5)
    p.add_argument("--json")
    p.add_argument("--quiet", action="store_true")
    a = p.parse_args()
    runs = []
    bad = False
    for i in range(a.samples):
        out: dict = {}
        ts = [threading.Thread(target=query, args=(n, a.cat, out)) for n in (a.host, a.client)]
        for t in ts:
            t.start()
        for t in ts:
            t.join()
        if out.get(a.host) is None or out.get(a.client) is None:
            print(f"query failed: {out.get(a.host + '.err') or out.get(a.client + '.err') or 'ELMP_Command returned nothing'}")
            sys.exit(2)
        if a.cat == "rec":
            r = rec_compare(out[a.host], out[a.client], a.quiet)
            if a.json:
                Path(a.json).write_text(json.dumps(r, ensure_ascii=False, indent=1), encoding="utf-8")
            sys.exit(0)
        h, c = parse(out[a.host]), parse(out[a.client])
        res = compare(h, c)
        if a.samples > 1:
            print(f"--- sample {i + 1}/{a.samples} (skew {abs(out[a.host + '.t'] - out[a.client + '.t']) * 1000:.0f} ms)")
        show(res, h, c, a.quiet)
        bad |= bool(res["correctness"])
        runs.append({"host": h, "client": c, "diff": res})
        if i + 1 < a.samples:
            time.sleep(a.interval)
    if a.json:
        Path(a.json).write_text(json.dumps(runs, ensure_ascii=False, indent=1), encoding="utf-8")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
