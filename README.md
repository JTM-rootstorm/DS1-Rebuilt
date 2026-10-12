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

Linux x86-64 only for now. The code is written to be portable (no x86
intrinsics, no `long double`, no host FPU state); a macOS (arm64, Metal)
port is planned.

## How it works

- **Lifter** (`tools/lifter`): decodes the EXE with Zydis and emits C++
  for every function, one basic block at a time, with exact x86 integer
  and flag semantics. Straight-line x87 code becomes guarded "runs" that
  keep the FPU register stack in locals.
- **x87 model** (`src/runtime/x87.cpp`): the game depends on 80-bit x87
  behaviour (precision and rounding control, status-word flags), so the
  FPU is modelled exactly on Berkeley SoftFloat with integer fast paths.
  Tests compare it bit for bit against a real x87.
- **Runtime and HLE** (`src/runtime`): the guest's 4 GiB address space,
  threads, SEH, and every imported Windows/DirectX/Miles/Bink function,
  implemented on SDL3, dxvk-native (Direct3D 9 on Vulkan) and ffmpeg.

[ARCHITECTURE.md](ARCHITECTURE.md) describes the design in detail.

## What you need

- Dungeon Siege 1 (the GOG release, version 1.11.1, was used for all
  development). DRM-free copies only.
- Linux x86-64 with a Vulkan driver.
- Clang and LLVM (22 used; the static `libLLVMDemangle.a` and its
  headers are needed), CMake 3.25 or newer, Ninja, Python 3.
- SDL3 and ffmpeg development packages (libavformat, libavcodec,
  libavutil, libswscale).
- Meson, to build dxvk-native.
- Ghidra (12.1 used), headless, to find the game's functions.

## Building

Paths below assume the game is in `$GAME` and this repository in `$REPO`.

1. **dxvk-native** (DXVK v3.1.1, Direct3D 9 only, SDL3 window system):

   ```sh
   git clone --branch v3.1.1 https://github.com/doitsujin/dxvk.git
   cd dxvk
   meson setup build-native -Dbuildtype=release -Dprefix="$PWD/../dxvk-native" \
     -Denable_dxgi=false -Denable_d3d8=false -Denable_d3d10=false \
     -Denable_d3d11=false -Dnative_sdl3=enabled -Dnative_sdl2=disabled \
     -Dnative_glfw=disabled
   ninja -C build-native install
   ```

   By default the build looks for the install next to the repository
   (`../tools_ext/dxvk-native`); pass `-DDXVK_NATIVE_DIR=...` otherwise.

2. **Configure and build the lifter**:

   ```sh
   cd "$REPO"
   cmake --preset release -DDXVK_NATIVE_DIR=/path/to/dxvk-native \
     -DDS1_LLVM_ROOT=/usr/lib/llvm/22
   cmake --build --preset release --target ds1_lift
   ```

3. **Symbols** from your copy of the EXE (written to `symbols/`, which
   is ignored by git):

   ```sh
   python3 -I tools/pe_symbols.py "$GAME/DungeonSiege.exe" --out symbols
   mkdir -p /tmp/ds1_ghidra
   bash /usr/share/ghidra/support/analyzeHeadless /tmp/ds1_ghidra ds1 \
     -import "$GAME/DungeonSiege.exe" -scriptPath tools/ghidra \
     -postScript ExportFunctions.java symbols/functions.tsv \
     -postScript ExportJumpTables.java symbols/jumptables.tsv
   ```

   Ghidra's plain auto-analysis is all that is needed: with Ghidra 12.1
   it reproduces the development function and jump-table lists byte for
   byte. (`pe_symbols.py` uses `llvm-undname` from your LLVM install.)

4. **Lift** the game and build it (the lifted C++ goes to `generated/`,
   also ignored):

   ```sh
   out/build/release/tools/lifter/ds1_lift --exe "$GAME/DungeonSiege.exe" \
     --symbols symbols --out generated
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
linked dynamically) and LLVM's demangler (Apache-2.0 with LLVM
exception) come from your system.
