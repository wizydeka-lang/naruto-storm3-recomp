# Naruto Recomp

An experimental native PC port of the Xbox 360 game **Naruto Shippuden: Ultimate Ninja Storm 3 Full Burst**, built with [ReXGlue](https://github.com/rexglue/rexglue-sdk). ReXGlue translates the game's PowerPC code into C++, which is then compiled for your PC.

**This repository contains no game files and no translated game code.** You need your own copy of the game. The build translates your `default.xex` and compiles it on your machine.

## Status

Experimental. On Linux the game reaches the menus and plays Free Battle fights. Story mode and other modes have not been checked. The Windows build follows the same steps but has not been tested yet. Please report problems with your log file attached.

## What you need

- Your own Xbox 360 copy of the game, extracted to a folder (see step 1).
- A 64-bit PC with an x86-64-v2 CPU and 16 GB of RAM. About 15 GB of free disk space for the SDK and build, plus about 10 GB for the game files.
- A GPU with Vulkan (Linux) or Direct3D 12 (Windows). On laptops with two GPUs, use the dedicated one. An integrated Radeon 660M reaches about 25 FPS; an RTX 2050 holds the game's 30 FPS.

## 1. Add your game files

1. Extract your game disc image with an Xbox 360 ISO tool (for example `extract-xiso`).
2. Copy the whole extracted folder into this repository's `assets/` folder, keeping its layout:

   ```text
   assets/
     default.xex
     data/
       disc/   adx2.cpk  dataRegion.cpk  movie.cpk
       sim/    data.cpk  sound.cpk  stage.cpk
     ...
   ```

3. Check that you have the version this project was made from:

   ```bash
   python3 tools/check-game.py      # Windows: py -3 tools\check-game.py
   ```

   It must print `Game files OK.` A different region or revision will not work, because the project's function list matches one exact `default.xex`.

## 2. Linux

### Install the tools

Install Git, CMake 3.25 or newer, Ninja, Clang 20 (`clang-20` and `clang++-20`), Python 3.11 or newer, the GTK3, PipeWire and PulseAudio development headers, and a Vulkan driver. On Debian or Ubuntu:

```bash
sudo apt install git cmake ninja-build clang-20 python3 libgtk-3-dev \
  libpipewire-0.3-dev libpulse-dev mesa-vulkan-drivers
```

Install the audio headers before setting up the SDK; without them the game has no sound.

### Set up the ReXGlue SDK

This clones ReXGlue at the exact revision this project uses, applies the fixes in `patches/`, downloads AMD's FidelityFX headers for the FSR and CAS upscalers, then builds and installs it into `~/rexglue-sdk`. It takes a while the first time.

```bash
bash tools/setup-sdk.sh
```

### Build

```bash
cmake --preset linux-amd64-release -DCMAKE_PREFIX_PATH=$HOME/rexglue-sdk/out/install/linux-amd64
cmake --build --preset linux-amd64-release --target naruto_codegen
cmake --preset linux-amd64-release
cmake --build --preset linux-amd64-release
```

The second command translates `assets/default.xex` into `generated/default/`. The third configures again so the build picks up those new files; skipping it ends in a link error about `PPCImageConfig`. The last command compiles everything, which takes a while and needs a lot of memory. Add `--parallel 4` to it if the compiler runs out of memory. Later rebuilds only need the last command.

### Run

```bash
bash tools/run-radeon.sh
```

It runs the Release build with `assets/` as the game data. It defaults to the NVIDIA driver; `--radeon` picks the AMD driver, `--debug` the Debug build, and `--profile` an isolated run with GPU logs. To pass other [game options](#game-options), start the game directly:

```bash
LD_LIBRARY_PATH=$HOME/rexglue-sdk/out/install/linux-amd64/lib64 \
  ./out/build/linux-amd64-release/naruto --game_data_root="$PWD/assets" --gpu_plugin=xenos
```

The game leaves a large shared-memory file in `/dev/shm` when it exits. Delete leftover `/dev/shm/xenia_memory_*` files while no game is running; otherwise they fill memory and later runs can crash.

## 3. Windows

These steps have not been tested on Windows yet.

### Install the tools

| Tool | Notes |
| --- | --- |
| [Git for Windows](https://git-scm.com/download/win) | |
| [Visual Studio 2022](https://visualstudio.microsoft.com/vs/) | Community edition is fine. Select **Desktop development with C++**, plus **C++ Clang Compiler for Windows** and **MSBuild support for LLVM (clang-cl) toolset**. |
| [LLVM](https://github.com/llvm/llvm-project/releases) | Clang 20 or newer, if Visual Studio's bundled Clang is older. `clang` and `clang++` must be on `PATH`. |
| [CMake](https://cmake.org/download/) | 3.25 or newer, on `PATH` |
| [Ninja](https://github.com/ninja-build/ninja/releases) | On `PATH` (Visual Studio's CMake tools also provide it) |
| [Python](https://www.python.org/downloads/windows/) | 3.10 or newer, for `tools\check-game.py` |

Use a short folder path such as `C:\dev\naruto-recomp`; long paths can break the SDK build.

Open **x64 Native Tools Command Prompt for VS 2022** from the Start menu, then run `powershell`. Run every command below in that window, from the repository folder.

### Set up the ReXGlue SDK

This clones ReXGlue into `C:\dev\rexglue-sdk`, applies the fixes in `patches\`, downloads AMD's FidelityFX headers for the FSR and CAS upscalers, then builds and installs it with the Direct3D 12 backend:

```powershell
powershell -ExecutionPolicy Bypass -File tools\setup-sdk.ps1
```

### Build

```powershell
$sdkPrefix = "C:/dev/rexglue-sdk/out/install/win-amd64"
cmake --preset win-amd64-release "-DCMAKE_PREFIX_PATH=$sdkPrefix"
cmake --build --preset win-amd64-release --target naruto_codegen
cmake --preset win-amd64-release
cmake --build --preset win-amd64-release --parallel 4
```

As on Linux, the second command translates the game, the third configures again to pick up the generated files, and the last compiles. Later rebuilds only need the last command.

The build copies the SDK's DLLs and the `rexgpu-xenos.dll` GPU plugin next to `out\build\win-amd64-release\naruto.exe`. If the game later says the GPU plugin is missing, copy them yourself:

```powershell
Copy-Item "$sdkPrefix/bin/*.dll" .\out\build\win-amd64-release\ -Force
```

### Run

```powershell
$gameRoot = (Resolve-Path .\assets).Path
& .\out\build\win-amd64-release\naruto.exe "--game_data_root=$gameRoot" --gpu_plugin=xenos
```

## Game options

Add these to the `naruto` command line. `--game_data_root` can point at any folder that holds the game files, for example on another drive.

| Option | Effect |
| --- | --- |
| `--window_width=1920 --window_height=1080` | Window size. The game's image is scaled to it by the upscaler, which costs almost nothing. |
| `--fullscreen=true` | Fill the screen; the window size then doesn't matter. |
| `--draw_resolution_scale_x=2 --draw_resolution_scale_y=2` | Render scale (see below). |
| `--present_effect=fsr` | Upscaler (see below). |
| `--mnk_mode=true` | Keyboard controls (see [Playing](#playing)). |

- **Render scale:** how many pixels the game is drawn at.

  | Scale | Rendered at | GPU work |
  | --- | --- | --- |
  | 1 (default) | 1280×720 | original |
  | 2 | 2560×1440 | about 4× |
  | 3 | 3840×2160 | about 9× |

  ReXGlue only scales by whole numbers: its render target and texture caches give each of the game's pixels a whole block of screen pixels, so steps such as 1.5x are not possible without rewriting them. 2x is sharper, but an RTX 2050 drops from 30 to about 18 FPS on the menus.
- **Upscaler:** sharpens the picture as it is scaled up to your screen.

  | `--present_effect` | Effect |
  | --- | --- |
  | `fsr` | AMD FSR 1: sharpest; crisp edges and texture detail |
  | `cas` | AMD CAS: light sharpening |
  | (not set) | Plain scaling, softest |

  Both upscalers ran at the full 30 FPS on an RTX 2050, so **render scale 1 with FSR is the recommended setting**, at any window size: sharper than plain scaling without the slowdown of 2x. They need an SDK set up with `tools/setup-sdk.sh` or `.ps1`, which downloads AMD's FidelityFX headers (about 5 MB).

The build itself always uses `assets/default.xex`; `--game_data_root` only sets where the game reads its data while running.

## Playing

- **Controller:** an Xbox-style controller works directly.
- **Keyboard:** add `--mnk_mode=true` when starting `naruto`.

  | Keys | Controller |
  | --- | --- |
  | W A S D | Left stick |
  | Arrow keys | Right stick |
  | Shift + arrow keys | D-pad |
  | Space / `;` | A |
  | Backspace / `'` | B |
  | L | X |
  | P | Y |
  | Q / E | Left / right trigger |
  | 1 / 3 | Left / right shoulder |
  | Enter / X | Start |
  | Tab / Z | Back |

- **In-game tools:** F3 shows the debug overlay, F4 the settings, and the backtick key the log console.

Settings are stored in `naruto.toml` next to the executable. The first build creates it from `config/naruto-performance.toml`, which includes rendering fixes this game needs. Logs are written to the `logs` folder next to the executable. Saves and the shader cache go to your user folder under `naruto`.

## Troubleshooting

- **`assets/default.xex not found` during configure:** do step 1 first.
- **`check-game.py` reports a different SHA-256:** your copy is a different version. It won't work with this function list.
- **The first launch stutters:** shaders are compiled and cached the first time each effect appears. Later runs are smoother.
- **Low FPS:** run on the dedicated GPU, and use render scale 1 with the FSR upscaler.
- **Updating:** after pulling new changes, run the setup script again if `patches/` changed. If `naruto_manifest.toml` changed, repeat all four build commands; otherwise only the last one.

## How the project is organised

| Path | Contents |
| --- | --- |
| `naruto_manifest.toml` | Codegen settings and the function boundaries that had to be found by hand |
| `src/` | Application startup code |
| `patches/` | Fixes applied to the ReXGlue SDK (see `patches/README.md`) |
| `config/` | Default runtime settings |
| `tools/` | SDK setup, game check, run script and build checks |
| `docs/` | Development notes |

Generated code (`generated/default/`) and build output (`out/`) are created on your machine and are not part of the repository.

## Legal

This project includes no game code or assets. You must own the game and supply your own files. Do not share `assets/`, `generated/default/` or built executables, since they contain or are derived from the original game.
