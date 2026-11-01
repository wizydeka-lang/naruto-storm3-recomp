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
