"""Regenerate data/catalog/items.json, data/icons and data/fonts from the local game paks (dev tool).

Paths come from devconfig.json (paks, usmap, cue4parse). Requires the .NET 10 SDK.
"""

import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import devconfig  # noqa: E402


def main() -> int:
    cfg = devconfig.load()
    for key in ("paks", "usmap", "cue4parse"):
        if not Path(cfg.get(key, "")).exists():
            raise SystemExit(f"devconfig '{key}' not found: {cfg.get(key)!r} (devconfig.json / devconfig.local.json)")
    project = Path(__file__).with_name("ElmpCatalog.csproj")
    cmd = ["dotnet", "run", "--project", str(project), "-c", "Release", f"-p:CUE4ParseRoot={cfg['cue4parse']}", "--",
           "--paks", cfg["paks"], "--mappings", cfg["usmap"], "--repo", str(devconfig.REPO), *sys.argv[1:]]
    return subprocess.call(cmd)


if __name__ == "__main__":
    raise SystemExit(main())
