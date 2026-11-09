// The VC6 CRT's locale and floating-point globals in the EXE, for the
// native functions lifter.manifest binds (msvc_crt::vc6::) and their
// oracle (tests/crt_stubs.cpp); FINDINGS.md "CRT".

#pragma once

#include "win32/msvc_crt.h"

inline constexpr recomp::native::msvc_crt::vc6::Locale kCrtLocale{
    .ctype_handle = 0x79D260,  // __lc_handle[LC_CTYPE]
    .codepage = 0x79D270,      // __lc_codepage
    .mb_cur_max = 0x75D808,    // __mb_cur_max
    .pctype = 0x75D814,        // _pctype
};

inline constexpr recomp::native::msvc_crt::vc6::Math kCrtMath{
    .fastflag = 0x79D050,      // _fastflag (0: never set)
    .adjust_fdiv = 0x79D054,   // _adjust_fdiv (set at startup)
    .matherr_flag = 0x7921D0,  // _matherr_flag (9876: the CRT's _matherr)
};
