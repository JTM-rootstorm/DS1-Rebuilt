# DS1-Rebuilt

A static recompilation of **Dungeon Siege** (PC, 2002) to a native 64-bit
Linux program. The game's 32-bit x86 machine code is translated ahead of
time into C++ and linked against a runtime that reimplements the parts of
Windows the game uses (Win32, DirectDraw 7 / Direct3D 7, Miles Sound
System, Bink). There is no emulator, interpreter or Wine in the result,
and no game code or data in this repository: you need your own copy of
the game, and the build reads it from where you installed it.

## Status

Single-player is playable: menus, hero creation, the intro cinematic,
the opening farm and the forest beyond, combat, saving and loading,
sound, music, voices and movies. The game holds its own 60 fps cap in
the scenes tested. Work continues; [FINDINGS.md](FINDINGS.md) tracks
what works, what was fixed and what is known to be missing.

Linux x86-64, and macOS on Apple Silicon (native Metal): on the Mac it
builds, passes its tests and plays into the game world in scripted runs
(the intro, the farm) at a steady 60 fps, also fullscreen on a 5K
display, but has not yet been played by hand there. The code is portable
(no x86 intrinsics, no `long double`, no host FPU state).

## How it works

The machinery lives in
[x86recomp](https://github.com/JTM-rootstorm/x86recomp), a library shared
with other PC game recomps and checked out next to this repository; this repository holds
what is specific to Dungeon Siege (configuration, lifter manifest, game
hooks, docs).

- **Lifter** (x86recomp `tools/lifter`): decodes the EXE with Zydis and
  emits C++ for every function, one basic block at a time, with exact
  x86 integer and flag semantics. Straight-line x87 code becomes guarded
  "runs" that keep the FPU register stack in locals.
- **x87 model** (x86recomp `src/runtime/x87.cpp`): the game depends on
  80-bit x87 behaviour (precision and rounding control, status-word
  flags), so the FPU is modelled exactly on Berkeley SoftFloat with
  integer fast paths. Tests compare it bit for bit against a real x87.
- **Runtime and HLE** (x86recomp `src/runtime`): the guest's 4 GiB
  address space, threads, SEH, and every imported Windows/DirectX/Miles/
  Bink function, implemented on SDL3 (graphics on SDL_GPU: Vulkan, or
  Metal on macOS) and ffmpeg.

x86recomp's ARCHITECTURE.md describes the design in detail;
[ARCHITECTURE.md](ARCHITECTURE.md) here covers the Dungeon Siege parts.

## What you need

- Dungeon Siege 1 (the GOG release, version 1.11.1, was used for all
  development). DRM-free copies only.
- Linux x86-64 with a Vulkan driver.
- Clang and LLVM (22 used; the static `libLLVMDemangle.a` and its
  headers are needed), CMake 3.25 or newer, Ninja, Python 3.
- SDL3 and ffmpeg (libavformat, libavcodec, libavutil, libswscale)
  development packages.
- glslang (`glslangValidator`), to build the shaders.
- Ghidra (12.1 used), headless, to find the game's functions.

## Building

Paths below assume the game is in `$GAME` and this repository in `$REPO`,
with an x86recomp checkout at `$REPO/../../x86recomp` (pass
`-DX86RECOMP_DIR=...` to cmake otherwise):

```sh
git clone https://github.com/JTM-rootstorm/x86recomp.git "$REPO/../../x86recomp"
```

1. **dxvk-native** (optional and off by default; DXVK v3.1.1, Direct3D 9
   only, SDL3 window system, built with Meson). It adds the d3d9 backend
   (`DS1_GFX=d3d9`), a reference image for the default SDL_GPU backend;
   skip this step otherwise:

   ```sh
   git clone --branch v3.1.1 https://github.com/doitsujin/dxvk.git
   cd dxvk
   meson setup build-native -Dbuildtype=release -Dprefix="$PWD/../dxvk-native" \
     -Denable_dxgi=false -Denable_d3d8=false -Denable_d3d10=false \
     -Denable_d3d11=false -Dnative_sdl3=enabled -Dnative_sdl2=disabled \
     -Dnative_glfw=disabled
   ninja -C build-native install
   ```

   Then configure with `-DDXVK_NATIVE_DIR="$PWD/../dxvk-native"` (the
   install prefix) to build the backend; without it the build has
   SDL_GPU only.

2. **Configure and build the lifter**:

   ```sh
   cd "$REPO"
   cmake --preset release -DRECOMP_LLVM_ROOT=/usr/lib/llvm/22
   cmake --build --preset release --target recomp_lift
   ```

3. **Symbols** from your copy of the EXE (written to `symbols/`, which
   is ignored by git):

   ```sh
   X86RECOMP="$REPO/../../x86recomp"
   python3 -I "$X86RECOMP/tools/pe_symbols.py" "$GAME/DungeonSiege.exe" --out symbols
   mkdir -p /tmp/ds1_ghidra
   bash /usr/share/ghidra/support/analyzeHeadless /tmp/ds1_ghidra ds1 \
     -import "$GAME/DungeonSiege.exe" -scriptPath "$X86RECOMP/tools/ghidra" \
     -postScript ExportFunctions.java symbols/functions.tsv \
     -postScript ExportJumpTables.java symbols/jumptables.tsv
   ```

   Ghidra's plain auto-analysis is all that is needed: with Ghidra 12.1
   it reproduces the development function and jump-table lists byte for
   byte. (`pe_symbols.py` uses `llvm-undname` from your LLVM install.)

4. **Lift** the game and build it (the lifted C++ goes to `generated/`,
   also ignored):

   ```sh
   out/build/release/x86recomp/tools/lifter/recomp_lift \
     --exe "$GAME/DungeonSiege.exe" --symbols symbols --out generated
   cmake --preset release
   cmake --build --preset release
   ```

5. **Run**:

   ```sh
   out/build/release/ds1 --game-dir "$GAME"
   ```

   Settings and saves live under `$XDG_DATA_HOME/ds1` (by default
   `~/.local/share/ds1`); crash reports and hang dumps go to `crashes/`
   and `hangs/` there.

**macOS** (Apple Silicon): no dxvk-native; SDL_GPU runs on Metal. With
Homebrew (`brew install cmake ninja sdl3 ffmpeg llvm@22 glslang
spirv-cross ghidra`), steps 2-5 are the same with these differences:
CMake finds Homebrew's LLVM; `pe_symbols.py` needs
`/opt/homebrew/opt/llvm@22/bin` on `PATH` (for `llvm-undname`); Ghidra's
headless script is
`$(brew --prefix ghidra)/libexec/support/analyzeHeadless`, run with
`JAVA_HOME=$(brew --prefix openjdk@21)/libexec/openjdk.jdk/Contents/Home`
(`brew install openjdk@21`; without a terminal it cannot ask for the
JDK). Settings and saves live under `~/Library/Application Support/ds1`.

The `debug` preset works the same way (`out/build/debug`); lifted code
and the x87 model are optimised in both.

## Tests

`ctest --test-dir out/build/<preset>` runs the x87 model against the
host CPU's own x87 and against its SoftFloat reference, and every x87
and integer instruction form found in the EXE (plus random x87
sequences) lifted versus executed natively. The EXE-derived tests need
`-DDS1_GAME_EXE="$GAME/DungeonSiege.exe"` at configure time.

## Useful switches

- `DS1_TRACE=1` logs every call into the reimplemented Windows API
  (`DS1_TRACE_FILTER=mss32` narrows it).
- `DS1_X87_RUNS=0` turns the lifted x87 runs off (per-instruction model
  only), for comparisons.
- `DS1_VSYNC=1` turns vsync on (off by default: the game caps itself at
  60 fps and frames go out in tear-free mailbox mode).

## Legal

Dungeon Siege is © Microsoft / Gas Powered Games; the franchise now
belongs to Square Enix. This project is not affiliated with or endorsed
by any of them. It contains no game code, data or assets: the lifted
code is generated locally from your own copy and is never committed.
Third-party binaries that ship with the game (such as `dbghelp.dll`) are
used only as behaviour references during development and are never
redistributed.

The code here is under the [MIT License](LICENSE). Dependencies keep
their own licences: Berkeley SoftFloat 3e (BSD-3-Clause) and Zydis (MIT)
are fetched at build time; SDL3 (zlib), DXVK (zlib), ffmpeg (LGPL,
linked dynamically) and LLVM's demangler (Apache-2.0 with
LLVM exception) come from your system.
