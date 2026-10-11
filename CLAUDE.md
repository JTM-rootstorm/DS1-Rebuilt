# Dungeon Siege (PC, 2002) → native Linux static recomp

Rules only. Status, findings and next steps go in FINDINGS.md; design in
ARCHITECTURE.md.

## Layout
- Project: ~/Documents/Programming/Recomp/Dungeon Siege/DS1
- Game (GOG, DRM-free; NEVER commit or copy into the repo):
  /home/mike/Games/Heroic/Dungeon Siege/  (DungeonSiege.exe, Resources/,
  Maps/, *.dll)
- dxvk source: ../tools_ext/dxvk (tag v3.1.1, do not edit; patches go in
  a separate branch with a note in FINDINGS.md)
- dxvk-native install prefix: ../tools_ext/dxvk-native (d3d9 only, SDL3 WSI)
- Ghidra project: ds1_recomp.gpr / ds1_recomp.rep (gitignored)
- Derived symbol data: symbols/ (gitignored, regenerate, see below)

## Build
- `cmake --preset debug && cmake --build --preset debug`
  (release: `--preset release`). Output in out/build/<preset>/.
- Lifting is a separate step; re-run it after any change to tools/lifter,
  symbols/ or the EXE, then re-run cmake (the generated file list is
  globbed):
  `out/build/debug/tools/lifter/ds1_lift --exe "/home/mike/Games/Heroic/Dungeon Siege/DungeonSiege.exe" --symbols symbols --out generated`
  It only rewrites files whose content changed. generated/lift_report.txt
  lists unimplemented instructions and warnings.
- Inspect one function's decode + C++: `ds1_lift --exe ... --dump <hexva>`.
- Run: `out/build/debug/ds1 --game-dir "/home/mike/Games/Heroic/Dungeon Siege"`
  (DS1_TRACE=1 logs every HLE call).
- Rebuild dxvk-native (only if tools_ext/dxvk changes):
  `ninja -C ../tools_ext/dxvk/build-native install`

## Product constraints (see ARCHITECTURE.md)
- No emulation in the shipping binary: no interpreter, JIT, runtime x86
  decoding or slow-path fallback. Unlifted code reached at runtime is a
  fatal error naming the VA; fix it in the lifter/manifest.
- No Wine in the product, ever. Wine running the original EXE is allowed
  only as a dev-time behaviour reference. Unicorn only in tests.
- 64-bit host only; no i386/multilib dependencies.
- Keep everything portable to macOS arm64: in lifted code and the
  runtime, no x86 intrinsics, no inline asm, no `long double`, no host
  FPU/x87 state, no assumption of 4 KiB host pages, no Linux-only
  syscalls/flags outside the platform layer (guard with #ifdef).
- The x87 FPU is modelled exactly (80-bit registers, CW precision and
  rounding, SW condition codes, per-thread state). Never map x87 to
  host double.

## Rules
- The path contains a space ("Dungeon Siege"): always quote it in shell
  commands, scripts and CMake.
- dxvk-native throws in Direct3DCreate9 unless DXVK_WSI_DRIVER is set.
  Host code sets `setenv("DXVK_WSI_DRIVER", "SDL3", 0)` before any D3D9
  call; keep it that way.
- Guest addresses must stay below 0x80000000 (the EXE is not
  large-address-aware and may compare pointers as signed) until the
  large-address audit in ARCHITECTURE.md is done; even then only behind
  its config flag.
- The guest arena base is chosen at startup (preferred 0x1000000000) and
  is not a compile-time constant. Do not hand host pointers to guest
  code; convert with GuestMemory::to_guest.
- Commits: GPG-signed (global config), no Claude co-author trailers or
  generated-by markers.
- Python scripts that read game files run with `python3 -I`.
- /usr/share/ghidra/support/analyzeHeadless has no exec bit: run it as
  `bash /usr/share/ghidra/support/analyzeHeadless ...`.
- Regenerate symbols after any Ghidra change that matters:
  `python3 -I tools/pe_symbols.py "/home/mike/Games/Heroic/Dungeon Siege/DungeonSiege.exe" --out symbols`
  and the headless ExportFunctions.java run (command in its header).
- No game assets, memory dumps, extracted tank contents or symbol dumps in
  git. Lifted code is generated and gitignored too.
- Every import needs an explicit HLE handler (src/runtime/hle/); an
  unhandled one is fatal by design. No catch-all stubs that return 0.
- HLE handlers take arguments with Arg(c, i) and return EAX; declare the
  stdcall argument count correctly in HLE(...) or the guest stack breaks.
- lifted.h semantics changes must keep tests/lift_semantics_test.cpp
  passing (bit-exact against the host CPU).
- Record findings in FINDINGS.md, not here.
