# Architecture

The generic design (guest arena, lifter, x87 model and runs, gfx::Device
and its backends, HLE, distribution rules, macOS plans) is the shared
library's: see ../../x86recomp/ARCHITECTURE.md. This file keeps what is
specific to Dungeon Siege.

## Goals and hard constraints
1. Native Linux first: one x86-64 ELF built from DungeonSiege.exe's
   machine code, running against the original game data.
2. macOS on Apple Silicon with a native Metal renderer later.
3. The x86recomp constraints: no emulation and no Wine in the product,
   64-bit host only, exact x87.

```
DungeonSiege.exe ──Ghidra──> functions.tsv, jump tables ─┐
       │                                                 ▼
       └──────── exports (FuBi) ──────────────>  recomp_lift (x86recomp)
                                                         │
                                          generated/*.cpp (gitignored)
                                                         │
   ┌──────────────── ds1 (native 64-bit, portable C++) ──┴────────────┐
   │ lifted game code + src/main.cpp (GameConfig, hooks)               │
   │        │                                                          │
   │ x86recomp runtime + HLE: kernel32 user32 gdi32 advapi32 ole32     │
   │   mss32 binkw32 ddraw/d3d7 dbghelp ... on SDL3, gfx::Device       │
   │   (SDL_GPU: Vulkan, Metal; d3d9 via dxvk-native as reference),    │
   │   ffmpeg                                                          │
   └───────────────────────────────────────────────────────────────────┘
```

## Dungeon Siege specifics

### x87 state the game relies on
- It calls its own setup routine (0x4143e6, 5 call sites) that sets
  precision control to 24-bit and rounding control to chop (round
  toward zero). All its float maths then runs under PC=24/RC=chop.
- The VC6 CRT maths helpers switch to 64-bit precision (`or eax,0x300`,
  `0x33f`) around their own work and restore afterwards, and __ftol
  temporarily selects chop.
- ~2.5k `fnstsw ax; sahf` sites depend on C0/C2/C3.
- DS passes DDSCL_FPUSETUP (coop flags 0xC13, FINDINGS.md), so D3D7
  device creation leaves its CW alone.

### HLE surface (from the 381 imports + dynamic loads)
Links recomp_runtime, recomp_mss32, recomp_bink, recomp_d3d7 and
recomp_dbghelp. Game-specific pieces:
- Miles: 68 calls, "Miles Fast 2D Positional Audio" (EAX off).
- dbghelp: UnDecorateSymbolName for FuBi, byte-exact with Microsoft's.
- ole32: CoCreateInstance fails for DirectPlay8 (multiplayer off).
- MapViewOfFile: Tank files are 300+ MB, mapped into the arena.
- GameConfig (src/main.cpp): guest dir C:\Dungeon Siege, registry seed
  of the retail installer, and the AppModule cursor hook.
- Address-space headroom: the game uses a few hundred MB and Tank
  archives are mapped views, so the large-address work is headroom for
  mods and long sessions, not a requirement.

### Supported EXEs
Every address in the manifest is specific to one build.

| Build | Size | SHA-256 |
|---|---|---|
| GOG 1.11.1.1486 (msqa:03.07.0202), linked 2011-01-13 | 3821628 | 41f14b145e030f2decd95e9f434ccd1de0729ba13d1c5628c4bd9536ee938a02 |

Retail CD 1.11 EXEs carry SafeDisc and are not supported. Other builds
need their own manifest.

## Milestones
- M0 (done): survey, Ghidra project, symbols, dxvk-native + smoke test,
  guest arena + PE mapper + import thunks.
- M1: lifter MVP. Lift everything, including the full x87 model; HLE
  stubs log every call. Run the CRT entry (0x409dcb) to WinMain.
- M2: Win32 breadth: files, memory, threads, TLS, SEH, registry. Game
  initialises through tank mounting up to DirectDraw creation.
- M3: DirectDraw/D3D7 on the d3d9 backend + SDL3 window and input.
  Main menu renders.
- M4: Miles audio, Bink, remaining HLE. Single-player playable on Linux.
- M5: SDL_GPU backend (Vulkan) with the fixed-function generator,
  matched against the d3d9 backend (running and the default since
  2026-10-13; see FINDINGS). Large-address audit and top-down
  stress mode; ~4 GiB guest address space behind a flag.
- M6: macOS arm64 build: Metal via SDL_GPU, memory-ordering audit.
