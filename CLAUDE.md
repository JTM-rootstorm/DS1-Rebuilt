# Dungeon Siege (PC, 2002) → native Linux static recomp

Rules only. Status, findings and next steps go in FINDINGS.md; design in
ARCHITECTURE.md. The lifter, x87 model, runtime and HLE live in the shared
x86recomp library (../../x86recomp, its own git repo with its own
CLAUDE.md); this repo holds what is specific to Dungeon Siege.

## Layout
- Project: ~/Documents/Programming/Recomp/Dungeon Siege/DS1
- Library: ~/Documents/Programming/Recomp/x86recomp (added with
  add_subdirectory; X86RECOMP_DIR overrides the path). Library changes
  are committed there, and its rules apply to them.
- Game (GOG, DRM-free; NEVER commit or copy into the repo):
  $HOME/Games/Heroic/Dungeon Siege/  (DungeonSiege.exe, Resources/,
  Maps/, *.dll)
- dxvk source: ../tools_ext/dxvk (tag v3.1.1, do not edit; patches go in
  a separate branch with a note in FINDINGS.md)
- dxvk-native install prefix: ../tools_ext/dxvk-native (d3d9 only, SDL3 WSI)
- Ghidra project: ds1_recomp.gpr / ds1_recomp.rep (gitignored)
- Derived symbol data: symbols/ (gitignored, regenerate, see below)

## Build
- `cmake --preset debug && cmake --build --preset debug`
  (release: `--preset release`). Output in out/build/<preset>/.
- Lifting is a separate step; re-run it after any change to the library's
  tools/lifter, symbols/, lifter.manifest or the EXE, then re-run cmake
  (the generated file list is globbed):
  `out/build/debug/x86recomp/tools/lifter/recomp_lift --exe "$HOME/Games/Heroic/Dungeon Siege/DungeonSiege.exe" --symbols symbols --out generated`
  It only rewrites files whose content changed. generated/lift_report.txt
  lists unimplemented instructions and warnings.
- Inspect one function's decode + C++: `recomp_lift --exe ... --dump <hexva>`.
- Tests: `ctest --test-dir out/build/debug`; configure with
  `-DDS1_GAME_EXE="$HOME/Games/Heroic/Dungeon Siege/DungeonSiege.exe"` to
  include the EXE-derived oracle tests.
- Run: `out/build/debug/ds1 --game-dir "$HOME/Games/Heroic/Dungeon Siege"`
  (DS1_TRACE=1 logs every HLE call).
- Rebuild dxvk-native (only if tools_ext/dxvk changes):
  `ninja -C ../tools_ext/dxvk/build-native install`

## Product constraints
The library's (x86recomp/CLAUDE.md): no emulation or Wine in the product,
64-bit host only, portable to macOS arm64 + Metal, exact x87.

## Rules
- The path contains a space ("Dungeon Siege"): always quote it in shell
  commands, scripts and CMake.
- Guest addresses must stay below 0x80000000 (the EXE is not
  large-address-aware and may compare pointers as signed) until the
  large-address audit in x86recomp's ARCHITECTURE.md is done for DS1;
  even then only behind its config flag.
- Dungeon Siege knowledge (paths, registry, AppModule offsets, game
  patches) goes in src/ here (GameConfig, hooks), never in x86recomp.
- Commits: GPG-signed (global config), no Claude co-author trailers or
  generated-by markers.
- Python scripts that read game files run with `python3 -I`.
- /usr/share/ghidra/support/analyzeHeadless has no exec bit: run it as
  `bash /usr/share/ghidra/support/analyzeHeadless ...`.
- Regenerate symbols after any Ghidra change that matters:
  `python3 -I ../../x86recomp/tools/pe_symbols.py "$HOME/Games/Heroic/Dungeon Siege/DungeonSiege.exe" --out symbols`
  and the headless ExportFunctions.java run (command in its header,
  ../../x86recomp/tools/ghidra).
- No game assets, memory dumps, extracted tank contents or symbol dumps in
  git. Lifted code is generated and gitignored too.
- Every import needs an explicit HLE handler (x86recomp
  src/runtime/hle/); an unhandled one is fatal by design.
- Record findings in FINDINGS.md, not here.
