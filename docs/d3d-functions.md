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
| `+0x2880` | register shadows: `0x2000`–`0x200F` (`RB_SURFACE_INFO`...) | draw functions flush them with `sub_82749108` |
| `+0x28CC` | register shadows: `0x2100`–`0x2114` (`RB_COLOR_MASK`, `RB_ALPHA_REF`, viewport scale/offset) | as above |
| `+0x2920` | register shadows: `0x2180`–`0x2184` | as above |
| `+0x2934` | register shadows: `0x2200`–`0x220B` (`RB_DEPTHCONTROL`, `RB_BLENDCONTROL0`, `RB_COLORCONTROL`, `PA_SU_SC_MODE_CNTL`, `PA_CL_VTE_CNTL`, `RB_MODECONTROL`) | as above |
| `+0x2FD0` | current vertex declaration (likely) | `SetVertexDeclaration` |
| `+0x320C` | current index buffer | `SetIndices`; `DrawIndexedVertices` reads its header |
| `+0x330C` | current pixel shader | `SetPixelShader` |
| `+0x3310` | current vertex shader | `SetVertexShader` |

`sub_82749108(device, mask, first register, shadow)` writes one register per
set mask bit, from the most significant bit, so register *first + k* is at
*shadow + 4k*.

An index buffer header keeps the Xbox 360 `D3DResource` layout: `Common` at
`+0` (bit 31 set for 32-bit indices, bits 29–30 the endian swap) and the
data's virtual address at `+0x18`. Vertex buffers are only known through the
vertex fetch constants at `+0x480` (physical address, size, endian swap).

## Shader objects

A vertex shader object keeps variants at `+896`, eight bytes each. The
per-draw commit `sub_8274A238` uses the first variant, or the one at `+904`
when bit `0x20` of `+872` is set. A variant's microcode is at
`*(shader + 32) + *(variant + 872)`, `*(variant + 876)` bytes long. Binding a
vertex declaration patches the vertex fetch instructions in place, so the
bound microcode describes the current vertex layout. A pixel shader's info
block is at `shader + *(shader + 64)`; its microcode is at
`*(shader + 24) + *(info + 40)`, `*(info + 44)` bytes long.

## Textures and boolean constants

Pixel shader sampler register *n* uses texture fetch constant slot *n* (device
`+0x480 + 24n`, six words in the Xenos layout). Boolean constants (registers
`0x4900`–`0x4907`) are shadowed at `+0x2780`: vertex shader `b0`–`b127` in the
first four words, pixel shader `b0`–`b127` in the next four.

Vertex buffers use the 8-in-32 endian swap. For formats with 16-bit components
(`16_16`, `16_16_FLOAT`...), the component first in memory (X) then sits in
the high half of each word, so a host renderer has to swap the halves.

## Resolve and present

`D3DDevice_Resolve` (`sub_8275BD48`) wraps `sub_8275ABF0`. Its arguments are
the device, flags, an optional source `D3DRECT`, the destination texture
object, an optional destination `D3DPOINT`, the mip level, the slice, the
clear color, and the clear depth in `f1`. Flag bits 0–2 select render target
0–3, or 4 for the depth buffer; `0x100` clears the render target afterwards
(`sub_8275A8B8`) and `0x200` the depth buffer (`sub_8275A320`). A texture
object's fetch constant is at `+0x1C` (after the `D3DResource` header and
`MipFlush`), and the resolve reads the destination's size and format from it.

The game resolves with flags `0x0`, `0x1` and `0x14` (render target 0, 1,
and the depth buffer) and never clears: no resolve sets `0x100` or `0x200`,
`Clear` is never called, and passes start drawing with depth testing on. Only
`sub_8274B330`, D3D's per-tile resolve for predicated tiling, adds `0x200`,
and the engine doesn't reach it in normal play. The native view clears each
depth buffer to 1.0 the first time a frame uses it.

`D3DDevice_Swap` (`sub_82746748`) takes the front buffer texture as its second
argument and copies its fetch constant; the game resolves its final image
into that texture (8888, 1280×640) just before.

Render targets are configured by `RB_SURFACE_INFO` (pitch in bits 0–13),
`RB_COLOR_INFO`/`RB_COLOR1_INFO`–`RB_COLOR3_INFO` (EDRAM base in bits 0–11,
format in bits 16–19) and `RB_DEPTH_INFO`, all in the `0x2000` register
shadow block.

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

- How the game clears render targets (probably with full-screen quads).
- Where the device keeps boolean shader constants.
- `sub_8274ABB8`, `sub_8274D0F8`, `sub_8274D230`, `sub_8274CA60` and
  `sub_8274CFC8`: load-time entry points the engine calls, probably buffer
  header and lock helpers.
- `sub_827452C0` / `sub_82745300`: a begin/end pair around device field
  `+0x2B08`, three times per frame.
