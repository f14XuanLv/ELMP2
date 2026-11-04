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

INSTALL = (REPO / "INSTALL.txt").read_text(encoding="utf-8")  # template: {version} {flavor} {archive}


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
    # Developer material stays outside the Shipping runtime Mod directory.
    files.append((REPO / "native" / "sdk" / "elmp_sdk.h", "sdk/elmp_sdk.h" if flavor == "shipping" else "ELMP/sdk/elmp_sdk.h"))
    if flavor == "dev":  # third-party example mods (Lua) only in the Dev archive
        for f in sorted((REPO / "sdk" / "examples").rglob("*")):
            if f.is_file():
                files.append((f, "ELMP/sdk/examples/" + f.relative_to(REPO / "sdk" / "examples").as_posix()))
    if flavor == "shipping":
        unexpected = [arc for _, arc in files if arc.startswith("ELMP/")
                      and arc not in ("ELMP/dlls/main.dll", "ELMP/elmp.json")
                      and not arc.startswith("ELMP/data/")]
        if unexpected:
            sys.exit(f"shipping Mod directory contains unexpected files: {unexpected}")
        bad = [arc for _, arc in files if arc.lower().endswith((".pdb",)) or "/pilot/" in arc]
        if bad:
            sys.exit(f"shipping package must not contain pdb / pilot files: {bad}")
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
