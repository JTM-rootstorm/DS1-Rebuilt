# Findings

## Status (2026-10-10)
Groundwork only. Done:
- Binary survey (below), Ghidra project imported + auto-analysed (65 s).
- symbols/: exports.tsv (3230 demangled FuBi functions), imports.tsv,
  functions.tsv (Ghidra: 27660 functions, 405 multi-chunk, 33 thunks),
  pe_info.txt.
- dxvk-native v3.1.1 built (d3d9 + SDL3) and verified: d3d9_smoke presents
  120 frames on the RTX 4080 (NVIDIA 615.78.8).
- Runtime skeleton: 4 GiB guest arena + PE mapper + import thunks;
  ds1_loader_probe maps the EXE cleanly.

Next: milestone M1 in ARCHITECTURE.md (lifter MVP, reach WinMain).

## The binary
- DungeonSiege.exe v1.11.1.1486 (msqa:03.07.0202), PE32 i386 GUI,
  link timestamp 2011-01-13 (GOG/MS rebuild of the 1.11 patch, no SafeDisc,
  no packer). PDB path C:\temp\Tattoo\_Exe\Retail\DungeonSiege.pdb (not
  shipped). Internal codename "Tattoo"; engine libs under "GPCore" (gpg).
- Compiler: Visual C++ 6.0 (Rich header: cl 12.00.8168 x262 C++ objs,
  8047 x143 C objs, link 6.00.8447). Expect VC6 idioms: _ftol via fldcw,
  __alloca_probe, __EH_prolog, jump tables inside .text.
- Image base 0x400000, NO relocations, NOT large-address-aware.
  .text 0x401000 +0x31eaba, .rdata 0x720000, .data 0x75b000 (+0x503a5,
  raw only 0x42000 -> 0x0e000+ of bss), .tls 0x7ac000, .rsrc 0x7ad000.
  Entry 0x409dcb (CRT WinMainCRTStartup). Stack reserve 1 MiB.
- TLS: template 0x7ac000..0x7acb9c, index at 0x7a6394, no callbacks.
  88 `mov reg, fs:[0x2c]` sites (__declspec(thread) variables).
- Instruction mix (objdump linear sweep, ~1.13M lines): integer + x87 only.
  No SSE/MMX/3DNow! (the 2 movups / odd in/out/daa hits are data decoded as
  code). x87 heavy: 18k fstp, 13.7k fld, ~2.5k fnstsw+sahf compare idiom,
  164 fistp, 51 fldcw, fsin/fcos/fsincos/fpatan/fsqrt present.
- SEH: only ~45 functions install fs:[0] frames. C++ EH is rare.
- 126 indirect `jmp` (switch tables, mostly); Ghidra recovers these.

## FuBi exports (huge free symbol source)
FuBi (Scott Bilas' function binder) exports script-callable C++ functions
from the EXE itself: 3239 exports, 3230 functions in .text, all demangle
with llvm-undname (full class, signature, calling convention). Top classes:
Go 203, GoMind 188, GoDb 112, UIWindow 110, GoInventory 104,
MoodSetting 97, GoAspect 96, ...
- At runtime FuBi parses the EXE export table and needs dbghelp for it:
  string "FuBi: unable to parse export table because no DBGHELP.DLL
  support, app must fatal". FUN_0041ff0a is the dbghelp loader
  (LoadLibrary DBGHELP.DLL, GetProcAddress SymInitialize ...
  UnDecorateSymbolName into a wrapper object, pointer at +0x4c); called
  from FUN_00421671 and FUN_00452e5e. Headers must be mapped at 0x400000
  (the PE mapper does this). The HLE must provide
  UnDecorateSymbolName with MSVC-compatible output; LLVM's
  microsoftDemangle is a candidate but its text formatting differs from
  dbghelp's, so FuBi's parser must be checked against it.
- Every export is an indirect-call target (FuBi calls through addresses);
  all must be lifted as entry points.

## Imports (381 slots, 11 DLLs)
KERNEL32 165, USER32 94, mss32 68 (Miles, `_AIL_*@N` stdcall), GDI32 20,
binkw32 10, ADVAPI32 6 (registry), ole32 6 (CoCreateInstance etc.),
WSOCK32 6 (by ordinal), VERSION 3, DDRAW 2 (DirectDrawEnumerateExA,
DirectDrawCreateEx), WINMM 1 (timeGetTime). Full list: symbols/imports.tsv.

Loaded dynamically (strings): DBGHELP.DLL (Sym*, MiniDumpWriteDump,
UnDecorateSymbolName), imm32 (Imm* IME API), SHFOLDER.DLL
(SHGetFolderPathA/W), LANGUAGE.DLL, LTDLL.DLL, GPLZO.DLL, gundll.dll,
updateclient(dll).dll. *.dsdll plugin search via %dsdll_path%.

COM GUIDs present: CLSID_DirectPlay8Client/Server/Address (multiplayer
only; Zone.com is dead, so fail CoCreateInstance), IID_IDirectDraw7,
IID_IDirect3D7, IID_IDirect3DHALDevice. No DirectInput (input is Win32
messages / GetAsyncKeyState), no DirectSound (Miles owns audio).

## Graphics
- DirectDraw 7 + Direct3D 7 (DirectDrawCreateEx -> IDirectDraw7 ->
  IDirect3D7 -> IDirect3DDevice7). DungeonSiege.ini picks
  "Primary Display Driver - Hardware TnL", 1024x768x32.
- GOG ships ddraw.dll = "GOG.com DirectX 1-7 wrapper" (1.5 MB) which
  imports d3d9.dll: GOG already runs this game as D3D7-on-D3D9. That is
  the same route we take natively.
- D7VK (WinterSnowfall, zlib) is a D3D7-on-DXVK-d3d9 layer but does NOT
  implement DirectDraw (it proxies to a real ddraw.dll), so it cannot be
  dropped in natively. Its D3D7->D3D9 state/caps mapping is a good
  reference: https://github.com/WinterSnowfall/d7vk
- dxvk-native headers ship d3d.h/d3dtypes.h/d3dcaps.h (D3D7 types) but no
  ddraw.h.

## Assets
Resources/*.dsres and Maps/*.dsmap are Tank archives (~1.1 GB). Engine
code: TankFileMgr.cpp / TankMgr.cpp; options tank_no_valloc,
tank_no_paging suggest memory-mapped access (MapViewOfFile on 300+ MB
files: the HLE must map views into the guest arena, not copy).
Format reference: https://github.com/glampert/reverse-engineering-dungeon-siege

## Environment notes
- Installed app-emulation/dxvk 3.1.1 is the Wine/PE build (crossdev-mingw);
  not usable natively. dxvk-native is built separately in tools_ext.
- Available: SDL3 3.4.16, SDL3_ttf, OpenAL Soft 1.24.3, ffmpeg libavcodec
  62 (has Bink decoder), Vulkan 1.4.357, clang 22, gcc 16, meson, ninja,
  Ghidra 12.1.3. Not installed: Zydis, Unicorn (vendor via CMake when the
  lifter needs them).
