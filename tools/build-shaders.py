#!/usr/bin/env python3
"""Build the native renderer's shader cache from the user's game files.

The game's shaders are Xbox 360 shader containers in
data/system/nuccMaterial_xbox360.nsh and nuccPostEffect_xbox360.nsh. This
converts each one to HLSL with XenosRecomp (pinned below, with
tools/shaders/xenosrecomp-naruto.patch) and compiles it to SPIR-V with DXC, then
writes them all to one cache file. The cache holds code derived from the game,
so it is built locally and never committed.

Cache format (little-endian): b"NSHC", version, entry count; then per entry
the library (0 nuccMaterial, 1 nuccPostEffect), stage (0 vertex, 1 pixel), two
padding bytes, the container's offset in its library file, and the SPIR-V
blob's offset in the cache and size; then the SPIR-V blobs.

Usage: python3 tools/build-shaders.py [--game assets] [--out out/shaders/naruto_shaders.bin]
Needs git, cmake, ninja and clang++ (or clang++-20) on PATH.
"""

import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
XENOSRECOMP_URL = "https://github.com/hedge-dev/XenosRecomp.git"
XENOSRECOMP_REVISION = "990d03b28a27b50277ee5d8d942e1c5f873869d1"
PATCH = ROOT / "tools" / "shaders" / "xenosrecomp-naruto.patch"
LIBRARIES = ["nuccMaterial", "nuccPostEffect"]
CACHE_VERSION = 2


def run(*args, **kwargs):
    subprocess.run(args, check=True, **kwargs)


def build_xenosrecomp(tools_dir):
    """Clones, patches and builds XenosRecomp once; returns its directory."""
    src = tools_dir / "XenosRecomp"
    if not src.exists():
        run("git", "clone", "--quiet", XENOSRECOMP_URL, str(src))
        run("git", "-C", str(src), "checkout", "--quiet", XENOSRECOMP_REVISION)
        run("git", "-C", str(src), "submodule", "update", "--quiet", "--init", "--recursive")
        run("git", "-C", str(src), "apply", str(PATCH))
    elif subprocess.run(["git", "-C", str(src), "apply", "--reverse", "--check", str(PATCH)],
                        capture_output=True).returncode != 0:
        sys.exit(f"{src} exists without {PATCH.name} applied; delete it to rebuild.")
    build = tools_dir / "XenosRecomp-build"
    if not (build / "build.ninja").exists():
        suffix = "-20" if shutil.which("clang++-20") else ""
        run("cmake", "-S", str(src), "-B", str(build), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
            f"-DCMAKE_CXX_COMPILER=clang++{suffix}", f"-DCMAKE_C_COMPILER=clang{suffix}")
    run("cmake", "--build", str(build), "--target", "XenosRecomp")
    return src, build / "XenosRecomp" / "XenosRecomp"


def containers(data):
    """Yields (offset, stage, bytes) for each shader container, like XenosRecomp."""
    i = 0
    while i + 36 <= len(data):
        flags, vsize, psize = struct.unpack_from(">III", data, i)
        f1c, f20 = struct.unpack_from(">II", data, i + 28)
        total = vsize + psize
        if (flags & 0xFFFFFF00) == 0x102A1100 and not f1c and not f20 and 0 < total <= len(data) - i:
            yield i, 0 if flags & 1 else 1, data[i:i + total]
            i += total
        else:
            i += 4


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--game", type=Path, default=ROOT / "assets",
                        help="folder holding default.xex and data/ (default: assets)")
    parser.add_argument("--out", type=Path, default=ROOT / "out" / "shaders" / "naruto_shaders.bin")
    args = parser.parse_args()

    src, recomp = build_xenosrecomp(ROOT / "out" / "tools")
    dxc_dir = src / "thirdparty" / "dxc-bin"
    dxc = dxc_dir / "bin" / "x64" / "dxc-linux"
    env = dict(os.environ, LD_LIBRARY_PATH=str(dxc_dir / "lib" / "x64"))
    include = src / "XenosRecomp" / "shader_common.h"

    entries, blobs, failed = [], [], []
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        for library_index, library in enumerate(LIBRARIES):
            path = args.game / "data" / "system" / f"{library}_xbox360.nsh"
            if not path.exists():
                sys.exit(f"{path} not found; point --game at the game files.")
            for offset, stage, blob in containers(path.read_bytes()):
                name = f"{library}_{offset:08x}"
                container, hlsl, spirv = tmp / f"{name}.bin", tmp / f"{name}.hlsl", tmp / f"{name}.spv"
                container.write_bytes(blob)
                subprocess.run([str(recomp), str(container), str(hlsl), str(include)],
                               check=True, capture_output=True)
                command = [str(dxc), "-T", "vs_6_0" if stage == 0 else "ps_6_0", "-HV", "2021",
                           "-all-resources-bound", "-spirv", "-fvk-use-dx-layout", "-Qstrip_debug",
                           str(hlsl), "-Fo", str(spirv)]
                if stage == 0:
                    command.insert(-3, "-fvk-invert-y")
                result = subprocess.run(command, env=env, capture_output=True, text=True)
                if result.returncode != 0:
                    errors = [l for l in result.stderr.splitlines() if "error" in l]
                    failed.append((name, errors[:1]))
                    continue
                entries.append((library_index, stage, offset))
                blobs.append(spirv.read_bytes())

    header = struct.pack("<4sII", b"NSHC", CACHE_VERSION, len(entries))
    position = len(header) + 16 * len(entries)
    table = b""
    for (library_index, stage, offset), blob in zip(entries, blobs):
        table += struct.pack("<BBxxIII", library_index, stage, offset, position, len(blob))
        position += len(blob)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(header + table + b"".join(blobs))
    print(f"{args.out}: {len(entries)} shaders, {position // 1024} KiB")
    for name, errors in failed:
        print(f"  failed: {name} {re.sub(r'^.*?error', 'error', errors[0]) if errors else ''}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
