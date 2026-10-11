# Architecture

## Goals and hard constraints
1. Native Linux first: one x86-64 ELF built from DungeonSiege.exe's
   machine code, running against the original game data.
2. No emulation in the shipping binary: no interpreter, no JIT, no
   runtime x86 decoding, no fallback path. Every guest instruction that
   can execute is lifted to C++ ahead of time. An indirect branch to an
   address that was not lifted is a fatal error (reported with its VA so
   it can be added to the manifest), never a slow path.
3. No Wine, in any form, in the product: the Win32/DirectX surface the
   game uses is reimplemented natively (HLE). Wine (with the original
   EXE) is allowed only as a dev-time reference to compare behaviour, and
   is never linked or shipped.
4. 64-bit everywhere on the host: host code, the runtime and every
   library are 64-bit; nothing needs i386 multilib.
5. Portable to macOS on Apple Silicon with a native Metal renderer
   later. Nothing in the lifter output or the runtime may assume an x86
   host, 4 KiB pages, or Linux-only APIs outside a platform layer.

```
DungeonSiege.exe ──Ghidra──> functions.tsv, jump tables ─┐
       │                                                 ▼
       └──────── exports (FuBi) ──────────────>  ds1_lift (x86-32 -> C++)
                                                         │
                                          generated/*.cpp (gitignored)
                                                         │
   ┌──────────────── ds1 (native 64-bit, portable C++) ──┴────────────┐
   │ lifted game code ── guest arena (4 GiB, base chosen at startup)   │
   │        │ calls to import thunks / COM vtables (0x7F000000+)       │
   │        ▼                                                          │
   │ HLE: kernel32 user32 gdi32 advapi32 ole32 mss32 binkw32 ddraw ... │
   │        │          │                 │                 │           │
   │     POSIX       SDL3         RenderDevice        SDL3 audio,      │
   │                (window,     ├ d3d9 (dxvk-native)  ffmpeg          │
   │                 input)      └ SDL_GPU: Vulkan / Metal             │
   └───────────────────────────────────────────────────────────────────┘
```

## What "64-bit" means here
The host process, the runtime and all libraries are 64-bit. The game's
own data layout stays 32-bit: its structures contain 32-bit pointers,
its heap lives in a 4 GiB arena, and the lifted code computes 32-bit
addresses, because that is what the compiled machine code does.
Recompilation keeps the program's semantics; it does not retype its
data. Giving the game 64-bit pointers in its own structures would mean
decompiling every structure and function to source, which is a
different project. The route there, if ever wanted, is incremental:
replace lifted functions with hand-written C++ one subsystem at a time.

The arena is not emulation: it is how the program's address space is
represented. A guest load is one add of a base register plus the load
itself, on both x86-64 and ARM64.

Host libraries (SDL3, dxvk/SDL_GPU, ffmpeg, libc) are ordinary 64-bit
libraries and never see guest addresses. The HLE layer is the only
place where the two worlds meet:
- Guest -> host: a guest pointer argument becomes `base + addr`
  (e.g. CreateFileA's path, a vertex array passed to DrawPrimitive).
  Little-endian on both sides, so data is used in place, no copies.
  Structs that contain pointers are read through guest-layout mirror
  types whose pointer fields are `u32`.
- Host -> guest: host objects are never exposed as 64-bit pointers.
  Kernel objects (files, events, threads) become 32-bit handles indexing
  a host table. COM objects (IDirectDraw7, IDirect3DDevice7, ...) are
  small guest-memory objects whose vtables point at thunk addresses and
  whose payload holds a host table index. Memory the game must read or
  write (Lock() results, MapViewOfFile views, heap blocks) is placed
  inside the arena.

## Decisions

### Guest arena
One 4 GiB reservation; guest VA A lives at host `base + A`. The image is
mapped at its real VAs (0x400000..), so every constant address in the
code is valid unchanged. `base` is chosen at startup (preferred
0x10_0000_0000, which is free on Linux and outside macOS's 4 GiB
__PAGEZERO and main-image range) and read from a global by lifted code.

Win32 memory semantics (4 KiB commit granularity, 64 KiB allocation
granularity, page protections, VirtualQuery) are tracked in a per-guest-
page table. Host protection is only "accessible or not" at host page
granularity (4 KiB on x86 Linux, 16 KiB on Apple Silicon and Asahi), so
nothing depends on host page faults for guest behaviour.

### Address-space headroom: 2 GiB -> ~4 GiB (planned, opt-in)
The EXE is not large-address-aware, so on Windows it never saw an
address at or above 0x80000000, and its code may rely on that. Until
audited, the guest allocator hands out only addresses below 0x80000000
(the thunk region at 0x7F000000 stays there either way). Then:
1. Audit for code that breaks with high addresses: signed compares or
   sign-extending shifts on pointers (`cmp ptr; jl/jg`, `sar`, `cdq`
   on addresses), "negative means error" checks on pointer-or-error
   returns, top-bit tagging of pointers, pointer differences stored in
   signed ints, and address-to-int conversions in hashing/sorting.
   Ghidra pointer typing plus a lifter pass that flags signed
   conditions on values derived from allocator results narrows this
   down.
2. Add a top-down allocation stress mode (like Windows'
   AllocationPreference=MEM_TOP_DOWN test for LAA apps): every
   VirtualAlloc/heap/MapViewOfFile result is placed as high as possible,
   so bugs show up at startup instead of hours into a session.
3. Fix what the audit finds with manifest overrides or host
   replacements, then allow allocations up to 0xFFFE0000 (the top 64 KiB
   stays reserved, as on Windows) behind a config flag. It becomes the
   default only after a full playthrough in stress mode.
Worth it for big mods and long sessions; the game itself uses a few
hundred MB, and Tank archives are mapped views, so this is headroom,
not a bring-up requirement.

### Lifter: x86-32 -> portable C++
No maintained tool lifts Win32 x86 PE to portable C. The code is VC6:
integer + x87 only, ~190 mnemonics, few SEH frames, so a purpose-built
lifter is tractable (rexglue-style: one C++ function per guest function).

- Decoder: Zydis (vendored via CMake FetchContent), build-time only.
- Function list: Ghidra functions.tsv + all FuBi exports + a manifest
  (TOML) for overrides: extra entries, jump tables Ghidra misses,
  no-return functions, functions to replace with host code.
- Output is plain C++20: no x86 intrinsics, no inline asm, no
  `long double`, no reliance on host FPU state.
- Context: `struct Ctx { u32 eax..edi, esp, eflags; X87 fpu; fs base }`,
  one per guest thread. Registers are cached in locals per function and
  flags computed eagerly into locals (clang drops dead ones).
- Calls: direct `call` -> C++ call; the real return address is pushed to
  the guest stack so code that reads it (__alloca_probe, SEH, FuBi)
  still works. `ret N` pops. Tail `jmp` to another entry -> tail call.
- Indirect calls/jumps: dense table indexed by (va - 0x401000), 3.2 M
  entries; addresses in the thunk region go to HLE handlers; anything
  else is fatal (constraint 2).
- SEH (~45 functions): the fs:[0] chain lives in guest memory as on
  Windows. RaiseException walks it and calls the lifted handlers.
  Resuming at a continuation (catch/except target) throws a host
  exception carrying {target va, esp, ebp}, caught by the lifted
  function that owns the target label. Only functions with SEH
  continuations get the wrapper.
- Differential testing (dev-time only, never linked into ds1): run
  single functions lifted and in a reference x86 emulator (Unicorn)
  from the same state and compare registers/memory/FPU. Most valuable
  for x87 and flags.

### x87: full 80-bit model
The game depends on x87 state, so the FPU is modelled exactly, not
mapped to double:
- It calls its own setup routine (0x4143e6, 5 call sites) that sets
  precision control to 24-bit and rounding control to chop (round
  toward zero). All its float maths then runs under PC=24/RC=chop.
- The VC6 CRT maths helpers switch to 64-bit precision (`or eax,0x300`,
  `0x33f`) around their own work and restore afterwards, and __ftol
  temporarily selects chop.
- D3D7 changes the CW itself (below).

Model per thread: 8 registers of 80 bits (sign, 15-bit exponent,
64-bit explicit-integer mantissa), TOP, tag word, control word (PC, RC,
exception masks) and status word (C0-C3, sticky exception flags, stack
fault). Semantics to honour:
- PC rounds the significand of fadd/fsub/fmul/fdiv/fsqrt (and their
  pop/reverse/integer-operand forms) to 24/53/64 bits while the exponent
  keeps the full 15-bit range (so PC=24 does not flush to float range).
  Loads, stores, transcendentals and fprem are unaffected by PC.
- RC applies to every rounding: arithmetic, fst/fstp to m32/m64,
  fist/fistp, frndint.
- fist/fistp out of range stores the integer indefinite (0x80000000 /
  0x8000 / 0x8000000000000000); masked invalid sets IE.
- fcom/fucom/ftst/fxam set C0/C2/C3 for the `fnstsw ax; sahf` idiom
  (~2.5k sites), NaN/unordered included.
- New threads start with the Windows default CW 0x027F (PC=53,
  RC=nearest, all exceptions masked), independent of the creator.

Implementation: Berkeley SoftFloat 3e `extF80` (BSD-3; has
`extF80_roundingPrecision` 32/64/80 and all four rounding modes, and is
portable C, so it works on ARM64 where `long double` is only 64 bits).
SoftFloat has no transcendentals: fsin, fcos, fsincos, fptan, fpatan,
fyl2x, fyl2xp1, f2xm1 and fscale need our own extF80 implementations
(reference: Bochs' x87 code, LGPL, which also models the hardware's
66-bit-pi argument reduction for fsin/fcos). Correctness first;
a fast path (host float/double ops with the host rounding mode set,
used only where it is provably identical) can come later, gated by the
differential tests.

### Graphics: D3D7 HLE over a RenderDevice interface
The game is DirectDraw 7 + Direct3D 7 fixed-function (TnL, texture
stages, lights, fog). Our ddraw/d3d7 HLE exposes guest-visible COM
objects (object + vtable in guest memory, vtable entries are thunk VAs)
and implements DirectDraw (surfaces, flipping chain, Lock/Blt, colour
keys, display modes) itself. Below it sits a small internal
RenderDevice interface with two backends:

1. d3d9 via dxvk-native (Linux bring-up). D3D9 still has the D3D7
   fixed-function pipeline and DXVK implements it, so this backend is
   mostly 1:1 state translation and gets pixels on screen fastest. It
   is also the reference image for backend 2. Same route as GOG's own
   DX1-7 wrapper.
2. SDL_GPU with our own D3D7 fixed-function shader generator: Vulkan on
   Linux and native Metal on macOS (shaders compiled from one source to
   SPIR-V and MSL at build time). This is the long-term backend on both
   platforms. dxvk-native cannot be it: DXVK on macOS would need
   MoltenVK, which lacks features DXVK requires, and would not be
   native Metal anyway.

D3D7 FPU side effect: the HLE must reproduce whatever the real runtime
does to the guest thread's CW, because the game's own float maths runs
on that state. What is known:
- DX7 docs: DDSCL_FPUSETUP = the app keeps the FPU in D3D's preferred
  state (single precision, exceptions masked); DDSCL_FPUPRESERVE = D3D
  saves and restores the CW around each call. Without either, the
  Windows runtime switches the thread to single precision and leaves it
  there (the classic "D3D lowered my precision" effect). To confirm.
- Wine's ddraw does not model that default: it never changes the CW the
  app sees; in FPUPRESERVE mode each device method sets 0x37F and
  restores the old CW afterwards (d3d_device7_fpu_preserve_vtbl). So
  Wine is not a reference for this point.
- Which flags DS passes, and whether its own 0x4143e6 call runs before
  or after device creation, is open (FINDINGS.md). If D3D's change comes
  last, PC=24 with RC=chop from the game's routine survives anyway,
  since D3D only touches precision and masks.

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
- mss32 (Miles, 68 calls): our own mixer on an SDL3 audio stream
  (runtime/audio): linear resampling, loop counts, gain ramps, and the
  "Miles Fast 2D Positional Audio" model (inverse-distance rolloff from
  min_dist, muted past max_dist, stereo pan) that the game selects with
  EAX off. Chosen over OpenAL Soft: exact Miles semantics (byte
  positions, EOS timing) are simpler in our own mixer, and SDL3 is
  already a dependency on every platform. MP3 through libavcodec.
- binkw32: ffmpeg (libavformat Bink demuxer from memory, binkvideo/
  binkaudio decoders, swscale), audio into the Miles mixer.
- dbghelp: UnDecorateSymbolName for FuBi; the rest stubbed.
- wsock32, imm32, version, shfolder: stubs / small shims.

All of it is written against POSIX + SDL3 + the RenderDevice, with
platform specifics (mmap flags, paths, page size) in one place.

### Distribution: users bring their own EXE
The lifter is a permanent build stage, not scaffolding: the game logic
in the port is its output, so it is re-run whenever its semantics, the
manifest or the function list change. It never ships inside the binary.

What can be published (this repo):
- the lifter, its manifest (addresses, overrides, replacement list),
  the runtime, the HLE layer, render backends, tools and docs.

What is never published or committed:
- DungeonSiege.exe, any DLL or data file from the game, generated/
  (lifted C++ is derived from the EXE), symbols/ (exports, function
  lists), the Ghidra project, memory dumps, extracted Tank contents.

Build for anyone else, the same as for us:
1. Own the game (GOG). Point the build at its DungeonSiege.exe.
2. The build checks the EXE against the supported list below and stops
   on a mismatch: every address in the manifest is specific to one build.
3. The lifter runs locally and generates C++; clang compiles it with
   the runtime into the native binary.
4. At run time the binary reads Resources/, Maps/ and the rest from the
   user's own install; it never needs the EXE again.

Supported EXEs:

| Build | Size | SHA-256 |
|---|---|---|
| GOG 1.11.1.1486 (msqa:03.07.0202), linked 2011-01-13 | 3821628 | 41f14b145e030f2decd95e9f434ccd1de0729ba13d1c5628c4bd9536ee938a02 |

Retail CD 1.11 EXEs carry SafeDisc and are not supported. Other builds
need their own manifest.

Licence hygiene for what we publish:
- dxvk (zlib), SDL3 (zlib), SoftFloat 3e (BSD-3), Zydis (MIT), D7VK
  (zlib, reference) are fine to vendor or link. ffmpeg is LGPL (as
  long as it is built without GPL parts): link dynamically.
- Wine and Bochs are LGPL and used as references only: read them to
  learn behaviour, do not copy code from them into this repo.

### Known macOS risk: memory ordering
x86 is TSO; ARM64 is weakly ordered. Lifted code that shares data
between threads without a lock would be correct on x86-64 hosts and
racy on Apple Silicon. Plan: every HLE synchronisation call (critical
sections, events, Interlocked*, waits) is a full barrier, `lock`-prefixed
instructions become seq_cst atomics, and any lock-free hand-off found in
the game's threads (loader, streaming audio) gets explicit barriers via
the manifest. Revisit before the macOS port.

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
  matched against the d3d9 backend. Large-address audit and top-down
  stress mode; ~4 GiB guest address space behind a flag.
- M6: macOS arm64 build: Metal via SDL_GPU, memory-ordering audit.
