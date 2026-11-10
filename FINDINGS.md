# Findings

## CRT (2026-11-07, branch native-crt; shared since 2026-11-08)
First step from lifted to native source: the statically linked VC6 CRT.
- Where it lives (2026-11-08): the native functions are SwARc's
  (recomp_msvc_crt, platforms/win32/docs/msvc_crt.md), shared by every
  game that links an MSVC CRT statically; lifter.manifest binds the EXE's
  copies with `native <va> msvc_crt::<name>` (the locale ones
  msvc_crt::vc6::<name>, their globals in src/crt_globals.h). The oracle
  is SwARc's too (ctest msvc_crt_oracle, with DS1_GAME_EXE; the game's
  stubs in tests/crt_stubs.cpp and tests/crt_stubs.manifest). What
  follows was found and measured while they were src/crt.cpp here.
- Identification: byte signatures of VC6's static runtime libraries name
  802 functions (CRT, C++ EH runtime, a few Dinkumware STL helpers),
  each checked here against the disassembly. The CRT sits in
  0x409CA0-0x411C64, 0x631420-0x637950 and 0x71EA90-0x71EB75.
  Signature names to correct: 0x40C7E0/0x40C7F0 are strcpy/strcat
  (VC6's strcat.asm; named _mbscpy/_mbscat), and 0x40D130, named memcpy
  like 0x40C930, is memmove (VC6 builds both from one source).
- No malloc, free, new or delete among them: the game's allocator is its
  own (VirtualAlloc, 10 call sites); the CRT's heap only serves the CRT.
- Most string and memory calls were inlined by the compiler (rep movs,
  repne scasb), so the out-of-line functions have few call sites. The
  most called: atexit (11208, static initialisers), std::_Lockit (601),
  _ftol (331), wcslen (187), _strcmpi (169), floor (103), memcpy (83).
- Native now (28): memset, memcpy, memmove, memchr,
  strlen, strcpy, strcat, strncpy, strncat, strcmp, strncmp, strchr,
  strrchr, strstr, strcspn, strpbrk, wcslen, wcscpy, wcsncpy, wcschr,
  wcsrchr, wcscspn, _allmul, _aulldiv, _aullrem, _allshl, _alloca_probe,
  _ftol. _ftol uses the x87 model's fistp under RC=chop, as the
  original does (NaN, out-of-range and flags as the hardware). A zero
  divisor in _aulldiv/_aullrem hands over to the original, whose div
  raises it. A fault on an uncommitted guest page from the native code
  goes to the game's filters as from lifted code (dispatch.cpp's handler
  takes faults in guest memory from any non-HLE host code).
- Oracle (ctest crt_oracle then, msvc_crt_oracle now) runs
  each original, lifted with --only, and the native function from the same
  memory and registers on random arguments, every alignment, bytes and
  units above 0x7F, overlapping memcpy/memmove, 64-bit values at the
  helpers' shortcuts, _alloca_probe sizes up to 256 KiB, and _ftol on
  every class of ST0 under every PC/RC. Compared: EAX, EDX for 64-bit
  results, ESP, EBX, ESI, EDI, EBP, ECX for _alloca_probe, every data
  byte, the stack at and above ESP, and the x87 state: 5399722 calls,
  0 mismatches. Not compared: strcpy/strcat between overlapping strings
  (undefined; the originals copy forwards a dword at a time). Mutations
  caught: strncmp's sign, strncpy's padding, _alloca_probe's saved ECX,
  _ftol's chop, _allshl's 64, wcschr's NUL. Two more (wcsncpy with one
  unit left at the NUL, strncat with n = 0) turned out not to change any
  result.
- Run: a scripted new game (isolated profile) reaches the farm as before;
  ctest 22/22.
- Locale: the game calls setlocale(LC_CTYPE, ".<code page>") at startup
  (0x421E0C), so _strcmpi, _strnicmp, tolower, toupper, _strlwr and
  _strupr always take the CRT's locale path (_pctype, LCMapStringA), not
  the ASCII one. Its globals: __lc_handle[LC_CTYPE] 0x79D260 (0: the C
  locale), __lc_codepage 0x79D270, __mb_cur_max 0x75D808, _pctype
  0x75D814, __setlc_active 0x7A7DD8, __unguarded_readlc_active
  0x7A7DDC.
- Native too (13 more): tolower, toupper, _strcmpi, _strnicmp,
  _memicmp, _strlwr, _strupr, isalpha, isdigit, isspace, isalnum,
  iscntrl, _isctype. Classes come from the guest's ctype table, read
  where the originals read it (any int; -1..255 for _isctype); 'A'-'Z'
  and 'a'-'z' map to each other, as LCMapString does in every code page.
  The originals' work, run from the start, for: another character the
  table marks for mapping, a string with a byte over 0x7F in
  _strlwr/_strupr (LCMapString maps the whole string; Windows-1252 has
  letters at 0x8A-0x9F too), a multibyte code page, values from 256 up.
  Kept from the originals: tolower/toupper leave their result in the
  argument's stack slot in the locale path; at a NUL, _strnicmp compares
  the bytes themselves; _strcmpi and _memicmp compare lowered bytes,
  _strnicmp lowered ints. Not kept: the locale lock (setlocale runs once,
  at startup, before other threads), and _strlwr/_strupr's buffer from
  the game's allocator (0x403BB0/0x403BA0; the original maps into it and
  copies back).
  - crt_oracle covers them in the C locale, a code page locale as the
    game's setlocale leaves it (handle and code page set, a table with
    Latin-1 and 1252's C1-range letters) and with __mb_cur_max 2, on
    strings of mixed-case ASCII with Latin-1/1252 bytes in a quarter,
    compared against case-flipped, changed or cut variants of
    themselves. The originals' kernel32 calls (LCMapStringA/W,
    Multi/WideChar conversions, GetStringTypeA/W, Interlocked*) are stubs
    that map like SwARc's HLE; the allocator and _lock are stubs
    (tests/crt_stubs.manifest). 6959418 calls in all, 0 mismatches.
    Mutations caught: the result in the argument's slot, the delta, the
    signed byte compare, the NUL rule, the 0x80 hand-over of _strlwr
    (only once the stub had 1252's C1 letters), isalpha's mask, the
    letter range, the multibyte hand-over.
  - Run: the scripted new game reaches the farm, the log as before (but
    for the frame the intro is skipped at); ctest 22/22.
- Left for later in this group: _wcsicmp/_wcsnicmp (16 + 4 calls). Their
  towlower (0x634456) returns 16 bits in AX over whatever EAX held, and
  they store whole registers into their argument slots.
- Native too (2026-11-08, SwARc msvc_crt::vc6): atoi 0x4118DC (21 calls;
  it calls atol), atol 0x411851 (3), strtol 0x40D465 (1; strtoxl
  0x40D47C), floor 0x6323E1 (103), ceil 0x631CB5 (14).
  - atol/strtol classify with _pctype (or _isctype when __mb_cur_max >
    1, same answer for a byte), so code page "digits" count. strtol hands
    overflow (errno through _errno 0x40E228, TLS) and table letters over
    0x7F (signed char to toupper) to the original.
  - floor/ceil switch to the control words at 0x791E00 (0x173F) and
    0x791DF0 (0x1B3F) with _ctrlfp 0x634CF6, frndint in _frnd 0x634B83,
    then compare with the argument; the natives take the same steps on
    the x87 model. EAX is _ctrlfp's return, 0x177F/0x1B7F (fnstcw reads
    bit 6 set). NaN/Inf go through _sptype 0x634BBE to _except1 0x63451F
    or _handle_qnan1 0x6344CB; an unmasked exception reaches
    RaiseException (0x6347CC); those go to the original.
  - msvc_crt_oracle (with _errno stubbed): 8119148 calls, 0 mismatches,
    the number parsers in all three locale states, floor/ceil on every
    class of argument, PC/RC and stack depth. Mutations caught: atol's
    sign, strtol's end pointer, digit range, 0x prefix and overflow
    hand-over, floor's compare and precision control.
  - Run: the scripted new game (out/runs/newgame_1080.txt plus Escape)
    reaches the farm; the log as before (but for Bink closing the intro a
    frame earlier); ctest 22/22.
- Native too (2026-11-08, SwARc msvc_crt::vc6): the intrinsics _CIpow
  0x6324B0 (9 calls) and _CIfmod 0x6326DA (5).
  - _CIpow stores x and y as doubles, sets (cw & 0x300) | 0x7F through
    0x6351C5 unless cw is 0x27F, loads x back (0x6351F5), fyl2x, 2^x in
    0x6351B0, a negative x through the integer check 0x6326A5 (times 0.5
    at 0x791E10 for odd/even); errors through 0x63254A (matherr, name
    "pow" at 0x791E18).
  - _CIfmod is _cintrindisp2 0x6354E0 with the table at 0x791E20: cw
    0x133F, _trandisp2 0x634FE7 classifies with fxam (class table
    0x79203C; a denormal counts as zero, so fmod(x, denormal) is a domain
    error), the core 0x6326E4 loops fprem (_adj_fprem 0x63615C when
    _adjust_fdiv is 1), the exit 0x6355AA.
  - The switches (src/crt_globals.h): _fastflag 0x79D050 (bss, never
    written: 0), _adjust_fdiv 0x79D054 (set at startup from
    IsProcessorFeaturePresent(0), 0x40A300), _matherr_flag 0x7921D0
    (9876: the CRT's own _matherr).
  - msvc_crt_oracle: 8719148 calls, 0 mismatches (pow 43.5k and fmod
    78.7k on the native path, the rest special cases handed over). It
    lifts the fmod table's targets (ENTRIES in CMakeLists.txt), which
    --only does not find. Mutations caught: the odd-power negation, both
    EAX forms, the result stores, the integer check's pops, the pow
    control word, the register y, the matherr stores, fmod's EAX and
    _fastflag.
  - Run: the scripted new game reaches the farm, the log as before (but
    for the frame the intro is skipped at); ctest 27/27 (gfx_program
    skipped: no GPU device in the capped scope).
- Native too (2026-11-08, SwARc msvc_crt::vc6): the format engines
  _output 0x40B596 and _woutput 0x6330D6, under sprintf 0x40A127,
  _snprintf 0x631840, _vsnprintf 0x6318CE and _vsnwprintf 0x6317A1, for
  their string streams (_IOSTRG); a real FILE, a non-ASCII character in
  the code-page locale the game sets, or a mid-format failure the native
  cannot undo hands over to the lifted original.
  - The state machine's classes and transitions are __lookuptable
    0x720A98; %f/%e/%g go through _cfltcvt_tab 0x75D5E0 (_cfltcvt
    0x40A6D9, also in _cldcvt's slot; _cropzeros 0x40A383, _fassign
    0x40A3E9, _forcdecpt 0x40A329, _positive 0x40A3D1), the decimal point is __decimal_point
    0x75D80C (src/crt_globals.h). The float chain is $I10_OUTPUT over a
    12-byte long double (__multtenpow12, __ld12mul), 17 significant
    digits, then _fptostr and _cftoe/_cftof/_cftog.
  - msvc_crt_oracle: 9319148 calls, 0 mismatches (narrow 265k native /
    35k handed over, wide 278k / 22k). Its lift takes _cfltcvt_tab's
    targets (ENTRIES). Mutations caught: every flag, width/precision and
    `*` path, the integer radix/sign/prefix code, the float formats and
    rounding, the INF/NaN strings, the stream counts and the error
    returns; survivors are unreachable (the ld12 multiply's exact tie and
    sticky bit) or masked by the oracle's 1:1 code-page stubs.
  - Run: the scripted new game reaches the farm, text and menus as
    before, the log as before (but for the frame the intro is skipped
    at); ctest 28/28.
- Native too (2026-11-09, SwARc msvc_crt::vc6): sscanf's engine _input
  0x6338BA (sscanf 0x63189A, 24 callers: version strings, "%f , %f , %f",
  "%x %x %x %x", "%dx%d", sets like "%50s[abcdef...]"), _fltin2 0x631DC8
  (atof's core; also called by game code at 0x426C53, a strtod-like
  wrapper testing its flags 0x240) and atof 0x631E47 (5 callers).
  - _input reads floats through _fassign 0x40A3E9 (_cfltcvt_tab[2]):
    _atodbl 0x40C5F9 / _atoflt 0x40C626, __strgtold12 0x40EDE9 (__mtold12
    0x40ED22, __multtenpow12 0x411151 with its tables at 0x7608D0 and
    0x760A30, 29 entries each up to 10^4096), _ld12tod 0x40C5CD /
    _ld12tof 0x40C5E3 through _ld12cvt 0x40C461 (descriptors 0x75DA20,
    0x75DA38; _RoundMan 0x40C306, _ZeroTail 0x40C267, _IncMan 0x40C2B0,
    _ShrMan 0x40C3D4). %s and %c take their sets from 0x791E90
    (" \t-\r]") and 0x791E98 ("]"). _inc 0x634316, _un_inc 0x634330
    (_ungetc_lk 0x63718B), _whiteout 0x634347, _hextodec 0x6342DF.
  - msvc_crt_oracle: 11179148 calls, 0 mismatches (_input 560k native,
    41k handed over, mostly the multibyte locale; _fltin2 and atof all
    native outside it). Mutations: 82 of 93 caught. Not caught: five
    equivalent (three rewrites of the same test, an overflow bound past
    where the value overflows anyway, the sub-denormal bound whose bits
    fall out of the double either way), two unreachable (_ungetc_lk at
    _base after a read, a negative hex digit), one line removed as dead
    (the slot write a ']' always overwrites), one masked by the oracle's
    1:1 code page stubs (mbtowc), and two that show only within a unit
    of the 80-bit value of a rounding tie (the 24th digit's own
    rounding). The originals run a %[ set to its ']'
    past the format's end, and __mtold12 loops forever on digits summing
    to zero (table digits are signed bytes).
  - Run: the scripted new game reaches the farm, the log as before (but
    for the frame the intro is skipped at); ctest 29/29.
- Next: the STL (below).

## STL (2026-11-09, branch view-distance)
Identification before any native code: which template instantiations
the game has and which member each function is. tools/stl_names.py does
it from symbols/, generated/table.cpp and llvm-objdump (2949 functions);
tools/ghidra/ApplyNames.java puts the names and the evidence (plate
comment) into the Ghidra project, and symbols/functions.tsv was exported
again from it (only the names column changed; the lift differs only in
the comments naming the functions). Three <stdexcept> functions reached
only through vtables are not in Ghidra's list: lifter.manifest names
them.
- The library is Dinkumware's for VC6, but its <deque> has the later
  layout (_Map, _Mapsize, _Myoff, _Mysize, not VC6's iterator pair) and
  <xtree> a hint insert. Template code is compiled once per type, and the
  linker folds identical bodies: list::_Buynode is shared by every list
  with the same node size.
- Library code the signatures name: std::_Lockit 0x637710 (a critical
  section 0x7A63A0 set up on first use, its state at 0x7A63B8 through
  InterlockedExchange: 0 none, 1 initialising (Sleep(1) and retry), 2
  ready, 3 cleaned up), ~_Lockit 0x6377AC, std::_CleanUp 0x63778E (an
  atexit), std::_Xlen 0x6377C1 ("string too long"), std::_Xran 0x63781B
  ("invalid string position"; the signatures call it _Xlen too),
  std::string::_Tidy 0x63790B and the locale facets.
- std::map/std::set (<xtree>): 195 instantiations in game code, 2715
  functions. Each has its own static _Nil and _Nilrefs (not always
  adjacent); _Tree::_Init (137 bytes, the only one that tests _Nil under
  _Lockit and allocates it with the game's operator new 0x403B80) gives
  both, and every function reading them belongs to that instantiation.
  Members by what they call: _Init, ~_Tree, _Insert, insert(value),
  insert(iterator,value) (once), erase(iterator),
  erase(iterator,iterator), erase(key), _Erase, _Copy, _Copy(tree), the
  copy constructor, _Lrotate, _Rrotate, iterator::_Inc/_Dec, _Lbound,
  _Ubound, clear, find, lower_bound, upper_bound, equal_range. Named
  std::_Tree@<_Nil>::<member>.
  - Object: comparator byte +0, allocator byte +1, _Head +4, _Multi +8
    (multimap/multiset), _Size +0xC. Node: _Left +0, _Parent +4, _Right
    +8, value +0xC, colour last (0 red, 1 black); node sizes 0x14 to 0x78
    (most 0x18/0x1C).
  - Keys, from the compare in _Lbound/_Ubound (or insert): 99 uint32
    (pointers among them), 10 int32, 3 uint32+uint16, 1 uint16, 2
    float, 1 double, 1 char* through an inline strcmp, 74 through a
    call: 35 the game's string class (its char* in the first word, null
    meaning "" at 0x721414) case-insensitive (0x42BC72: _strcmpi, or
    _strnicmp for a length), 19 that string against a char* (0x698527,
    the same), 4 case-sensitive (0x4253EE), the rest one or two each. 4
    instantiations are never inserted into, so nothing compares their
    keys.
  - The value's construction, copy and destruction are inlined into
    _Insert, erase(iterator), _Erase and _Copy, so those differ between
    instantiations with the same layout; rotations, iterator steps,
    _Init and ~_Tree differ only in their addresses and offsets.
- std::vector: 63 instantiations by their growing insert(P,n,x) (it
  compares _End - _Last with n, else allocates size() + max(size(), n),
  copies, fills, frees), one insert(P,first,last) (vector<char>), 32
  insert(P,x) and 55 push_back above them. Object: allocator +0, _First
  +4, _Last +8, _End +0xC. Element sizes 1 to 0x1AC (most 8 to 0x18).
  Named std::vector@elem<size>@<insert(P,n,x)>::<member>.
- std::list: 17 _Buynode (one per node size after folding, 0xC to 0x90),
  6 list() and 29 insert(iterator,value) calling them. Object:
  allocator +0, _Head +4, _Size +8; node _Next +0, _Prev +4, value +8.
  Named std::list@node<size>::<member>.
- std::deque: one, of 0xA0-byte elements, 25 to a 0xFA0-byte block
  (its copy constructor 0x5FEC52, assignment 0x5FEB55): _Xlen 0x5FDD4E
  ("deque<T> too long"), _Growmap 0x5FD906, push_back 0x5FD336, back
  0x5FD2EE, operator[] 0x5FD2CC, element access 0x5FD4C1, pop_front
  0x5FD30D, pop_back 0x5FD8C4, _Tidy 0x5FD4A8, operator= 0x5FD228,
  erase(first,last) 0x5FD3FF, std::copy/copy_backward over its iterators
  0x5FE35A/0x5FE317. Object: _Map +4, _Mapsize +8, _Myoff +0xC, _Mysize
  +0x10.
- std::string (reference-counted: a count byte before the text, 0 or
  255 unshared) is used only by <stdexcept>: _Tidy 0x5FDFDC,
  assign(const char*, n) 0x5FE017, assign(const string&, pos, n)
  0x5FDEFA, erase 0x5FE056, _Grow 0x5FE0BD, _Copy 0x5FE142, _Split
  0x5FE231; logic_error 0x5FDDA2 (from a string), 0x5FDE61 (copy),
  ~logic_error 0x5FDE17, what 0x5FDDED, _Doraise 0x5FDDFA; length_error
  copy 0x5FDEE2, _Doraise 0x5FDEA2.
- Not named: the vector and list members that neither grow nor
  allocate (erase, clear, operator=, _Destroy, ~vector/~list where not
  inlined, splice, sort, remove): nothing ties them to a container, and
  hand-written linked lists or arrays in the game would look the same. Map vs set is not
  told apart (the key extractor is inlined; the value size only bounds
  it), nor the element types.

## macOS arm64 (2026-10-13, branch macos-metal)
Built and run on an M3 Pro (macOS, Homebrew SDL3/ffmpeg/llvm@22) with
SwARc's SDL_GPU backend on native Metal (no dxvk). Symbols from a
fresh Ghidra 12.1.4 auto-analysis lift to the same 31292 functions.
Verified by scripted runs (DS1_TEST_INPUT, DS1_SHOT_EVERY): logos and the
Bink intro, main menu, Single Player, hero creation (the skinned, lit
hero), difficulty, loading, and the farm in game (terrain, animated
multitexture water, the waterfall, fog, smoke, the HUD, the pause menu),
with CoreAudio sound and voices. No graphics warnings in the log. Not yet played by
hand; not yet compared frame by frame against the d3d9 backend.

What the port needed from the game's side:
- Tank demand paging on 16 KiB host pages: committing one 4 KiB guest
  page made its whole host page accessible, so the filter (0064AB4C) was
  never asked for the neighbours and the index read as zeros; ReadFile
  of Objects.dsres failed its CRC (error 0x17, "Read error! Resource file
  is either corrupt"). SwARc now faults in the rest of the host page
  after the filter continues.
- "Read error!" with last error 0x17 is the tank index CRC check
  (FUN_00649eba -> CRC-32 at 0041AEA9 over the index, compared with the
  value at header+0x50). A copy of Voices.dsres whose index was all
  zeros (an incomplete copy) failed it on every host; checking
  zlib.crc32(index) against header+0x50 for each Resources/*.dsres tells
  a damaged install from a runtime bug.

## Software cursor on the GPU (2026-10-14, src/cursor.cpp)
At 5120x2880 (M3 Pro, desktop fullscreen) gameplay at the farm ran at
60 fps without focus but ~50 (steady 20 ms) with it: with focus the game
draws its software cursor. 0x656E02(false), on the main thread before
present, saves the back buffer under the cursor (Blt to a system-memory
surface), blends the image into a copy on the CPU (0x65780F) and Blts it
back. The save is a readback, and SwARc's SDL_GPU device submits a
frame's draws only at a readback or present, so the main thread waited
for the frame's whole GPU work (~11.7 ms) after recording it (~8 ms):
CPU and GPU one after the other.
- Splitting the render pass earlier in SwARc was measured and
  rejected: the GPU starts sooner but loses hidden-surface removal across
  the split, so the frame's fragment work grows from ~11 to ~16 ms
  (SwARc ARCHITECTURE.md, Graphics).
- Now (cursor.cpp, replacing 0x656E02, its Blt helper 0x6576EA and the
  blend 0x65780F): on the main-thread path the save is an asynchronous
  readback (gfx StartReadBack), the copy and the CPU blend are skipped,
  and the image is blended into the back buffer on the GPU (BlitBlend:
  source over, straight alpha; the game's d + ((s - d) * a >> 8) differs
  by at most 2 levels, measured on menu frames). The foreground-window
  check of 0x6576EA is kept.
- The game's cursor thread (0x656862) calls 0x656E02(true) every ~30 ms
  to move the cursor on the primary between frames (and to show the
  busy cursor, +0x158, after 0.5 s without a main-thread draw). It reads
  the primary back while holding the gfx device, which also submits the
  main thread's half-recorded pass: with the mouse moving, frames took
  7.5 or 31 ms (58 fps). It now runs only when the main thread has not
  drawn the cursor for 100 ms (loading, stalls), and its first Blt
  finishes the pending save into the save surface, which it restores
  from.
- Measured (scripted farm run, DS1_BACKGROUND=1 DS1_TEST_FOCUS=2,4,
  no screenshots): 60.0 fps, p99 17.1 ms, with the pointer still or
  moving every 50 ms; DS1_SOFT_CURSOR=1 (the game's blending, kept for
  comparison) 50 fps. The asynchronous save matched a synchronous
  readback byte for byte in every case it was used. Frame dumps: the
  cursor and the HUD as before.

## Widescreen (2026-10-14)
Game patches in src/ (`replace` in lifter.manifest). Not yet played by
hand.

- Window (src/window.cpp). In fullscreen (forced in this build) the
  game creates a WS_POPUP covering the screen rect from 0x43A408
  (GetSystemMetrics SM_CXSCREEN/SM_CYSCREEN; 0x43A44B is the virtual
  screen, falling back to it) and keeps that rect in its AppModule
  (+0xA0..+0xAC): every WM_ACTIVATEAPP (window proc 0x414B38) does
  MoveWindow to it plus ShowWindow(SW_RESTORE), and WM_GETMINMAXINFO
  makes its size the maximum and the tracking size. The ini's
  width/height only size the window in windowed mode (default
  0x400x0x300). The game asked before SwARc had the host display
  up and got the runtime's 1024x768 placeholder, so its window was
  pinned at 1024x768 whatever the mode. Both rects are now the host
  desktop (DesktopSize), as on Windows: CreateWindowExA(TOPMOST,
  WS_POPUP|WS_VISIBLE, 0,0 3840x2160) on a 3840x2160 panel at 1.5x
  desktop scale (SwARc 0cadefc: games see pixels, host windows are
  sized in points; before it, 2560x1440). With <ID>_FULLSCREEN=window
  (scripted runs) the host window stays desktop-sized instead of
  following the mode; not looked into. Scripted input positions are host
  window points: the 800x600 menu is then 1920x1440 points at x 320.
- Resolutions (src/resolutions.cpp). The options menu (0x4B49B7) lists
  the display modes whose "WxHxBPP" appears in the gas block
  config:options:video:allowed_res (read with 0x496307 each time the menu
  opens); the game also checks modes against config/system_detail, one
  tier per video memory size (8, 12, 16, 32, 64 MB, two detail levels
  each; record filed by 0x51E28C: +0 memory, +8 detail, +0xC shadow
  texture size, +0x10 vector of {w, h, max_back_buffers}). Every list
  stopped at 1024x768. Both now also have the desktop size, 1152x864,
  1280x960, 1280x1024, 1600x1200 and 1280x720 to 3840x2160 (32 bpp in
  allowed_res; the system_detail tiers of 32 MB and up, with their
  largest entry's back buffer count). Checked: the Video tab's list
  (logged entries) ran 640x480 to 2560x1440 while the runtime reported
  the scaled desktop in points; with pixels it should reach 3840x2160.
  Not yet checked: picking one there and applying it.
- Hor+ (src/fov.cpp). The camera FOV (SiegeCamera +0x124, 60 degrees,
  radians) is horizontal: Rapi 0x6539DE builds x scale cot(fov/2), y
  scale that times the viewport aspect (Hor-). Three functions read it:
  the camera update 0x66192A (culling frustum 0x6799C1 and projection
  0x653D06), the drag-selection volume 0x672701 (every frame: the
  hover pick too) and the screen-to-ray 0x6729D3. Around each the member
  holds fov' = 2 atan(tan(fov/2) * A * 3/4) for A > 4/3, restored after
  (saves and scripts see the original). At 1920x1080 the projection
  went from 1.732 x 3.079 to 1.299 x 2.309 (4:3's vertical); more ground
  at the sides, nothing culled at the edges. 5:4 and 4:3 unchanged.
  Click-to-move and drag-select not yet tried by hand.
- World frustums (src/world_frustum.cpp, 2026-11-08). Nodes and objects
  load only inside axis-aligned boxes around their owners, sized in one
  place, SiegeFrustum::SetDimensions 0x6804BA (__thiscall width, height:
  half-extents in metres), from WorldMap::RCSet's world_frustum_radius /
  world_frustum_height (+0x34/+0x38 of the WorldMap at [0x7A0634];
  defaults 45 / 100000) in Player::RCSetInitialFrustum 0x560591 and the
  server's per-Go frustum 0x57BE06, and from the mood's frustum_width /
  frustum_height as ManageMoods blends them (0x590A87). With Hor+ the
  screen's sides reached past the 45 m box: in the opening cutscene at
  1920x1080 the clear colour (99,99,99) showed beyond the mill where 4:3
  showed trees. The width is now scaled by A * 3/4 for aspect A > 4/3 (60
  m at 16:9), A the wider of the desktop's and Rapi's: the player's
  frustum is made while the front end is still at 800x600. A 3x box (135
  m at 16:9) filled the rest with terrain that was fully fogged and
  slowed the game. The far clip is Rapi 0x652F96: the
  fog's far distance times Rapi+0xE94 while fog is on (+0xE84; mood
  fog_far_dist, about 33 m in the cutscene, linear vertex fog
  D3DRENDERSTATE_FOGVERTEXMODE 3, black), else Rapi+0xE90 (75); doubling
  it changed no frame. The grey beside the ruins at the start of the
  cutscene is the edge of that set: no wider box or farther clip draws
  anything there (4:3 shows its corners too).
- Automated runs: DS1_DESKTOP=WxH (SwARc <ID>_DESKTOP) is the
  desktop the game sees, so its window stays that size. Scripted pointer
  positions are the game's own pixels, independent of window and desktop:
  the front end's are in 800x600 (out/runs/newgame_1080.txt: SINGLE
  PLAYER 400 150, START NEW GAME 400 265, name box 400 508, NEXT 510 583,
  NORMAL 400 292).
- The front end runs at 800x600 (pillarboxed on wide screens, like
  CoD1's 4:3 menus); the game switches to the configured size in game.
- UI scale (src/ui_scale.cpp). The UI is laid out in screen pixels:
  UIShell (0x7A065C; screen size +0x60/+0x64, set by 0x6DE255) places
  each interface for that size from its intended resolution, windows
  draw XYZRHW quads (FVF 0x144) through Rapi's DrawPrimitive 0x654160,
  and the shell update 0x6DFFF7(dt, mouse x, y) hands the pointer to
  its mouse-move 0x6E108A (hover, tooltips: +0x78/+0x7C). Every 2D draw
  in game comes from the shell's draws 0x6E02B8, 0x6E046C, 0x6E0592,
  from the compass 0x66A28F (engine-drawn at viewport width - margin,
  margin; 0x66A370) or, while loading, the progress text. Now the shell
  lays out for the screen divided by a scale (height / 768, at least 1;
  DS1_UI_SCALE overrides), its draws are scaled up by it (in place in
  0x654160, restored after the draw), the pointer it gets is scaled
  down, and the compass is scaled about the top right corner. At
  3840x2160 (scale 2.8125, logical 1365x768) the HUD has its 1024x768
  proportions; clicking the menu button opens the in-game menu
  (centred, scaled); hover picking marks the right character. The
  menus at 800x600 are unchanged. Labels the game places at projected
  positions not checked.
- Software cursor at the UI scale (src/ui_scale.cpp). 0x656E02 (main
  thread before present, and the game's cursor thread between frames)
  blends a cursor image {+0 width = row stride, +4 height, +0xC ARGB32
  pixels} at the pointer minus the hotspot. The cursor object (global
  0x7A0680) holds the image in use (+0x38), the hotspot in use
  (+0x11C/+0x120) and the request (+0x148 texture, +0x14C/+0x150 hotspot,
  +0x154 dirty) set by 0x656D49(texture, hx, hy); 0x65754B takes on an
  image (and sizes the save surfaces). 0x656E02 also re-sets an earlier
  request it saved (+0x15C/+0x164/+0x16C, copied from the current one)
  through 0x656D49 (returning to 0x656F15). Now the request's hotspot is
  scaled (not on that internal call, whose values already are), the
  image is a bilinear copy scaled alike (premultiplied alpha, cached per
  image and scale, on the process heap), and a scale change (menus to
  game) sets the last request again. At 3840x2160 the sword is 2.8x the
  size, its tip on the pointer (DS1_TEST_FOCUS run; 1x for comparison
  with DS1_UI_SCALE=1).
  The copy is made where 0x656E02 looks up the requested texture's image
  (0x43CB2E, returning to 0x656F83 to compare it with +0x38 and to
  0x656FB1 to take it on). Made in 0x65754B at first, the image in use
  never equalled the looked-up one, so every request reloaded it and set
  +0x145 (moved): the game requests its cursor every frame, and the
  cursor thread, catching a request between frames, redrew the cursor on
  the primary ~17 times a second.

## Status (2026-10-12)
Since 2026-10-12 the lifter, x87 model, runtime and HLE are the shared
SwARc library (../../SwARc); src/runtime/..., tools/lifter_x86/...
and tests/... paths in the notes below now refer to that repository.
Lifted output is unchanged by the move (generated/ identical apart from
the namespace), all 5 tests pass, and the game boots to the main menu.

M4 in progress; single player is playable from a new game to Wesrin Cross
(user playtest, ~1 h of play). Verified in play: menus, hero creation, the
intro cinematic, movement and combat (frame rate holds with a party at the
krug camp), inventory, journal, map, spells (zap, fireball), potions,
hotkeys 1-4, skill level-ups, NPC conversations with voice, multi-stage
quests (Edgaar's basement, Seek Gyorn), region and basement transitions
(through Stonebridge and a torch-lit tower to Wesrin Cross), the first
crypt (life shrine, mana fountains, Ruby Gargoyle doors), a party member
(Ulora), krug shaman resurrections, shops (buying and selling; pack mule
bought in Stonebridge), party members falling unconscious and recovering,
party member death and resurrection, saving and loading, Options and Exit;
Miles audio and Bink movies. Debug and release builds hold the game's own
60 fps cap in game. Not yet seen: regions past Wesrin Cross, the ending.
Known gaps: the GDI "Lucida Console" font texture is blank (dev console /
debug text; needs a rasteriser), IME functions are not provided
(GetProcAddress NULL), the game's resolution list stops at 1024x768 (its
own filter). Open: the intermittent issues below.

M3 (main menu renders through the D3D7 HLE on dxvk-native) was reached
before; notes below.

M4 findings:
- Sound init is gated by prefs.gas `sound_enabled` in the per-user dir
  (~/.local/share/ds1/drive_c/Documents and Settings/Player/My
  Documents/Dungeon Siege/). The M3 stub's failed AIL_open_digital_driver
  made the game write `sound_enabled = false` there; reset it by hand.
- GPGSound (0x62D806..0x6309xx) is the only Miles client: 2D samples,
  up to 25 3D samples on "Miles Fast 2D Positional Audio" (picked when
  EAX is off), MP3/WAV streams for music/voice via AIL_open_stream with
  file callbacks into the Tank file system, fade timers.
- EAX (SwARc 23adb0b): with EAX on (the audio option, saved as
  `eax_enabled` in prefs.gas, or `enable_eax=true` on the command line,
  read in GPGSound init 0x62DBD5) the game opens "Creative Labs EAX (TM)"
  (EAX 1.0), allocates its 3D samples there, sets room types from its
  mood settings (eMoodRoomType) and per-sample effects levels.
- 3D coordinates: the game's world is right-handed (every VIEW matrix
  has det -1); GPGSound negates z on all positions and orientations to
  give Miles its left-handed space, so up x face is screen-right there.
- AIL_set_3D_sample_distances is documented (S, max, min) but the game
  passes (min, max), e.g. (6, 40); Mss32 swaps the pair so the larger is
  max. Taking them literally made every ambient loop full volume out to
  its max distance (loud waterfall, mis-heard panning).
- The game ships Miles 6.1c. Mssfast.m3d's falloff and pan law and
  Mss32's balance table were read from the shipped binaries; the
  formulas are in SwARc's audio/mixer.cpp (Mixer::TargetGains), and
  its asymmetry at +-90 degrees (hard left leaves -36 dB on the right,
  hard right nothing on the left) is the original's (SwARc
  platforms/win32/docs/miles.md, "Miles 6.1c Fast 2D").
- Game audio data: 619 PCM WAVs (22/44 kHz, 16-bit) + 131 raw MP3s in
  Sound.dsres; Mp3dec.asi in system/mss is Miles' MP3 decoder.
- Callbacks (EOS, stream end, timers) run on a Miles service guest thread
  (StartServiceThread) with no lock held; per-sample sequence numbers
  drop stale end-of-sample events. The game treats "finished" as a set,
  so whether AIL_end_sample fires EOS does not matter; we do not.
- Sample file images live in demand-paged Tank views: hle::EnsureReadable
  raises guest AVs for uncommitted pages before HLE code copies them
  (what a DLL's read would do on Windows).
- Bink movies are opened BINKFROMMEMORY | BINKSNDTRACK. ffmpeg's first
  Bink reference frame is zero bytes (green in YUV); RAD's is black. The
  credits keep their background as skip blocks, so frame buffers are
  pre-filled black (custom get_buffer2).
- Deactivation: the game's WM_ACTIVATEAPP(0) handler minimizes itself
  (fullscreen-exclusive behaviour). ShowWindow(SW_MINIMIZE) used to count
  as "show" and re-activate the app inside that handler, leaving the game
  inactive while we believed it active: it never resumed. Only
  activating SW_ commands activate now; in exclusive mode the host window
  keeps the display-mode size (MoveWindow/minimize are bookkeeping) and
  mouse coordinates are scaled window -> mode. DS1_TEST_FOCUS=lost,gained
  (seconds) injects focus loss/regain for automated runs.
- Cursor: the game re-centres the pointer every frame (SetCursorPos to the
  screen centre) and moves its own cursor by trunc((pos - centre) *
  sensitivity) (FUN_00414642; peek reads only touch locals). Warping the
  host pointer for that trapped it in the window. Windowed mode, where
  the game uses absolute positions, is disabled in this build (the store
  of the `fullscreen` setting at 0x41653A is NOPed). Now: SetCursorPos
  never warps; while the host pointer is over the window, GetCursorPos
  reads the game's cursor from its AppModule (GWL_USERDATA: cursor at
  +0x12C/+0x130, sensitivity +0x17C/+0x180) and returns the position that
  lands it exactly under the pointer; outside, the cursor stays put.
  Limitation: drag-rotate stops at the window edge (no pointer lock).
- Hangs: a watchdog dumps every guest thread's host backtrace (lifted
  function names) and the import each is inside to
  $XDG_DATA_HOME/ds1/hangs/ when the main thread stops pumping events for
  5 s (DS1_WATCHDOG_SECS). Open: a hard freeze in a new game when the
  hero drops the hoe (farmer warns of the Krug attack); models sometimes
  snap/fly off.
- New-game freeze (fixed): the game installs uniprocessor lock functions
  (plain inc/dec, 0x43EC99..) when it sees one CPU; we reported 1 while
  running its threads in parallel, which corrupted its handle lists. We
  report the host CPU count (2..8) now.
- Crash in dxvk from the worker thread (0x656862) uploading textures
  while the main thread rendered: the game sets DDSCL_MULTITHREADED, so
  every ddraw.dll entry point now runs under one recursive lock, like
  DirectDraw's own; the d3d9 device is created D3DCREATE_MULTITHREADED.
- In-game frame time 22.5 -> 16.7 ms (60 fps): ~75% of the main thread
  was the x87 model. Add/sub/mul/div of normal or zero operands now take
  an integer fast path (exact 128-bit result rounded by RoundPack; no
  host FPU), as do FST m32/m64 of values that stay normal; the rest still
  goes through SoftFloat. The bigger half of the win was mechanical:
  forced inlining of the arithmetic chain (F80s and result structs went
  through memory between calls: store-forwarding stalls) and no stack
  protector in ds1_x87. tests/x86/x87_fast_test checks the fast paths against
  SoftFloat (portable; 2M cases, mutation-checked); x87_test and lift_x87
  still pass against the hardware.
- Forest past the farm (the user's save, rain, many skinned models):
  27.7 -> 16.7 ms/frame (the game's 60 fps cap) with lifter x87 runs
  (ARCHITECTURE.md "x87 runs"): 6887 runs; the register stack in
  locals, one guard per run, write-back at the end. The rest of the win
  came from (1) passing values as two full 64-bit fields (F80's padded
  half was assembled in memory around every call: store-forwarding
  stalls), (2) the game's mode being PC=24 with RC=chop, not nearest (a
  first fast path for nearest only never triggered), and (3) forcing
  the PC=24 fast path inline. lift_x87_test now also runs 4000 random
  sequences of the EXE's x87 forms as blocks against the CPU; about half
  of the trials take the run path; mutation-checked.
- fsin/fcos/fsincos: the series now runs in 128-bit fixed point; the
  result is used only when it lies more than 2^-100 from every rounding
  boundary (both methods' errors are far below that), else the binary128
  path decides, so results are bit-identical to before (x87_fast_test,
  0 mismatches even at a 2^-120 margin). Binary128 dropped from ~13% to
  ~3% of the main thread in the rainy forest past the farm, which still
  runs ~26 ms/frame: millions of plain x87 instructions per frame (CPU
  skinning: quaternion rotate 0x5338E9 alone is ~19%) at ~5 ns each.
  ThinLTO of lifted code + ds1_x87 gained nothing measurable.
- Faster exact x87 (SwARc x87-fast 7e0dc70, 2026-10-13). Runs now
  have inline division, zero operands, m64 operands at PC=24, m32/m64
  loads and rounding stores, and compares. A single instruction is a
  run, and the tag word became an empty mask (SwARc
  ARCHITECTURE.md).
  - Measured on the newest save loaded by script (party at the farm
    gate, idle). The game sits at its 60 fps cap there, so the metric
    is main-thread work per frame outside the limiter, from perf leaf
    samples, two runs each:
    - Work: 6.55 -> 5.55 ms/frame (-15%).
    - Out-of-line x87 helpers: 3.0 -> 1.0 ms/frame.
    - CompareCC, StoreF64, StoreF32 and LoadConverted are gone from the
      profile.
  - The limiter (0x41B794 timer read, 0x41B991 Sleep(0) loop) is idle
    time. It converts 64-bit QPC counts at CW 0x0F7F (PC=64, chop)
    through run::ArithF64, which no fast path covers. Count it with the
    limiter, not as work.
- x87 runs on host doubles (SwARc x87-double 79a8931, 2026-10-13):
  no change here. The doubles apply only under CW 0x027F (PC=53,
  RC=nearest), and this game runs its float maths at PC=24/RC=chop
  under D3D.
  - Measured on the newest save loaded by script (in_load: Single
    Player, Load Game, Load). Isolated XDG_DATA_HOME copy,
    SDL_AUDIO_DRIVER=dummy, 60 fps cap. perf on the main thread (user
    task-clock, 1 ms period, dwarf call graphs) for 30 s from t=70 s.
    The limiter is every sample with f_0041B794 or f_0041B991 on the
    stack.
  - Work: base 7e0dc70 9.53 / 10.01 ms/frame, new 9.61 / 10.25,
    DS1_X87_DOUBLE=0 9.59. That is within run-to-run noise (+-0.3 ms).
    Out-of-line x87 helpers stay at 0.8 ms/frame.
  - The absolute work is higher than in the entry above: that save and
    setup are not the same. Compare within one entry only.
  - Lifted code: 160 -> 171 MB.
  - ctest 9/9 with DS1_GAME_EXE (lift_x87 against the CPU).
- Fullscreen, display modes, presentation (SwARc widescreen,
  2026-10-13). Exclusive mode (SetDisplayMode) now makes the window host
  fullscreen, borderless at the desktop's resolution, with the 4:3 frame
  pillarboxed on a wide screen; DS1_FULLSCREEN=window keeps the old
  mode-sized window (the default for DS1_BACKGROUND runs), =exclusive
  switches the host mode. The pointer maps back through the shown
  rectangle (over the bars it is at the screen's edge); relative motion
  is not scaled. EnumDisplayModes also lists the desktop's size and wide
  ones; the game still uses 800x600 for the menus and 1024x768 in game.
  - Scripted runs (in4 input, isolated XDG_DATA_HOME): windowed, desktop
    fullscreen (the input's pointer positions moved into the 2560x1440
    window's 1920x1440 picture) and DS1_GFX=d3d9 all reach the farm
    through the same menus, the scripted clicks landing on the same
    buttons; the frames match the build before (RMSE 2-3% in game, from
    animation).
  - ctest 10/10 with DS1_GAME_EXE (SwARc's pacing test is new).
  - ctest 9/9 with DS1_GAME_EXE (lift_x87: the EXE's forms and run
    sequences against the CPU).
- Freeze on a quick focus flip (fixed): with focus-follows-mouse, the
  pointer overshot the window and came back; the main thread hung in a
  back-buffer readback (cursor Blt -> GetRenderTargetData -> dxvk
  waitForResource). The GPU work sat behind a vsync (FIFO) present that
  the compositor never released (no frame callbacks). Vsync is now off by
  default and dxvk presents in mailbox mode (dxvk.tearFree = True, set in
  DXVK_CONFIG unless the user set tearFree): no tearing, never blocks; the
  game's own 60 fps cap paces it. DS1_VSYNC=1 restores FIFO. Confirmed
  in play: the same overshoot no longer freezes.
- Cursor glitches on mouse-over (fixed): the game draws a software
  cursor (0x656E02) only while GetForegroundWindow is its window: save
  the pixels under it, blend a 64x64 ARGB image, Blt back. The main
  thread does it on the back buffer before presenting; a cursor thread
  does it on the primary between frames. We treated the primary as the
  back buffer, so the cursor thread saved/restored the half-drawn next
  frame (black squares, stale cursors, smudges). The gfx device now has
  the game's back buffer, a front buffer (the primary: Present copies
  the back buffer there, then shows it) and the swap chain; primary
  Blt/Lock read and write the front buffer. Automated runs need
  DS1_TEST_FOCUS to exercise this path (background windows are never
  foreground).
- Instruction lifting verified: tests/x86/lift_x87_test (111 x87 forms) and
  tests/x86/lift_int_test (13703 integer forms) run every form in the EXE
  lifted and natively from random states, bit-exact (configure with
  -DDS1_GAME_EXE=...).
- Animated models snapping/flying off (fixed): IDirect3DDevice7::
  MultiplyTransform composed current x given; D3D's order is given x
  current ("pMatrix times State"; dxvk's column-major current*given is
  the same thing). The game calls it ~3.7M times in two minutes of play
  to place objects and bones. DS1_CHECK_INDICES=1 checks indexed draws
  against their vertex counts (clean).
- Hero could not move, farmer silent (fixed): our UnDecorateSymbolName
  differed from Microsoft's on 949 of 3230 exports ("X **" vs "X * *",
  ") const" vs ")const ", "*const __thiscall" on returned const
  pointers). FuBi parses these strings to register functions for Skrit,
  so jobs (move, conversations) failed silently. Now byte-identical to
  the game's own dbghelp.dll (platforms/win32/tools/undname). Dialogue "conversation_
  norick" plays s_v_fh_norick1 with subtitles; the hero moves.
- Automated runs: DS1_BACKGROUND ignores the desktop's real input;
  DS1_TEST_INPUT=<file> scripts input (goto/move/pointer/click/key);
  run.sh-style runs use their own XDG_DATA_HOME profile. Arguments after
  `--` go on the game's command line.
- Performance: the debug preset built the x87 model and SoftFloat at -O0;
  the menu intro animation (x87-heavy) ran at 135-145 ms/frame. They are
  -O2 in every preset now (also the mixer and pixel formats). The game
  locks the back buffer every frame without writing to it; Unlock now
  uploads only rows that changed (snapshot diff) and readback reuses its
  staging surface with a row-copy fast path. DS1_HITCH=ms logs slow
  frames.

Fixed on the way to the menu:
- UnmapViewOfFile zero-filled pages after remapping them PROT_NONE ->
  forwarded as a guest AV -> game's top-level __except shut it down.
  Faults inside HLE handlers are now runtime-bug fatals (t_hle_depth).
- Nested guest faults (the Tank decompressor faults inside its own SEH
  filter): SA_NODEFER + depth limit; handler guest stack gap 32 KiB.
- MoveWindow sent WM_SIZE/WM_MOVE unconditionally; the game's WM_SIZE
  handler calls MoveWindow -> infinite recursion. Now only on change.
- ShowWindow/activation recorded the active window after sending
  WM_ACTIVATEAPP -> re-entrant loop. Now recorded first.
- The game presents with Blt(primary <- backbuffer), not Flip: treated
  as a swap-chain Present.
- FIFO present to an occluded Wayland window blocks forever (froze the
  frame loop and the loading thread). DS1_VSYNC=0/1; DS1_BACKGROUND=1
  (automated runs) implies no vsync, no pointer warps, no focus steal.
- SetCursorPos only warps while the game window has keyboard focus (it
  recentres the pointer every frame and stole the user's mouse).
- SDL_HINT_NO_SIGNAL_HANDLERS: SIGTERM must actually stop the process.

Dev aids: DS1_SHOT_EVERY=N + DS1_SHOT_DIR=dir saves every Nth presented
frame as BMP. DS1_HITCH=ms logs frames slower than that. DS1_TEST_FOCUS=
lost,gained injects focus changes. Automated runs use
SDL_AUDIO_DRIVER=disk SDL_AUDIO_DISK_OUTPUT_FILE=out.raw (F32LE stereo
44.1 kHz) so nothing plays on the desktop's speakers.

M3 work so far (see platforms/win32/src/win32/{com,ddraw,d3d7,dbghelp}.cpp,
src/runtime/gfx/):
- Guest COM: objects [vtable][magic][id] in guest memory, vtables of named
  HLE thunks ("IDirectDrawSurface7::Blt"), unimplemented methods fatal.
- ddraw_types.h guest structs verified against the mingw SDK headers by
  generated static_asserts (platforms/win32/tools/layout, run once under wine).
- gfx::Device interface (D3D7 semantics) + d3d9 backend via dxvk-native.
- Adapter reported as GeForce3 (10DE/0200, 64 MB): best profile in
  system_detail.gas (trilinear, render-target shadows).
- Game passes coop flags 0xC13 incl. DDSCL_FPUSETUP (0x800), so D3D7 must
  NOT touch the FPU (answers the old open question; the game sets
  PC=24/RC=chop itself).
- Tank demand paging: the game reserves NOACCESS memory and an SEH filter
  (0064AB4C under __except_handler3) commits + reads on access violation.
  Host SIGSEGV in guest memory -> RaiseAccessViolation -> guest SEH;
  "continue execution" retries the faulting instruction. Lifted code is
  built with -fnon-call-exceptions; each guest thread has a 16 MB
  sigaltstack.
- Heap is MicroQuill SmartHeap (static, on VirtualAlloc/VirtualQuery);
  game never calls HeapAlloc.
- dbghelp: FuBi undecorates with flags 0x800 and parses the text;
  UndnameMsvc() normalises LLVM output to MSVC undname style (",", "> >",
  "X *", "(__cdecl*)"). Uses system libLLVMDemangle.a (vendor for macOS).

Bugs found and fixed in M3:
- Lifter: functions whose code extends below the entry (shared tails,
  e.g. strstr -> strchr body) started executing at the lowest address.
  Fix: emit `goto L_<entry>` first (372 functions affected).
- GlobalMemoryStatus / VirtualAlloc semantics (see M2 notes).

Debug tooling: DS1_TRACE=1 logs HLE calls with return values;
tools/ghidra/Decompile.java for headless decompiles; the fault handler
prints the guest address and lifted host backtrace.

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

## Open intermittent issues

- Pre-placed dead krug shown rotated upright and sunk into the ground
  (seen once, ~23 min into a session, just past Skartis on the way to the
  first crypt). Not saved: the same save loads with it lying correctly,
  it stays correct for 5+ min idle (automated) and across leaving into
  the crypt and back (user), with x87 runs on or off. So it is runtime
  placement state (orientation/position), rebuilt on load. Suspects:
  terrain alignment of the corpse, node-relative placement during
  streaming, or a race in the loader now that the game takes its
  multiprocessor paths (we report 2..8 CPUs). Needs a repro; Wine run of
  the original as a reference if it recurs.

- Startup crash inside dxvk, about 1 in 70 starts (3 of ~370 automated
  starts; only with DS1_GFX=d3d9 since SDL_GPU became the default): right after the d3d9 device is created, the main thread calls a
  null pointer from DxvkImage::updateDebugName() (libdxvk_d3d9.so
  +0x26431c, the vkSetDebugUtilsObjectNameEXT call). Debug utils are not
  enabled (no DXVK_DEBUG, no capture env), so that entry point is never
  loaded; reaching it means an image with a debug name and owned storage
  (only presenter swap images carry a name without capture mode) or a
  freed image. Happens with tearFree on (mailbox) and off (immediate), so
  not the focus-flip fix. Not reproduced under gdb. Next: rule vsync in
  or out (DS1_VSYNC=1 loop), then find the image (a dxvk patch branch with
  a null check plus a log of the image's info), and check upstream dxvk.

## Pinned for after the baseline (improvements over the original)

- Sounds drop out when many play at once (cut off or never start; the
  threshold varies). The original does the same on real hardware, so it
  is faithful, not an M4 bug. Likely cause: GPGSound's fixed pools of
  Miles handles (up to 25 3D samples on the Fast 2D provider, a few dozen
  2D samples) and its voice stealing when they run out. Revisit once the
  recompilation is a stable baseline (raise the pools or prioritise
  better, as an opt-in enhancement).

- Uncapped frame rate (planned): the game holds itself at 60 fps. Before
  lifting the cap: find where it is set and check that simulation, AI,
  animation and physics use elapsed time, not frames (a fixed step would
  speed the game up). Then profile a heavy scene: with no cap, x87 cost
  (~5 ns/instruction, CPU skinning heaviest) and everything else show up
  directly. Exact speedups first (runs across more code, dropping unread
  status/tag work); host float/double stays ruled out (CLAUDE.md).

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
- SoftFloat-only cost at PC=24/chop (before the integer fast paths):
  fadd ~19 ns, fld/fstp m32 ~10 ns, fsqrt ~24 ns, fsin ~210 ns, fsincos
  ~390 ns.

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
IID_IDirect3D7 (F5049E77), IID_IDirect3DTnLHalDevice (F5049E78, earlier
mislabelled as IID_IDirect3D7), IID_IDirect3DHALDevice,
IID_IDirectDrawGammaControl. No DirectInput (input is Win32
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
- SDL_GPU on Vulkan (2026-10-13): SwARc's SDL_GPU backend is now the
  default on Linux too (its shaders are GLSL, SPIR-V for Vulkan, MSL via
  SPIRV-Cross for Metal); dxvk-native is optional, for DS1_GFX=d3d9 as
  the reference. Against d3d9 on "Strange Shadows (0-50-27)" (menu,
  load, 70 s in game, a screenshot every 300 frames): in-game frames
  equal pixel for pixel except on the two characters and their shadow
  outlines (idle animation phase; mean |d| <= 0.11 of 255); the menu
  and loading frames differ only by timing (the SDL_GPU run finished
  loading sooner). The terrain, the HUD and the projected character
  shadows match. No Khronos validation messages over 90 s. Not yet
  played by hand on it.
- Character shadows (fixed): with complex shadows the game renders each
  caster's silhouette (grey, TEXTUREFACTOR CFCFCF) into a 64x64
  DDSCAPS_TEXTURE | DDSCAPS_3DDEVICE texture every frame (SetRenderTarget
  to it, then back to the back buffer), after a colour-fill Blt to white,
  and projects it onto the ground. We created those textures in the
  managed pool, which D3D9 cannot render into, so there were no shadows.
  They are now D3DUSAGE_RENDERTARGET textures in the default pool
  (recreated empty on device Reset), and a colour-fill Blt on one is a
  GPU ColorFill (the old fill went to guest memory and the upload failed,
  leaving the texture black: ground-wide black shadows).
- Shadow size (not a bug, most likely): shadows that looked too long in
  some saves follow the nearest light. In a torch-lit tower (user
  playtest) they change length and direction moving between torches.
  The game projects each caster's silhouette from a per-caster light
  (perspective, e.g. ~11 m away looking down ~38 deg in "Strange
  Shadows (0-50-27)"), and we pass its maths through unchanged: the
  silhouette pass and the ground pass share that projection with the
  standard 0.5 scale/bias, and the texture matrix is invView * that,
  in D3D's MultiplyTransform order. No reference from the original:
  it does not run under Wine/Proton here.

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
