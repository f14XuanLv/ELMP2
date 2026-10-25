"""devconfig.json (+ devconfig.local.json overrides): the only place development paths live.

Strings may contain {game}, expanded to the "game" value. Instances are resolved to absolute paths.
"""

import json
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
EXE = "EnderLiliesSteam-Win64-Shipping.exe"


def _merge(base: dict, over: dict) -> dict:
    for k, v in over.items():
        base[k] = _merge(base.get(k, {}), v) if isinstance(v, dict) and isinstance(base.get(k), dict) else v
    return base


def _expand(value, game: str):
    if isinstance(value, str):
        return value.replace("{game}", game)
    if isinstance(value, dict):
        return {k: _expand(v, game) for k, v in value.items()}
    return value


def load() -> dict:
    cfg = json.loads((REPO / "devconfig.json").read_text(encoding="utf-8"))
    local = REPO / "devconfig.local.json"
    if local.exists():
        _merge(cfg, json.loads(local.read_text(encoding="utf-8")))
    return _expand(cfg, cfg["game"])


def instance(name: str) -> dict:
    cfg = load()
    if name not in cfg["instances"]:
        raise SystemExit(f"unknown instance '{name}' (devconfig.json: {', '.join(cfg['instances'])})")
    inst = dict(cfg["instances"][name])
    inst["name"] = name
    root = Path(inst["root"])
    inst["root"] = root
    inst["exe"] = root / EXE
    inst["mods"] = root / "ue4ss" / "Mods"
    inst["bridge"] = root / "ue4ss" / "bridge"
    inst["userDir"] = Path(inst["userDir"])
    inst["saved"] = inst["userDir"] / "Saved"
    return inst
