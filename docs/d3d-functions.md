# Direct3D functions in default.xex

`config/naruto_d3d_funcs.toml` lists the game's Xbox 360 Direct3D functions
that a native renderer would hook instead of emulating the Xenos GPU. This page
records how they were found, so the list can be extended and checked.

## How the game reaches Direct3D

The game never calls Direct3D directly. CyberConnect2's engine renderer
(`0x827EB000`–`0x827FC000`) is reached through C++ virtual calls and calls the
D3D draw functions. Render state is set through a per-device table of setters
rather than through a static table in the image, and the draw functions read
the device's state when they run. A native renderer can therefore hook the
draws and read the device object, instead of hooking every setter.

## Evidence

- **Present.** `sub_82746748` is the only function calling `VdGetSystemCommandBuffer`
  and `VdSwap`, and runs exactly once per frame (19,493 calls in a 711-second
  trace at 27.5 frames per second).
- **Draws.** Two functions account for all 342 draws per frame: `sub_827518D8`
  (301/frame) and `sub_827514C0` (41/frame). The engine calls the first with
  five arguments `(device, 6, 0, offset >> 1, count)`, the
  `DrawIndexedVertices` signature with a triangle strip, and the second with
  four `(device, 13, 0, 4)`, `DrawVertices` with a four-vertex quad list.
- **Shaders.** `sub_82750838` and `sub_82750630` store their argument at device
  `+0x3310` and `+0x330C` and set dirty bits; the per-draw commit
  `sub_8274A238` reads those two fields and `+0x2FD0` first, then writes the
  `IM_LOAD` shader packet.
- **Render and sampler states.** At runtime the heap holds one table of 242
  code pointers: 121 setters followed by 121 getters, each half starting with
  10 stub entries. Xbox 360 render state values are byte offsets
  (`D3DRS_ZENABLE` is 40), so entry *n* is state value 4*n*. The 111 real
  entries per half equal the XDK's 91 render states and 20 sampler states, and
  the Wrap0–Wrap15 getters are 16 consecutive 16-byte functions at the
  expected position.

## Device object fields

The engine keeps the device pointer at `0x82AC4384` (the second word of its
renderer object at `0x82AC4380`); in the traced runs the device was at
`0x40003880`.

| Offset | Contents | Evidence |
| --- | --- | --- |
| `+0x10` | 64-bit dirty flags | set by every setter (`ld`/`oris`/`std`) |
| `+0x40` | render/sampler state setters (121), then getters (121) | table found at `0x400038C0` = device + `0x40` |
| `+0x480` | fetch constants: 32 slots × 24 bytes (textures, vertex buffers) | `SetVertexFetchConstant` writes slot (95 − register) / 3 |
| `+0x780` | vertex shader float constants, 16 bytes per register | `SetVertexShaderConstantFN` |
| `+0x1780` | pixel shader float constants | `SetPixelShaderConstantFN` |
| `+0x2FD0` | current vertex declaration (likely) | `SetVertexDeclaration` |
| `+0x330C` | current pixel shader | `SetPixelShader` |
| `+0x3310` | current vertex shader | `SetVertexShader` |

## Resources are not created through Direct3D

D3D's allocator (`sub_82737308`, over `XPhysicalAlloc` at `sub_8272EF00`) is
only reached from `Direct3D_CreateDevice`. The engine allocates GPU memory
itself and fills resource headers with the XG helpers (`sub_8275DBF0`,
`sub_8275DE48`, `sub_8275DEE8`, called while loading textures, six times for
cube faces). A native renderer therefore learns about a texture or buffer when
it is bound, from the fetch constants at `+0x480`, not when it is created.

## Reproducing

The call counts come from an instrumented build that counts calls to every
guest function between `0x82700000` and `0x82810000`:

```bash
cmake --preset linux-amd64-release -B out/build/linux-amd64-trace \
  -DNARUTO_TRACE_GUEST_CALLS=ON -DCMAKE_PREFIX_PATH=$HOME/rexglue-sdk/out/install/linux-amd64
cmake --build out/build/linux-amd64-trace --parallel 4
NARUTO_TRACE_FILE=guest_calls.tsv ./out/build/linux-amd64-trace/naruto \
  --game_data_root="$PWD/assets" --gpu_plugin=xenos
```

The file gets each traced function's total call count once a second. The
render state table was read from guest memory at `0x400038C0`, through the
runtime's `/dev/shm/xenia_memory_*` file while the game ran (guest addresses
`0x40000000`–`0x7FFFFFFF` are at the same offset in that file).

## Not yet identified

- The shader object layout: the per-draw commit reads an offset at `+896` of
  the vertex shader object, which leads to its microcode. Needed to match bound
  shaders to the precompiled `.nsh` shaders.
- The register shadows after `+0x2780` (render target, depth, viewport, blend
  and raster state as the GPU sees them).
- `sub_8274ABB8`, `sub_8274D0F8`, `sub_8274D230`, `sub_8274CA60` and
  `sub_8274CFC8`: load-time entry points the engine calls, probably buffer
  header and lock helpers.
- `sub_827452C0` / `sub_82745300`: a begin/end pair around device field
  `+0x2B08`, three times per frame.
