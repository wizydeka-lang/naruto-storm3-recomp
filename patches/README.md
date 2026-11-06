# ReXGlue Vulkan texture exponent correction

`rexglue-vulkan-texture-exponent.patch` applies to SDK
`v0.10.0-2-gc94f5eb` (`c94f5ebdcb3c9d1a460ca48e04f9758448f8d518`).
It fixes the SPIR-V texture fetch translator, not generated PowerPC code.

The Xenos texture descriptor stores signed `exp_adjust` in word 3, bits 13–18
(`include/rex/graphics/xenos.h`). The Vulkan translator read those bits from
word 4, which contains LOD bias. In the captured character-selection scene,
descriptor words 3 and 4 are `0x01280D10` and `0x003CD003`. The intended exponent
is zero, but the old shader extracted -26 and multiplied all sampled channels
by approximately 0.0000000149. Those colors round to black in an RGBA8 target.
The correction loads word 3 for color scaling and retains word 4 for LOD.

The executable loads the GPU plugin dynamically; PowerPC regeneration is not
required for this shader-translator correction.

`tools/setup-sdk.sh` (Linux) and `tools/setup-sdk.ps1` (Windows) apply both
patches to a clean SDK checkout at the revision above. To apply one by hand,
from the project root, with `SDK` set to the SDK checkout:

```bash
git -C "$SDK" apply --check "$PWD/patches/rexglue-vulkan-texture-exponent.patch"
git -C "$SDK" apply "$PWD/patches/rexglue-vulkan-texture-exponent.patch"
(cd "$SDK" && cmake --build --preset linux-amd64-release --target install --parallel 4)
```

The game build copies the rebuilt plugin beside the executable when it next
links; otherwise copy `out/install/linux-amd64/lib64/librexgpu-xenos.so` from
the SDK yourself.

Close Naruto before replacing a loaded plugin. Do not apply the patch again
to the already-patched checkout. A later SDK update can replace this change;
check whether its translator already loads word 3 before reapplying.

## Verification

- Debug GPU plugin compilation and SDK Debug installation succeeded.
- `git diff --check` passed in the SDK.
- Installed and staged plugins have matching SHA-256:
  `08b62f47d16a827a514fb33530d725f538d4cf4d1af3de397d96cda4619b4935`.
- An isolated RenderDoc replay of `testframe2.rdc`, event 1370, changed only
  the faulty exponent descriptor load. Previously black pixels became
  `(211, 124, 40)` in the VS artwork and `(0, 71, 0)` in a menu tile.
  The original RDC was not modified. This verifies the captured shader's
  failure mechanism; it does not by itself verify the complete live game.
- Replay scripts, before/after PNGs, JSON evidence, and build/install logs
  are in `.tools/gpu-debug/`. Original plugin backups are in
  `.tools/gpu-debug/backups/before-texture-exponent-fix/`.
- The patched native game was launched on Radeon 660M with PipeWire and
  synchronous shader compilation and default dynamic rendering. The user
  confirmed that the live character-selection menu is now visible. Gameplay
  beyond that menu has not been verified.

The unchanged build remained black with dynamic rendering disabled. This
patch therefore keeps the default dynamic-rendering path.

## Presentation semaphore reuse

`rexglue-vulkan-present-semaphores.patch` applies independently to the same SDK
revision. It fixes the separate validation error
`VUID-vkQueueSubmit-pSignalSemaphores-00067` in the host window presenter.
Present-wait semaphores now belong to swapchain images. Reacquiring an image
protects reuse of its semaphore; a submission fence alone does not cover the
previous presentation operation. See the [Khronos explanation](https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html).

The retirement path also waits for the present queue before destroying the
old image resources. This is the conventional queue-idle retirement approach;
explicit presentation-completion fencing would require a swapchain-maintenance
extension that this SDK does not enable.

Both Debug and Release runtimes were rebuilt and installed. Both game builds
passed all 252 configured function/parent dispatch checks. A 35-second Release
run on Radeon 660M with the local Khronos validation layer enabled reported
zero VUID diagnostics, including fullscreen/windowed swapchain recreation.
This verifies the exercised presentation path, not every resize/error path or
a gameplay speed improvement. Evidence is in
`out/performance/runs/semaphore-validation/logs/naruto_001.log`.

Close Naruto before installing either runtime. Apply this patch to a clean SDK
using the same `git apply --check` procedure above, then build/install the
matching runtime and GPU plugin and rebuild the game as described in
`docs/performance.md`. The shader correction remains necessary as a separate
patch. Pre-change runtimes are backed up under
`out/performance/backups/before-present-semaphore-fix/`.

## Android build support

`rexglue-android.patch` applies independently to the same SDK revision. It lets
the SDK compile and link for Android arm64 with NDK r30 (Clang 21, libc++).
NDK r27 is too old: its libc++ lacks floating-point `std::from_chars` and
`std::jthread`. Desktop builds are unaffected.

- `chrono.h`: supplies `clock_time_conversion` and `clock_cast`, which the
  NDK's libc++ lacks, as Apple's libc++ already needed.
- `main_android.h` (new): `GetAndroidApiLevel()`, which `memory_posix.cpp` and
  `threading_posix.cpp` call but the SDK never defined.
- `fiber_posix.cpp`, `fiber.h`: bionic has no `getcontext`, `makecontext` or
  `swapcontext`, so Android fibers switch with a small arm64 assembly routine
  that saves the AAPCS64 callee-saved registers on the fiber's own stack.
- `threading_posix.cpp`: bionic has no robust mutexes; Android uses plain
  mutexes, as macOS does.
- `filesystem_posix.cpp`: `OpenAndroidContentFileDescriptor` opens a
  `content://` URI from the Android file picker through `ContentResolver`,
  using SDL's JNI environment and activity.
- `surface_android.h`/`.cpp` (new), `window_sdl.cpp`: present to the SDL
  window's `ANativeWindow` instead of X11 or Wayland.
- `gpu_plugin_loader.cpp`: on Android the executable is the system's
  `app_process`, so GPU plugins are loaded from the folder holding
  `librexruntime.so`, the app's native library folder.
- `windowed_app.h`: Android no longer switches to Xenia's several-apps-in-one-
  library registration. SDL's activity runs one game per app, so the game
  defines `GetWindowedAppCreator()` as on desktop.
- CMake: links `android` instead of X11/Wayland, and not `pthread`/`rt`, which
  are part of bionic's libc.

Configure the SDK with the NDK toolchain in its own build directory:

```bash
cmake -S "$SDK" -B "$SDK/out/build/android-arm64" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$HOME/Android/android-ndk-r30/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -DANDROID_STL=c++_shared \
  -DCMAKE_BUILD_TYPE=Release -DREXGLUE_ENABLE_FIDELITYFX=OFF -DREXGLUE_ENABLE_TRACY=OFF
cmake --build "$SDK/out/build/android-arm64"
```

The SDK names this platform `linux-arm64`, so its libraries land in
`out/linux-arm64/`; do not build a real Linux arm64 SDK in the same checkout.

## Verification

- The complete SDK, including `librexruntime.so` and `librexgpu-xenos.so`,
  builds for Android arm64; the libraries use 16 KB segment alignment.
- The game builds against it as `libmain.so` with the `android-arm64-release`
  preset, and passes all 252 function/parent dispatch checks.
- The desktop Linux SDK still builds with the patch applied.
- None of this has run on a device yet. The fiber switch, content URIs and the
  Android surface are untested at run time, and guest memory on 16 KB-page
  devices remains an open question.

# Skipping Xenia's rendering

`rexglue-vulkan-skip-rendering.patch` adds the `vulkan_skip_rendering` option.
With it set, the Vulkan command processor still reads the command stream, so
fences, interrupts and frame pacing keep working, but draws, resolves and
presents do nothing. The native view (`src/native_view.cpp`) then does all the
rendering; `bash tools/run-radeon.sh --native-only` turns both on. Resolved
data never reaches guest memory in this mode, so anything the game reads back
on the CPU from a resolve sees stale memory.

## Netplay (online play without Xbox Live)

`rexglue-netplay.patch` adds the `netplay` setting (off by default). It fakes
just enough of Xbox Live for the game's own online code to run peer to peer
on a local network. Apply it after `rexglue-android.patch`.

```bash
naruto --netplay=true --netplay_peer=192.168.0.4   # the other machine's IP
```

On Android, put `netplay = true` and `netplay_peer = "<PC IP>"` in
`files/user/naruto.toml`. Both sides then use Online Battle → Player Match:
one hosts, and the other searches or uses Quick Match.

What the patch does with `--netplay=true`:

- **Sign-in.** Player 1 is signed in to Xbox Live with every privilege and Gold.
  `XamGetSystemVersion` reports a current dashboard. Without it the game skips
  `XSessionGetDetails` and crashes when hosting.
- **Lobby.** A lobby on UDP 36000 answers session searches with the session
  hosted on this machine. It asks `netplay_peer` and broadcasts on the LAN.
- **Ports.** Game socket ports move up by `netplay_port_offset` (36000). The
  game's TCP 8996 becomes 44996 and its UDP 1000/1001 become 37000/37001, so
  allow those through firewalls.
- **Sessions.** Create, join, leave, modify, details, start, end and delete
  are implemented. A joining copy reports the host's slot counts. The game
  sizes its peer table from `XSessionGetDetails` and otherwise writes through
  a null pointer (guest 0x54) once the match starts.
- **Winsock on POSIX.** Option and ioctl constants are translated, errno is
  mapped to WSA codes, and `select` gets a real `nfds`. Socket handles are
  positive, because the game treats negative ones as invalid. `sendto` and
  `recvfrom` convert address byte order: before this, every UDP packet went
  to a byte-swapped IP.
- **Logging.** Calls are logged with a `[netplay]` prefix, capped at the first
  40 of each kind.

The Android app needs the `INTERNET` permission (in the manifest).

Tested on one Wi-Fi network between a Linux PC and an Android phone. With the
phone hosting and the PC joining, a Player Match reaches the fight and both
screens stay in step. With the PC hosting, the phone joins and gets through
character select. That direction has not yet been played into a fight with
the UDP fix.
