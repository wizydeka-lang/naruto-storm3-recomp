#!/usr/bin/env python3
"""Interactive launcher: choose the game's default.xex and the GPU, then play.

Choices are remembered in out/launcher.json. Without arguments it opens a
window (needs PySide6) or, failing that, a terminal menu; the options below
launch directly.
Usage: python3 tools/launcher.py [--xex PATH] [--gpu ID] [--keyboard | --no-keyboard]
                                 [--build release|debug] [--resolution 720p|1080p|1440p]
                                 [--render-scale 1|2|3]
                                 [--fullscreen | --no-fullscreen] [--upscaler fsr|cas|bilinear]
                                 [--play] [--list-gpus] [--cli]
       (Windows: py -3 tools\\launcher.py ...)
"""

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys

sys.dont_write_bytecode = True  # keep tools/ free of __pycache__

ROOT = Path(__file__).resolve().parent.parent
SETTINGS = ROOT / "out" / "launcher.json"
WINDOWS = platform.system() == "Windows"
ICD_DIRS = (Path("/usr/share/vulkan/icd.d"), Path("/etc/vulkan/icd.d"))
VENDORS = {
    "0x10de": ("NVIDIA", "nvidia_icd"),
    "0x1002": ("AMD", "radeon_icd"),
    "0x8086": ("Intel", "intel_icd"),
}
# Window size per choice. The game always draws its own 1280x720 frame; the
# render scale below sets how many host pixels each of its pixels gets, and the
# upscaler stretches the result to the window or screen.
RESOLUTIONS = {
    "720p": ("720p (original)", None),
    "1080p": ("1080p", (1920, 1080)),
    "1440p": ("1440p", (2560, 1440)),
}
# Internal render scale (draw_resolution_scale_x/y). The SDK's render target
# and texture caches map each guest pixel to a whole block of host pixels, so
# only whole numbers work; the GPU work grows with the square of the scale.
RENDER_SCALES = {
    1: "1x (1280x720)",
    2: "2x (2560x1440, about 4x the GPU work)",
    3: "3x (3840x2160, about 9x the GPU work)",
}
# Output upscalers (ReXGlue present_effect). They sharpen the 720p image as it
# is scaled to the screen and cost almost nothing; they need an SDK set up
# with the FidelityFX headers (tools/setup-sdk.*).
UPSCALERS = {
    "fsr": "AMD FSR 1 (sharpest)",
    "cas": "AMD CAS (light sharpening)",
    "bilinear": "Off (plain scaling)",
}
# Windows' per-app GPU preference (Settings > System > Display > Graphics),
# which orders the Direct3D 12 adapters the game picks from.
WINDOWS_GPUS = {
    "auto": ("Let Windows decide", 0),
    "power": ("Power saving (integrated GPU)", 1),
    "performance": ("High performance (dedicated GPU)", 2),
}


def load_check_game():
    spec = importlib.util.spec_from_file_location("check_game", ROOT / "tools" / "check-game.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def xex_status(xex):
    """Return None if xex is the expected game version, else a problem."""
    path = Path(xex)
    if not path.is_file():
        return "file not found"
    if hashlib.sha256(path.read_bytes()).hexdigest() != load_check_game().XEX_SHA256:
        return "different game version (SHA-256 does not match)"
    return None


def linux_gpus():
    """GPUs from /sys/class/drm with the Vulkan driver that serves each."""
    gpus = []
    for card in sorted(Path("/sys/class/drm").glob("card[0-9]*")):
        if "-" in card.name or not (card / "device" / "vendor").exists():
            continue
        vendor = (card / "device" / "vendor").read_text().strip()
        device = (card / "device" / "device").read_text().strip()
        slot = (card / "device").resolve().name
        brand, icd_prefix = VENDORS.get(vendor, ("Unknown", None))
        name = brand
        if shutil.which("lspci"):
            line = subprocess.run(["lspci", "-s", slot], capture_output=True, text=True).stdout
            if ": " in line:
                name = line.split(": ", 1)[1].split(" (rev")[0].strip()
                # "NVIDIA Corporation GA107 [GeForce RTX 2050]" -> "NVIDIA GeForce RTX 2050"
                if name.endswith("]") and "[" in name:
                    name = f"{brand} {name[name.rindex('[') + 1:-1]}"
        icd = None
        if icd_prefix:
            for directory in ICD_DIRS:
                matches = sorted(directory.glob(icd_prefix + "*.json"), key=lambda p: "x86_64" not in p.name)
                if matches:
                    icd = str(matches[0])
                    break
        gpus.append({"id": f"{vendor[2:]}:{device[2:]}", "name": name, "brand": brand, "icd": icd})
    return gpus


def gpu_choices():
    """(id, label) pairs for this platform; 'auto' always comes first."""
    if WINDOWS:
        names = subprocess.run(
            ["powershell", "-NoProfile", "-Command",
             "(Get-CimInstance Win32_VideoController).Name"],
            capture_output=True, text=True).stdout.split("\n")
        found = ", ".join(n.strip() for n in names if n.strip())
        choices = [(key, label) for key, (label, _) in WINDOWS_GPUS.items()]
        return choices, found
    choices = [("auto", "Automatic (driver's choice)")]
    for gpu in linux_gpus():
        if gpu["icd"]:
            choices.append((gpu["id"], f"{gpu['name']} ({gpu['id']})"))
    return choices, ""


def find_build(kind):
    system = "win-amd64" if WINDOWS else "linux-amd64"
    suffix = "d" if kind == "debug" else ""
    directory = ROOT / "out" / "build" / f"{system}-{kind}"
    exe = directory / ("naruto.exe" if WINDOWS else "naruto")
    plugin = directory / (f"rexgpu-xenos{suffix}.dll" if WINDOWS else f"librexgpu-xenos{suffix}.so")
    return exe, plugin


def upscalers_supported(exe):
    """Whether the game's ReXGlue runtime was built with FSR/CAS."""
    if WINDOWS:
        candidates = exe.parent.glob("rexruntime*.dll")
    else:
        candidates = Path(sdk_lib_dir(exe)).glob("librexruntime*.so")
    for library in candidates:
        if b"bilinear, cas, fsr" in library.read_bytes():
            return True
    return False


def sdk_lib_dir(exe):
    """Linux: the SDK runtime library directory the build was configured with."""
    cache = exe.parent / "CMakeCache.txt"
    if cache.is_file():
        for line in cache.read_text(errors="replace").splitlines():
            if line.startswith("rexglue_DIR:PATH="):
                return str(Path(line.split("=", 1)[1]).parents[1])
    prefix = os.environ.get("NARUTO_SDK_PREFIX", str(Path.home() / "rexglue-sdk/out/install/linux-amd64"))
    return str(Path(prefix) / "lib64")


def pick_file(start):
    """Native file picker where one exists, otherwise typed input."""
    try:
        if WINDOWS:
            import tkinter
            from tkinter import filedialog
            tkinter.Tk().withdraw()
            return filedialog.askopenfilename(
                initialdir=start, title="Select default.xex",
                filetypes=[("Xbox 360 executable", "default.xex"), ("All files", "*")]) or None
        if shutil.which("zenity"):
            result = subprocess.run(
                ["zenity", "--file-selection", "--title=Select default.xex",
                 f"--filename={start}/", "--file-filter=default.xex | default.xex"],
                capture_output=True, text=True)
            return result.stdout.strip() or None
        if shutil.which("kdialog"):
            result = subprocess.run(["kdialog", "--getopenfilename", start, "default.xex"],
                                    capture_output=True, text=True)
            return result.stdout.strip() or None
    except Exception:
        pass
    typed = input("Path to default.xex: ").strip().strip('"')
    return typed or None


def set_windows_gpu_preference(exe, choice):
    import winreg
    key = winreg.CreateKey(winreg.HKEY_CURRENT_USER, r"Software\Microsoft\DirectX\UserGpuPreferences")
    with key:
        winreg.SetValueEx(key, str(exe), 0, winreg.REG_SZ, f"GpuPreference={WINDOWS_GPUS[choice][1]};")


SHM = Path("/dev/shm")


def guest_memory_files():
    """Linux: the game's guest-memory files in /dev/shm."""
    return set() if WINDOWS or not SHM.is_dir() else set(SHM.glob("xenia_memory_*"))


def remove_guest_memory_files(before):
    """Delete guest-memory files left by a game that has exited.

    The game does not unlink its multi-gigabyte /dev/shm file when it quits,
    and leftovers fill shared memory until later runs crash.
    """
    for path in guest_memory_files() - before:
        try:
            path.unlink()
        except OSError:
            pass


class LaunchError(Exception):
    pass


def prepare_launch(settings):
    """Return (command, env) for the chosen settings, or raise LaunchError.

    On Windows this also stores the GPU preference for the executable.
    """
    exe, plugin = find_build(settings["build"])
    if not exe.is_file() or not plugin.is_file():
        raise LaunchError(f"No {settings['build']} build found at {exe.parent}. "
                          "Build the game first (see README.md).")
    problem = xex_status(settings["xex"])
    if problem:
        raise LaunchError(f"{settings['xex']}: {problem}.")

    game_root = str(Path(settings["xex"]).resolve().parent)
    command = [str(exe), f"--game_data_root={game_root}", "--gpu_plugin=xenos"]
    if settings["keyboard"]:
        command.append("--mnk_mode=true")
    _, window = RESOLUTIONS[settings["resolution"]]
    scale = settings["render_scale"]
    command += [f"--draw_resolution_scale_x={scale}", f"--draw_resolution_scale_y={scale}",
                f"--fullscreen={'true' if settings['fullscreen'] else 'false'}"]
    if window:
        command += [f"--window_width={window[0]}", f"--window_height={window[1]}"]
    if settings["upscaler"] != "bilinear" and upscalers_supported(exe):
        command.append(f"--present_effect={settings['upscaler']}")
    env = dict(os.environ)
    gpu = settings["gpu"]
    if WINDOWS:
        set_windows_gpu_preference(exe, gpu)
    else:
        env["LD_LIBRARY_PATH"] = os.pathsep.join(
            filter(None, [sdk_lib_dir(exe), env.get("LD_LIBRARY_PATH")]))
        env.setdefault("SDL_AUDIO_DRIVER", "pipewire")
        if gpu != "auto":
            match = next((g for g in linux_gpus() if g["id"] == gpu and g["icd"]), None)
            if match is None:
                raise LaunchError(f"GPU {gpu} is not available; choose another.")
            # Load only that vendor's Vulkan driver, and point Mesa's device
            # selection at the exact card in case two share a driver.
            env["VK_DRIVER_FILES"] = env["VK_ICD_FILENAMES"] = match["icd"]
            env["MESA_VK_DEVICE_SELECT"] = gpu
            if match["brand"] == "NVIDIA":
                env["__NV_PRIME_RENDER_OFFLOAD"] = "1"
            command.append("--vulkan_device=0")
    return command, env


def launch(settings):
    try:
        command, env = prepare_launch(settings)
    except LaunchError as error:
        print(f"Cannot launch: {error}")
        return 1
    print("Starting Naruto...")
    before = guest_memory_files()
    code = subprocess.run(command, cwd=ROOT, env=env).returncode
    remove_guest_memory_files(before)
    return code


def load_settings():
    settings = {"xex": str(ROOT / "assets" / "default.xex"), "gpu": "auto",
                "keyboard": False, "build": "release", "resolution": "720p",
                "render_scale": 1, "fullscreen": True, "upscaler": "fsr"}
    try:
        settings.update(json.loads(SETTINGS.read_text()))
    except (OSError, ValueError):
        pass
    if WINDOWS and settings["gpu"] not in WINDOWS_GPUS:
        settings["gpu"] = "auto"
    if settings["resolution"] not in RESOLUTIONS:
        settings["resolution"] = "720p"
    if settings["render_scale"] not in RENDER_SCALES:
        settings["render_scale"] = 1
    if settings["upscaler"] not in UPSCALERS:
        settings["upscaler"] = "fsr"
    return settings


def save_settings(settings):
    SETTINGS.parent.mkdir(parents=True, exist_ok=True)
    SETTINGS.write_text(json.dumps(settings, indent=2) + "\n")


def menu(settings):
    while True:
        choices, found = gpu_choices()
        labels = dict(choices)
        problem = xex_status(settings["xex"])
        print("\n=== Naruto launcher ===")
        print(f" 1) Game:     {settings['xex']}  [{problem or 'OK'}]")
        print(f" 2) GPU:      {labels.get(settings['gpu'], settings['gpu'] + ' (not found)')}")
        if found:
            print(f"              GPUs in this PC: {found}")
        print(f" 3) Keyboard: {'on (W A S D, Space, Backspace, Enter...)' if settings['keyboard'] else 'off (use a controller)'}")
        print(f" 4) Build:    {settings['build']}")
        print(f" 5) Resolution: {RESOLUTIONS[settings['resolution']][0]}")
        print(f" 6) Fullscreen: {'on' if settings['fullscreen'] else 'off'}")
        print(f" 7) Upscaler:   {UPSCALERS[settings['upscaler']]}")
        print(f" 8) Render scale: {RENDER_SCALES[settings['render_scale']]}")
        print(" P) Play   Q) Quit")
        answer = input("> ").strip().lower()
        if answer == "1":
            chosen = pick_file(str(Path(settings["xex"]).parent))
            if chosen:
                settings["xex"] = chosen
        elif answer == "2":
            for number, (_, label) in enumerate(choices, 1):
                print(f"   {number}) {label}")
            pick = input("   GPU number: ").strip()
            if pick.isdigit() and 1 <= int(pick) <= len(choices):
                settings["gpu"] = choices[int(pick) - 1][0]
        elif answer == "3":
            settings["keyboard"] = not settings["keyboard"]
        elif answer == "4":
            settings["build"] = "debug" if settings["build"] == "release" else "release"
        elif answer == "5":
            keys = list(RESOLUTIONS)
            settings["resolution"] = keys[(keys.index(settings["resolution"]) + 1) % len(keys)]
        elif answer == "6":
            settings["fullscreen"] = not settings["fullscreen"]
        elif answer == "7":
            keys = list(UPSCALERS)
            settings["upscaler"] = keys[(keys.index(settings["upscaler"]) + 1) % len(keys)]
        elif answer == "8":
            keys = list(RENDER_SCALES)
            settings["render_scale"] = keys[(keys.index(settings["render_scale"]) + 1) % len(keys)]
        elif answer == "p":
            save_settings(settings)
            return launch(settings)
        elif answer == "q":
            save_settings(settings)
            return 0
        save_settings(settings)


def main():
    parser = argparse.ArgumentParser(description="Choose the game files and GPU, then launch Naruto.")
    parser.add_argument("--xex", help="path to default.xex (its folder is used as the game data)")
    parser.add_argument("--gpu", help="GPU id from --list-gpus, or 'auto'")
    parser.add_argument("--keyboard", action=argparse.BooleanOptionalAction, help="keyboard controls")
    parser.add_argument("--build", choices=("release", "debug"))
    parser.add_argument("--resolution", choices=tuple(RESOLUTIONS))
    parser.add_argument("--render-scale", type=int, choices=tuple(RENDER_SCALES))
    parser.add_argument("--fullscreen", action=argparse.BooleanOptionalAction)
    parser.add_argument("--upscaler", choices=tuple(UPSCALERS))
    parser.add_argument("--play", action="store_true", help="launch now without the menu")
    parser.add_argument("--list-gpus", action="store_true", help="list GPU ids and exit")
    parser.add_argument("--cli", action="store_true", help="use the terminal menu instead of the window")
    args = parser.parse_args()

    if args.list_gpus:
        choices, found = gpu_choices()
        for gpu_id, label in choices:
            print(f"{gpu_id:12} {label}")
        if found:
            print(f"GPUs in this PC: {found}")
        return 0
    settings = load_settings()
    if args.xex:
        settings["xex"] = str(Path(args.xex).resolve())
    if args.gpu:
        if args.gpu not in dict(gpu_choices()[0]):
            print(f"Unknown GPU '{args.gpu}'. Run with --list-gpus.")
            return 1
        settings["gpu"] = args.gpu
    if args.keyboard is not None:
        settings["keyboard"] = args.keyboard
    if args.build:
        settings["build"] = args.build
    if args.resolution:
        settings["resolution"] = args.resolution
    if args.render_scale:
        settings["render_scale"] = args.render_scale
    if args.fullscreen is not None:
        settings["fullscreen"] = args.fullscreen
    if args.upscaler:
        settings["upscaler"] = args.upscaler
    save_settings(settings)
    if args.play:
        return launch(settings)
    if not args.cli:
        try:
            import launcher_gui
        except ImportError:
            print("PySide6 is not installed (pip install PySide6); using the terminal menu.")
        else:
            return launcher_gui.run(settings)
    return menu(settings)


if __name__ == "__main__":
    sys.exit(main())
