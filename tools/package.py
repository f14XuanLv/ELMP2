"""Release packages (C1.3): dist/ELMP-<version>-shipping.zip and -dev.zip, root folder ELMP/.

  shipping: ELMP/dlls/main.dll + ELMP/data/ + ELMP/elmp.json + INSTALL.txt — asserted free of .lua (X-7)
  dev:      the Dev build + main.pdb (+ pilot/ once it exists)

Run through `build.bat package`.
"""

import argparse
import json
import sys
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import devconfig  # noqa: E402

REPO = devconfig.REPO
UE4SS = {"shipping": "UE4SS_v3.0.1-998-g32d8a381.zip", "dev": "zDEV-UE4SS_v3.0.1-998-g32d8a381.zip"}

INSTALL = """ELMP {version} ({flavor})

Requirements: Windows 10/11 (x64), ENDER LILIES (Steam), UE4SS v3.0.1-998-g32d8a381 ({archive}):
https://github.com/UE4SS-RE/RE-UE4SS/releases/download/experimental/{archive}

1. Extract UE4SS into ENDER LILIES\\EnderLilies\\Binaries\\Win64\\ (ue4ss\\ and dwmapi.dll appear there).
2. Copy the ELMP folder from this archive into Binaries\\Win64\\ue4ss\\Mods\\.
3. Add the line "ELMP : 1" to Binaries\\Win64\\ue4ss\\Mods\\mods.txt (before the built-in Keybinds entry).
4. Start the game. ELMP writes its status to ue4ss\\UE4SS.log ("preflight ok" or the reason it disabled itself).
"""


def build_zip(flavor: str, config: str, version: str) -> Path:
    dll = REPO / "build" / config / flavor / "dlls" / "main.dll"
    if not dll.exists():
        sys.exit(f"{dll} not found; run build.bat")
    files: list[tuple[Path, str]] = [(dll, "ELMP/dlls/main.dll"), (REPO / "elmp.json", "ELMP/elmp.json")]
    if flavor == "dev" and dll.with_suffix(".pdb").exists():
        files.append((dll.with_suffix(".pdb"), "ELMP/dlls/main.pdb"))
    for f in sorted((REPO / "data").rglob("*")):
        if f.is_file():
            files.append((f, "ELMP/data/" + f.relative_to(REPO / "data").as_posix()))
    if flavor == "dev" and (REPO / "pilot").exists():
        for f in sorted((REPO / "pilot").rglob("*")):
            if f.is_file():
                files.append((f, "ELMP/pilot/" + f.relative_to(REPO / "pilot").as_posix()))
    if flavor == "shipping":
        lua = [arc for _, arc in files if arc.lower().endswith(".lua")]
        if lua:
            sys.exit(f"shipping package must not contain .lua: {lua}")
    out = REPO / "dist" / f"ELMP-{version}-{flavor}.zip"
    out.parent.mkdir(exist_ok=True)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for src, arc in files:
            z.write(src, arc)
        z.writestr("INSTALL.txt", INSTALL.format(version=version, flavor=flavor, archive=UE4SS[flavor]))
    if flavor == "shipping":
        with zipfile.ZipFile(out) as z:
            bad = [n for n in z.namelist() if n.lower().endswith(".lua")]
        if bad:
            out.unlink()
            sys.exit(f"shipping zip contained .lua after writing: {bad}")
    print(f"packaged {out} ({len(files) + 1} entries)")
    return out


def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("--config", default="Game__Shipping__Win64")
    a = p.parse_args()
    version = json.loads((REPO / "elmp.json").read_text(encoding="utf-8"))["version"]
    build_zip("shipping", a.config, version)
    build_zip("dev", a.config, version)


if __name__ == "__main__":
    main()
