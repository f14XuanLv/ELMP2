"""Regenerate data/catalog/items.json, data/icons and data/fonts from the local game paks (dev tool).

Paths come from devconfig.json (paks, usmap, cue4parse). Requires the .NET 10 SDK.
"""

import argparse
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import devconfig  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--size", type=int, help="icon size in pixels when regenerating the catalog (default: 128)")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--triggers", type=int, choices=[1], help="list trigger classes without regenerating data")
    mode.add_argument("--dump", metavar="ASSET", help="dump packages with this asset name as JSON")
    parser.add_argument("--out", metavar="DIR", help="output directory required with --dump")
    args = parser.parse_args()
    if args.size is not None and args.size <= 0:
        parser.error("--size must be a positive integer")
    if args.size is not None and (args.triggers is not None or args.dump is not None):
        parser.error("--size is only valid when regenerating the catalog")
    if args.dump is not None and not args.dump.strip():
        parser.error("--dump requires a nonempty asset name")
    if args.dump is not None and (args.out is None or not args.out.strip()):
        parser.error("--out is required with --dump")
    if args.out is not None and args.dump is None:
        parser.error("--out is only valid with --dump")

    options = []
    if args.size is not None:
        options.extend(["--size", str(args.size)])
    if args.triggers is not None:
        options.extend(["--triggers", str(args.triggers)])
    if args.dump is not None:
        options.extend(["--dump", args.dump, "--out", args.out])

    cfg = devconfig.load()
    for key in ("paks", "usmap", "cue4parse"):
        if not Path(cfg.get(key, "")).exists():
            raise SystemExit(f"devconfig '{key}' not found: {cfg.get(key)!r} (devconfig.json / devconfig.local.json)")
    project = Path(__file__).with_name("ElmpCatalog.csproj")
    cmd = ["dotnet", "run", "--project", str(project), "-c", "Release", f"-p:CUE4ParseRoot={cfg['cue4parse']}", "--",
           "--paks", cfg["paks"], "--mappings", cfg["usmap"], "--repo", str(devconfig.REPO), *options]
    return subprocess.call(cmd)


if __name__ == "__main__":
    raise SystemExit(main())
