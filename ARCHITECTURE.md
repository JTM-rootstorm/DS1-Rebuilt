# Architecture

Goal: a native x86-64 Linux executable built from DungeonSiege.exe's
machine code, running against the original game data, with no Wine.

```
DungeonSiege.exe ──Ghidra──> functions.tsv, jump tables ─┐
       │                                                 ▼
       └──────── exports (FuBi) ──────────────>  ds1_lift (x86-32 -> C++)
                                                         │
                                          generated/*.cpp (gitignored)
                                                         │
   ┌───────────────────── ds1 (native x86-64) ───────────┴────────────┐
   │ lifted game code  ── guest arena (4 GiB @ host 0x100000000)       │
   │        │ calls to import thunks / COM vtables (0x7F000000+)       │
   │        ▼                                                          │
   │ HLE: kernel32 user32 gdi32 advapi32 ole32 mss32 binkw32 ddraw ... │
   │        │            │              │            │                 │
   │      POSIX        SDL3        dxvk-native     OpenAL / ffmpeg     │
   │                 (window,      d3d9 (Vulkan)                       │
   │                  input)                                           │
   └───────────────────────────────────────────────────────────────────┘
```

## Decisions

### 64-bit host, sandboxed 32-bit guest (chosen)
Guest memory is one 4 GiB reservation; guest VA A lives at host
0x100000000 + A. The image is mapped at its real VAs (0x400000..), so
every constant address in the code is valid unchanged.

Rejected: a 32-bit (-m32) host with identity-mapped memory. Simpler
pointers, but needs 32-bit SDL3/Vulkan/dxvk-native, and keeps the port
stuck on i386. Little-endian guest on little-endian host means the
sandbox costs only an add per access (constant base, folds into the
addressing mode).

### dxvk-native d3d9 as the renderer backend (chosen, worth it)
The game is D3D7 fixed-function (TnL, texture stages, lights, fog).
D3D9 still has the full fixed-function pipeline, and DXVK implements it
with a mature FF shader generator, so an HLE D3D7 layer becomes mostly a
1:1 state translation. This is exactly what the GOG wrapper does on
Windows. Writing our own Vulkan/SDL_GPU FF emulation would duplicate
that work. Cost: one shared library (libdxvk_d3d9.so) built from
tools_ext/dxvk, Windows types from dxvk's native headers.

DirectDraw itself (surfaces, flipping chain, Lock/Blt, colour keys,
display modes) is ours to implement on top of d3d9 textures/surfaces and
the SDL3 window. D7VK and Wine's ddraw are the references.

### Own lifter, x86-32 -> C++
No maintained tool lifts Win32 x86 PE to portable C. The code is VC6:
integer + x87 only, ~190 mnemonics, few SEH frames, so a purpose-built
lifter is tractable (rexglue-style: one C++ function per guest function).

- Decoder: Zydis (vendored via CMake FetchContent).
- Function list: Ghidra functions.tsv + all FuBi exports + a manifest
  (TOML) for overrides: extra entries, jump tables Ghidra misses,
  no-return functions, functions to replace with host code.
- Context: `struct Ctx { u32 eax..edi, esp; flags; x87 st[8] as double,
  top, cw/sw; fs base }`. Registers are cached in locals per function,
  flags computed eagerly into locals (clang drops dead ones).
- Memory: `*(T*)(kGuestBase + addr)` with kGuestBase a constant.
- Calls: direct `call` -> C++ call; the real return address is pushed to
  the guest stack so code that reads it (__alloca_probe, SEH, FuBi)
  still works. `ret N` pops. Tail `jmp` to another entry -> tail call.
- Indirect calls/jumps: dense table indexed by (va - 0x401000), 3.2 M
  entries; addresses in the thunk region go to HLE handlers.
- x87: 8-slot stack of double (D3D7 drops the FPU to 24-bit precision
  anyway), control word honoured for rounding in fist/frndint.
- SEH (~45 functions): fs:[0] chain lives in guest memory as on Windows.
  RaiseException walks it and calls the lifted handlers. Resuming at a
  continuation (catch/except target) throws a host exception carrying
  {target va, esp, ebp}, caught by the lifted function that owns the
  target label. Only functions with SEH continuations get the wrapper.
- Differential testing: run single functions in the lifted build and in
  a reference x86 emulator (Unicorn) with the same input state, compare
  registers/memory. Most valuable for x87 and flags.

### HLE surface (from the 381 imports + dynamic loads)
- kernel32: heap/virtual memory in the arena, files with case-insensitive
  path mapping onto the game dir, MapViewOfFile as mmap into the arena
  (Tank files are 300+ MB), threads (one Ctx + guest stack each), events,
  mutexes, critical sections, TLS (TEB at a fixed guest VA, fs:[0x2c]),
  time, locale stubs.
- user32/gdi32: one SDL3 window; SDL events translated to WM_* and fed
  to the guest WndProc through PeekMessage/DispatchMessage. GDI text
  (CreateFont/TextOutW/GetDIBits) via SDL3_ttf only if the game draws
  text through GDI (check with Ghidra). Dialogs stubbed.
- advapi32: registry backed by a small file.
- ole32: CoCreateInstance fails for DirectPlay8 (multiplayer off).
- ddraw/d3d7: guest-visible COM objects (object + vtable in guest memory,
  vtable entries are thunk VAs) backed by host objects on d3d9.
- mss32 (Miles, 68 calls): OpenAL Soft for 2D/3D samples and streams,
  MP3 via a small decoder (dr_mp3/minimp3).
- binkw32: stub first (BinkOpen -> NULL skips videos), later ffmpeg.
- dbghelp: UnDecorateSymbolName for FuBi; the rest stubbed.
- wsock32, imm32, version, shfolder: stubs / small shims.

## Milestones
- M0 (done): survey, Ghidra project, symbols, dxvk-native + smoke test,
  guest arena + PE mapper + import thunks.
- M1: lifter MVP. Lift everything; unknown instructions trap with their
  VA. HLE stubs log every call. Run the CRT entry (0x409dcb) to WinMain.
- M2: Win32 breadth: files, memory, threads, TLS, SEH, registry. Game
  initialises through tank mounting up to DirectDraw creation.
- M3: DirectDraw/D3D7 on dxvk-native + SDL3 window and input. Main
  menu renders.
- M4: Miles audio, Bink, remaining HLE. Single-player playable.
- M5: Fixes and niceties (resolutions, widescreen, frame pacing).
