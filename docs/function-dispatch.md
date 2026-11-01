# Preventing missing guest function entries

Linux builds check explicit functions from `naruto_manifest.toml` with
`tools/verify_function_dispatch.py`. Python 3.11 or newer and GNU nm or
llvm-nm are required. The checker uses only Python's standard library.

The project CMake integration runs the check after ReXGlue code generation
and before compiling the game. Each configured standalone function must have
a generated `SetFunction` entry. Shared chunks with `parent` are checked
through their parent's registration instead. Included configuration files
are processed depth first, with inline function entries taking precedence,
matching the installed ReXGlue v0.10.0 configuration loader.

After linking, the check also requires each registered function symbol in the
executable. A missing entry, stale executable, stripped symbol table, or failed
nm invocation fails the build with an error. Check before stripping a packaged
binary. These checks stay in the project CMakeLists rather than the generated
SDK build integration, so regeneration does not erase them.

Build normally:

```bash
cmake --build --preset linux-amd64-debug --parallel 4
```

To also recheck an existing executable when no relink is needed:

```bash
cmake --build --preset linux-amd64-debug --target naruto_verify_functions --parallel 4
```

Run the checker's regression tests:

```bash
python3 -B -m unittest discover -s tests -p 'test_verify_function_dispatch.py'
```

## Additional verified thunk

The manifest now includes `0x8263B1E0`, size `0x10`. Original XEX disassembly:

```text
8263B1DC  4E800420  bctr                 ; previous function ends
8263B1E0  81830000  lwz r12,0(r3)
8263B1E4  816C0008  lwz r11,8(r12)
8263B1E8  7D6903A6  mtctr r11
8263B1EC  4E800420  bctr
8263B1F0  7D8802A6  mflr r12             ; next function begins
```

It is separate code between two established function boundaries. No runtime
crash at this address was required to discover it. The earlier 131-function
manifest correction remains in place.

## Limits and investigation

These checks protect configured entries and their compiled implementations;
they do not prove that every possible runtime callback has been discovered.
The SDK's static call validation primarily checks direct branch targets.
A scan of this XEX for the specific four-instruction thunk pattern above
found 164 matches; other function shapes need separate analysis.

Validation on October 31, 2026: all 164 matching sequences are now registered
and compiled. The normal Debug build passed the automatic generation and
linked-symbol checks for 252 configured function/parent entries. All eight
checker regression tests passed, and an additional real-ELF negative check
correctly rejected an absent callback symbol. The patched Vulkan plugin was
preserved. The game has not been rerun for this change.

For another invalid indirect call, preserve its log and inspect the original
instructions, function boundaries, caller state, and target pointer. Add a
manifest entry only when the target is confirmed as a legitimate independent
entry point. Incorrect runtime pointers need a different fix. Regenerate and
rebuild after each authorized manifest change; do not edit generated C++.

References: the ReXGlue [codegen pipeline wiki](https://github.com/rexglue/rexglue-sdk/wiki/Codegen-Pipeline-Overview),
verified against installed SDK `v0.10.0-2-gc94f5eb`, particularly
`src/codegen/config.cpp`, `manifest.cpp`, `phase_validate.cpp`, and
`src/system/function_dispatcher.cpp`. This SDK uses the manifest's
`[entrypoint.functions]` section; older wiki examples show `[functions]`.
