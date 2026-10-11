# Findings

## Status (2026-10-10)
M0, M1 and M2 done. The recompiled binary runs the CRT, all static
constructors and WinMain's app setup natively: keyboard LL hook, main
window (SDL3, class gpgwndclass_dungeonsiege_exe, 1024x768 WS_POPUP),
registry and INI lookup, its 200 MB commit probe, temp-file checks, and
stops at the first DirectDraw call: ddraw!DirectDrawEnumerateExA from
0065C6FD (renderer device enumeration). Next: M3, DirectDraw/D3D7 on the
d3d9 backend.
- Game-visible facts learned in M2:
  - It reads DungeonSiege.ini from My Documents\Dungeon Siege (absent on
    first run -> defaults), maps under the per-user tree.
  - GlobalMemoryStatus values are compared as signed ints; anything
    >= 2 GiB reads as negative and triggers the "swap file" error.
  - It probes VirtualAlloc(NULL, 200 MB, MEM_COMMIT) (commit without
    reserve) and expects success.
  - LANGUAGE.DLL (localisation) is tried and absent in the GOG install;
    the game continues without it.
  - It resolves dbghelp (FuBi), OpenThread, SHGetFolderPathA/W and
    IsProcessorFeaturePresent dynamically.
- SEH is wired (RaiseException/RtlUnwind walk the guest fs:[0] chain; the
  __except block call in __except_handler3 at 0040A0A3 is a manifest
  `resume` site; 93 functions that write fs:[0] catch GuestResume) but has
  not been exercised by the game yet.
- ds1_lift: 31292 functions (27660 Ghidra/exports + 3632 from code
  immediates and .rdata/.data pointer scans), 1.17M instructions, 98 C++
  files (~125 MB), -O2 build in ~30-40 s on 16 cores. 151 "unimplemented"
  sites are all in data that merely decodes (aaa/aam/far calls) and never
  runs.
- Tests (ctest): lift_semantics (12.4M integer/flag checks, bit-exact vs
  host CPU) and x87 (6.9M cases: bit-exact for everything but the
  transcendentals, which are within 1 ulp). Both pass.
- Runtime: guest main thread runs on the process main thread on a 256 MB
  makecontext stack (SDL/Cocoa need the main thread).
- HLE so far: kernel32 (CRT startup, memory, heaps, TLS, sync, threads,
  files, mappings, locale, time), advapi32 registry (seeded EXE Path),
  version.dll (real VS_VERSION_INFO), ole32 basics, user32 keyboard/
  metrics/wsprintfA.

## x87 model notes
- SoftFloat 3e pinned at 5c06db33 (3e + doc fixes, before BF16, which
  only builds for 8086-SSE). `8086` specialization. Gotchas fixed in
  cmake/softfloat.cmake: upstream C99 `INLINE` emits no out-of-line
  bodies (link errors at -O0) -> `static inline`; softfloat_types.h field
  order depends on LITTLEENDIAN, so the target exports LITTLEENDIAN=1,
  SOFTFLOAT_FAST_INT64 and THREAD_LOCAL publicly.
- Oracle is the host CPU, an AMD Ryzen 7 9800X3D, so where Intel is
  undefined the model follows AMD: FFREE clears C1, FCMOV keeps it,
  PC=01 behaves as 64-bit, FSINCOS takes C1 from the sine. Deliberate
  deviation: FCOMI/FUCOMI clear C1 (Intel docs; AMD keeps it).
  Unmasked underflow with an exact tiny result is not flagged.
- Transcendentals are correctly rounded in a sampled check; the AMD host
  itself was off by up to 0.58 ulp on fptan, so 1-ulp diffs vs host are
  expected (0.4-8.5% of cases).
- Cost at PC=24/chop: fadd ~19 ns, fld/fstp m32 ~10 ns, fsqrt ~24 ns,
  fsin ~210 ns, fsincos ~390 ns. Fine for bring-up; a proven-equivalent
  fast path can come later.

## Lifter notes
- Ghidra's 59 "unresolved" computed jumps are all `jmp [IAT]` thunks (22)
  and vtable/register tail jumps; every real switch table (86) resolves.
- Function pointers stored by code (`mov [esi+0x54], 0x404B60`) are not
  Ghidra functions; the immediate/data pointer scan finds them. Treating
  any such address as an entry is safe: code reached by jumping into an
  entry is tail-called rather than inlined.
- SEH plan (M2): VC6 __except_handler3 / __CxxFrameHandler finish by
  restoring esp/ebp and `jmp`ing to a continuation inside an ancestor
  function. Lifted, that ancestor is further up the host stack, so the
  dispatcher must throw a host exception {target, esp, ebp} for such
  targets and the owning lifted function (matched by its frame ebp) must
  catch it and `goto` the label. Only functions whose scopetables /
  FuncInfo name continuations need the wrapper.

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

## x87 control word (why the FPU must be modelled exactly)
- FUN_004143e6 is the game's FPU setup: fnstcw; `and eax,0xfcff`
  (PC=24-bit); `or eax,0xc00` (RC=chop, round toward zero); fldcw.
  Call sites: 0x414168 (FUN_0041413d), 0x43f2ac (not inside a Ghidra
  function yet), 0x57180f (FUN_005717f6), 0x5f1586 (FUN_005f157a),
  0x64e205 (FUN_0064e0f7). Likely main thread init plus worker thread
  entries; to confirm. So game float maths runs at PC=24, RC=chop.
- VC6 CRT maths helpers (FUN_00635f56, FUN_0063615c and unlabelled code
  around 0x63579c-0x63649c, FUN_006351c5) switch to PC=64 (`or eax,0x300`
  / `0x33f`, `and eax,0xf3ff` = RC nearest) around their work.
- __ftol (0x631810) sets RC=chop temporarily (`or ah,0xc`); same in
  FUN_0040c02f.
- Open: the DDSCL flags passed to IDirectDraw7::SetCooperativeLevel
  (vtable +0x50). DirectDrawCreateEx is called from 0x64efbd and
  0x65ce61 (thunk 0x71eb72); the +0x50 calls near there are
  IDirect3DDevice7::SetRenderState (device ptr at [renderer+0x600]), not
  SetCooperativeLevel. Find it via Ghidra (decompile callers of the
  stored IDirectDraw7 pointer).
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
