# Radeon 660M gameplay build

Launch the optimized Linux build from the project root:

```bash
bash tools/run-radeon.sh
```

The launcher uses `out/build/linux-amd64-release/naruto`, RADV on the Radeon
660M, SDL X11, and PipeWire audio. `SDL_AUDIO_DRIVER=dummy` selects silent dummy
audio when needed. `NARUTO_SDK_PREFIX` overrides the installed SDK prefix;
the default is `$HOME/rexglue-sdk/out/install/linux-amd64`.

The Release preset explicitly enables `-O3 -DNDEBUG`. The matching Release SDK
runtime and GPU plugin were rebuilt from the existing SDK checkout so that
the texture-exponent rendering fix and native SDL audio backends are included.
Project CMake stages the correct GPU plugin beside each executable and keeps
the function-registration verification checks active.

`config/naruto-performance.toml` seeds the Release build's `naruto.toml` on
its first configure. Existing local settings files are preserved on subsequent
configures. The profile uses info-level logs, asynchronous shader compilation,
native guest resolution, and display-paced FIFO presentation. Validation and
GPU markers are disabled for normal gameplay. Warnings and errors remain
visible. The launcher also sets the presentation and shader options explicitly.
Some first-use shader preparation can still cause short pauses or skipped
frames; this setting does not guarantee that every scene is already cached.

The Debug configuration's old GPU trace and APU debug logging were reduced
to info. Its previous settings are backed up at
`out/performance/backups/naruto-debug.toml`.

## Rebuild

From an SDK checkout set up by `tools/setup-sdk.sh` (default `~/rexglue-sdk`),
run from the project root:

```bash
(cd ~/rexglue-sdk && cmake --build --preset linux-amd64-release --target install --parallel 4)
cmake --preset linux-amd64-release \
  -DCMAKE_PREFIX_PATH=$HOME/rexglue-sdk/out/install/linux-amd64
cmake --build --preset linux-amd64-release --parallel 4
```

SDL only builds its PipeWire and PulseAudio backends when their development
packages are installed before the SDK is configured. A stock SDK installation
may lack these backends or the texture-exponent fix.

## Comparison runs

Preflight checks do not launch the game or write files:

```bash
bash tools/run-radeon.sh --check
bash tools/run-radeon.sh --debug --check
```

To profile guest-output updates without enabling GPU trace spam:

```bash
bash tools/run-radeon.sh --profile
# Optional matching Debug comparison:
bash tools/run-radeon.sh --debug --profile
```

Each profile run copies the selected executable and its GPU plugin into a new
directory under `out/performance/runs/`, with GPU debug logging and APU info
logging. The normal game configuration is not changed. These diagnostic logs
contain one `XELOG_GPU PRESENT` message per refreshed guest output. Compare
the same characters, stage, camera, and gameplay phase after shader warm-up.
Do not compare a loading screen with a battle, or benchmark while compiling.
Log intervals measure guest-output updates, not raw GPU execution time or
monitor presentation rate. Release disables the SDK's normal perf-counter
macros, so the comparison uses these log timestamps rather than claiming
results from a disabled FPS counter.

## Initial evidence

The previous unoptimized Debug run wrote 108,229,755 bytes of logs over
410.783 seconds, including 426,422 GPU trace messages and 303,045 APU debug
messages. Logged guest-output intervals had a median of 49 ms, a 95th
percentile of 82 ms, and a maximum pause of 982 ms. This covered mixed scenes
and is not a controlled benchmark. Baseline data, backups, and build logs are
kept under `out/performance/`.

Release PipeWire, PulseAudio, and dummy backend probes all initialized and
drained a silent 48 kHz stream successfully. This verifies backend operation,
not every aspect of in-game audio or graphics performance.

## Native-resolution investigation

The user chose to retain the original rendering resolution. Guest output stays
1280×640 at 1× scale. The optimized Release setup alone did not resolve the
reported continuous gameplay slowness.

In one fixed slow menu/dialog scene, guest-output updates remained around
50 ms with the Radeon reporting full utilization. Disabling forced anisotropic
filtering, switching FIFO to immediate presentation, disabling occlusion
queries, bypassing memory-page invalidation, changing guest wait yielding, and
batching primary command buffers did not improve that scene. These diagnostic
overrides are not enabled by the normal launcher. The memory-coherence and
query behavior remain at their defaults. Results from different scenes are
not treated as controlled speed comparisons.

A separate confirmed presentation-semaphore defect has been corrected in the
SDK runtime; the patch and validation evidence are described in
`patches/README.md`. It is a synchronization correction, with no measured speed
improvement claimed. Subsequent native-resolution battle measurements are
needed to determine the remaining bottleneck.
