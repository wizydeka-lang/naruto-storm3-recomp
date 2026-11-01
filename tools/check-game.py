#!/usr/bin/env python3
"""Check that assets/ holds the game version this project was recompiled from.

The function boundaries in naruto_manifest.toml match one exact default.xex,
so a different region or revision would build but not run. Read-only.
Usage: python3 tools/check-game.py   (Windows: py -3 tools\\check-game.py)
"""

import hashlib
from pathlib import Path
import sys

ASSETS = Path(__file__).resolve().parent.parent / "assets"
XEX_SHA256 = "da6d872d7bd2266709810233c9782b3268cfbe1a491861ce3813962442774fcd"
REQUIRED = (
    "data/disc/adx2.cpk",
    "data/disc/dataRegion.cpk",
    "data/disc/movie.cpk",
    "data/sim/data.cpk",
    "data/sim/sound.cpk",
    "data/sim/stage.cpk",
)


def main():
    xex = ASSETS / "default.xex"
    if not xex.is_file():
        print(f"Missing {xex}. Copy your extracted game files into assets/.")
        return 1
    digest = hashlib.sha256(xex.read_bytes()).hexdigest()
    if digest != XEX_SHA256:
        print(f"default.xex SHA-256 is {digest},\nexpected {XEX_SHA256}.")
        print("This is a different game version; the manifest will not match it.")
        return 1
    missing = [name for name in REQUIRED if not (ASSETS / name).is_file()]
    for name in missing:
        print(f"Missing assets/{name}")
    if missing:
        print("Copy the complete game folder into assets/, keeping its layout.")
        return 1
    print("Game files OK.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
