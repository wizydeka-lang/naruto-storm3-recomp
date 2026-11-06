# Naruto Recomp

An experimental native PC and Android port of the Xbox 360 game **Naruto Shippuden: Ultimate Ninja Storm 3 Full Burst**, built with [ReXGlue](https://github.com/rexglue/rexglue-sdk). ReXGlue translates the game's PowerPC code into C++, which is then compiled for your machine.

**This repository contains no game files and no translated game code.** You need your own copy of the game. The build translates your `default.xex` and compiles it on your machine.

## Status

| Platform | State |
| --- | --- |
| Linux | Reaches the menus and plays Free Battle fights. Main development platform. |
| Android | Runs on arm64 phones with a capable Vulkan GPU. |
| Windows | Installer and build scripts are written but **not yet tested** on a Windows PC. |
| Online | **Experimental** peer-to-peer play, see [Online mode](#online-mode-experimental). |

Story mode and other modes have not been checked. Please [report problems](#reporting-problems) with your log file attached.

## What you need

- Your own Xbox 360 copy of the game, extracted to a folder.
- A 64-bit PC with an x86-64-v2 CPU and 16 GB of RAM. About 15 GB of free disk space for the SDK and build, plus about 10 GB for the game files.
- A GPU with Vulkan. On laptops with two GPUs, use the dedicated one. An integrated Radeon 660M reaches about 25 FPS; an RTX 2050 holds the game's 30 FPS.

## 1. Prepare your game files

1. Extract your game disc image with an Xbox 360 ISO tool (for example `extract-xiso`). You get a folder like this:

   ```text
   default.xex
   data/
     disc/   adx2.cpk  dataRegion.cpk  movie.cpk
     sim/    data.cpk  sound.cpk  stage.cpk
   ...
   ```

2. On **Windows**, the installer asks for this folder (next section).
   On **Linux**, copy or link the folder as `assets/` in this repository (`ln -s /path/to/game assets`), then check it:

   ```bash
   python3 tools/check-game.py
   ```

   It must print `Game files OK.` A different region or revision will not work, because the project's function list matches one exact `default.xex`.

## 2. Windows (installer)

Not yet tested on real Windows machines; reports are welcome.

1. Put this repository in a short path, for example `C:\dev\naruto-recomp` (download it with Git, or as a ZIP from GitHub and extract it). Long paths can break the build.
2. Double-click **`install-windows.bat`**.

The installer builds everything on your PC from source. The first run takes 30 to 90 minutes and needs about 25 GB of free disk space. It:

1. Installs missing build tools with `winget`: Git, CMake, Ninja, LLVM (Clang 20 or newer), Python 3 and the Visual Studio 2022 C++ Build Tools. Windows may ask for permission.
2. Asks for your extracted game folder, links it as `assets\` (no copy), and checks the game version.
3. Clones ReXGlue into `C:\dev\rexglue-sdk`, applies the fixes in `patches\`, and builds it with the Vulkan backend.
4. Translates `default.xex` and builds `out\build\win-amd64-release\naruto.exe`.
5. Adds **Naruto Storm 3** shortcuts to the desktop and the Start menu, plus **Naruto Storm 3 - Online (experimental)** in the Start menu.

Run `install-windows.bat` again after pulling updates; finished steps are skipped and the game is rebuilt. Options: `install-windows.bat -GameDir D:\games\storm3 -SdkDir D:\rexglue-sdk`.

To start the game without the shortcuts, pass [game options](#game-options) after `--`:

```powershell
powershell -ExecutionPolicy Bypass -File tools\play-windows.ps1 -- --fullscreen=true
```

`-Classic` uses Xenia's own renderer instead of the native one, which is slower but useful if the picture is wrong.

<details>
<summary>Building by hand on Windows</summary>

Install the tools above yourself. Open **x64 Native Tools Command Prompt for VS 2022**, run `powershell`, and from the repository folder:

```powershell
powershell -ExecutionPolicy Bypass -File tools\setup-sdk.ps1
$sdkPrefix = "C:/dev/rexglue-sdk/out/install/win-amd64"
cmake --preset win-amd64-release "-DCMAKE_PREFIX_PATH=$sdkPrefix"
cmake --build --preset win-amd64-release --target naruto_codegen
cmake --preset win-amd64-release
cmake --build --preset win-amd64-release --parallel 4
Copy-Item "$sdkPrefix/bin/*.dll" .\out\build\win-amd64-release\ -Force
```

</details>

## 3. Linux

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

### Launcher

```bash
python3 tools/launcher/naruto_launcher.py --install
```

This adds two entries to the app menu (on Bazzite, KDE's launcher): **Naruto Shippuden: Ultimate Ninja Storm 3** starts the game, and **Storm 3 Launcher** opens the settings. Their icons and the launcher's background come from your copy of the game (`assets/nxeart`). The launcher, which needs PySide6 (part of Bazzite and Fedora), sets:

- **Graphics:** resolution (720p, 1080p or 1440p), windowed or fullscreen, screen ratio (16:9, 16:10, 21:9, 32:9, 4:3 or fill the screen; the game is made for 16:9), upscaling to the screen (FSR 1, CAS or bilinear) with sharpness, FXAA anti-aliasing, and the GPU.
- **Controller:** the button layout for each type of controller (Xbox, PlayStation, Nintendo, and Steam Input or other). Press a button to see its row light up.
- **Keyboard:** a key or two for every Xbox control.
- **System:** **Add to Steam** puts the game in your Steam library, and in Game Mode.

It starts the game with the native renderer (`run-radeon.sh --native-only`). `--play` starts the game with the saved settings without showing the launcher. Settings are kept in `~/.config/naruto-storm3/launcher.json`.

The game leaves a large shared-memory file in `/dev/shm` when it exits. Delete leftover `/dev/shm/xenia_memory_*` files while no game is running; otherwise they fill memory and later runs can crash.

## 4. Android (experimental)

An arm64 phone with Android 8 or newer and a Vulkan 1.3 GPU that supports 64-bit integers in shaders. Not every phone GPU has these; the game stops at start-up on those that don't.

### Build (on Linux)

Install the Android NDK r30, JDK 17 and the Android SDK (platform 35), set up the desktop SDK first (step 3), then build the SDK for Android as described in [`patches/README.md`](patches/README.md#android-build-support). Then:

```bash
export ANDROID_NDK_HOME=~/Android/android-ndk-r30
cmake --preset android-arm64-release
cmake --build out/build/android-arm64-release
cd android
JAVA_HOME=~/Android/jdk-17 ANDROID_HOME=~/Android/sdk ./gradlew assembleRelease
```

The APK is `android/app/build/outputs/apk/release/app-release.apk`. If you built the shader cache on the PC (`out/shaders/naruto_shaders.bin`), it goes into the APK, so the phone doesn't compile shaders while you play. The app icon is made from your game's box art.

### Play

1. Copy the game folder (with `default.xex` and `data`) anywhere on the phone or its SD card.
2. Open the app and allow **All files access** when asked. The launcher looks for the game itself; if it doesn't find it, tap **Choose folder**.
3. Pick **720p** or **1080p**, and FXAA if you like, then tap **Play**.

The picture always fills the screen. The game runs at its own 30 FPS: the screen is switched to the lowest refresh rate that 30 divides, and the phone's sustained performance mode keeps the clocks steady instead of boosting and throttling. 720p uses the least power.

The launcher shows an Xbox 360-style on-screen controller, with its size and opacity. A Bluetooth or USB controller works too, in the launcher as well (A selects, Start plays).

Logs and caches are in `Android/data/dev.narutorecomp/files` on the phone's storage.

## Online mode (experimental)

The game's own Online Battle mode works peer to peer, without Xbox Live: the SDK fakes a signed-in Xbox Live Gold profile and the game's matchmaking, and the two games talk to each other directly. It is **experimental**: expect disconnects and desyncs, and keep your expectations low.

**Tested:** Linux PC and Android phone on the same Wi-Fi. With the phone hosting, a Player Match reaches the fight and both screens stay in step. With the PC hosting, the phone gets through character select; a full fight in that direction has not been tested yet. Windows online is untested.

### Start it

Both players need this build, and each one sets the **other** player's IP address:

| Platform | How |
| --- | --- |
| Windows | Start menu, **Naruto Storm 3 - Online (experimental)**. It asks for the other player's IP and shows yours. |
| Linux | `bash tools/run-radeon.sh --native-only -- --netplay=true --netplay_peer=192.168.0.4` |
| Android | Add `netplay = true` and `netplay_peer = "192.168.0.17"` to `Android/data/dev.narutorecomp/files/user/naruto.toml`. |

Then in the game: **Online Battle, Player Match**. One player creates a match; the other searches for it (or uses Quick Match). Leaving the peer empty finds games on your local network only.

### Playing with someone far away

The two machines must reach each other directly. The easiest way is a free mesh VPN such as [Tailscale](https://tailscale.com/) or ZeroTier: both players install it, join the same network, and use each other's VPN address (`100.x.y.z` on Tailscale) as the peer. Plain internet play through router port forwarding has not been made to work yet.

### Ports and firewalls

Allow these through the firewall on both machines (Windows asks the first time the game goes online; allow it on private networks):

| Port | Use |
| --- | --- |
| UDP 36000 | Lobby: finding and joining matches |
| TCP 44996 | Game connection (the game's port 8996, moved up by 36000) |
| UDP 37000-37001 | Game traffic (the game's ports 1000-1001, moved up by 36000) |

### Known limits

- Two players only. Only Player Match has been tried; ranked matches, leaderboards and friends lists are not expected to work.
- No internet matchmaking: you must know the other player's address.
- Both games must be built from the same version of this project.
- Logs: lines starting with `[netplay]` in the log file. Include them when you report a problem.

Technical details are in [`patches/README.md`](patches/README.md#netplay-online-play-without-xbox-live).

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

  Both upscalers ran at the full 30 FPS on an RTX 2050, so **render scale 1 with FSR is the recommended setting**, at any window size: sharper than plain scaling without the slowdown of 2x. They need an SDK set up with `tools/setup-sdk.sh` or `.ps1` (the Windows installer runs it), which downloads AMD's FidelityFX headers (about 5 MB).

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
- **Windows: a tool is "not on PATH" right after the installer installed it:** close the window and run `install-windows.bat` again.
- **Windows: "path too long" errors:** move the repository to a short path such as `C:\dev\naruto-recomp`, or enable long paths (admin PowerShell: `Set-ItemProperty HKLM:\SYSTEM\CurrentControlSet\Control\FileSystem LongPathsEnabled 1`).
- **`check-game.py` reports a different SHA-256:** your copy is a different version. It won't work with this function list.
- **The first launch stutters:** shaders are compiled and cached the first time each effect appears. Later runs are smoother.
- **Low FPS:** run on the dedicated GPU, and use render scale 1 with the FSR upscaler.
- **Updating:** on Windows, run `install-windows.bat` again. On Linux, run `tools/setup-sdk.sh` again if `patches/` changed; if `naruto_manifest.toml` changed, repeat all four build commands, otherwise only the last one.
- **Online: the other player's match is not found:** check both IPs, the firewall ports above, and that both machines can ping each other.

## Reporting problems

Open an issue at [Issues, New issue](https://github.com/wizydeka-lang/naruto-storm3-recomp/issues/new/choose) and pick the form that fits:

| Form | For |
| --- | --- |
| **Bug report** | Crashes, freezes, wrong graphics or sound in the game |
| **Build or install problem** | The Windows installer, SDK setup or build fails |
| **Online mode problem** | The experimental online mode does not connect or desyncs |
| **Feature request** | Ideas and improvements |

A good report has:

1. **Your platform and hardware:** CPU, GPU and driver version; on Android, the phone model.
2. **The project version:** `git log --oneline -1`, or the date you downloaded it.
3. **What you did and what happened**, step by step, with a screenshot or short video if you can.
4. **The log file**, attached:
   - Windows and Linux: the newest file in the `logs` folder next to the executable (`out/build/win-amd64-release/logs` or `out/build/linux-amd64-release/logs`).
   - Android: `Android/data/dev.narutorecomp/files/naruto.log`.
   - Build problems: the last 50 lines of the build output, as text.
5. For online problems, the logs from **both** players.

Search the existing issues first; adding your details to a matching issue helps more than a duplicate. **Never attach game files, `default.xex`, `generated/` code, or built executables or APKs.** They contain the game and will be removed.

## Why there are no ready-made downloads

A built `naruto.exe` or Android APK contains the game's own code, recompiled from `default.xex`, plus shader data from the game. Sharing it would mean sharing the game, so this project only publishes source code, and everyone builds from their own copy. The Windows installer makes this mostly automatic.

## How the project is organised

| Path | Contents |
| --- | --- |
| `naruto_manifest.toml` | Codegen settings and the function boundaries that had to be found by hand |
| `src/` | Application code: startup, the native Vulkan renderer, texture decoding, input |
| `android/` | Android app (launcher, touch controls) |
| `patches/` | Fixes applied to the ReXGlue SDK (see `patches/README.md`) |
| `config/` | Default runtime settings |
| `tools/` | SDK setup, Windows installer, game check, run scripts, launcher and build checks |
| `third_party/` | Small libraries used by the renderer, with their licenses |
| `docs/` | Development notes |

Generated code (`generated/default/`) and build output (`out/`) are created on your machine and are not part of the repository.

## Legal

This project includes no game code or assets. You must own the game and supply your own files. Do not share `assets/`, `generated/default/` or built executables, since they contain or are derived from the original game.
