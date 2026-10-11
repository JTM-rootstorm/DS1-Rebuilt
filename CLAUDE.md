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
- Rebuild dxvk-native (only if tools_ext/dxvk changes):
  `ninja -C ../tools_ext/dxvk/build-native install`

## Rules
- The path contains a space ("Dungeon Siege"): always quote it in shell
  commands, scripts and CMake.
- dxvk-native throws in Direct3DCreate9 unless DXVK_WSI_DRIVER is set.
  Host code sets `setenv("DXVK_WSI_DRIVER", "SDL3", 0)` before any D3D9
  call; keep it that way.
- Guest addresses must stay below 0x80000000: the EXE is not
  large-address-aware and may compare pointers as signed.
- The guest arena is reserved at host 0x100000000 (guest VA + base). Do not
  hand host pointers to guest code; convert with GuestMemory::to_guest.
- Python scripts that read game files run with `python3 -I`.
- /usr/share/ghidra/support/analyzeHeadless has no exec bit: run it as
  `bash /usr/share/ghidra/support/analyzeHeadless ...`.
- Regenerate symbols after any Ghidra change that matters:
  `python3 -I tools/pe_symbols.py "/home/mike/Games/Heroic/Dungeon Siege/DungeonSiege.exe" --out symbols`
  and the headless ExportFunctions.java run (command in its header).
- No game assets, memory dumps, extracted tank contents or symbol dumps in
  git. Lifted code is generated and gitignored too.
- Record findings in FINDINGS.md, not here.
