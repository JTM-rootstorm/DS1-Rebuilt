// The VC6 CRT's locale globals in the EXE, for the native locale functions
// lifter.manifest binds (msvc_crt::vc6::) and their oracle
// (tests/crt_stubs.cpp); FINDINGS.md "CRT".

#pragma once

#include "win32/msvc_crt.h"

inline constexpr recomp::native::msvc_crt::vc6::Locale kCrtLocale{
    .ctype_handle = 0x79D260,  // __lc_handle[LC_CTYPE]
    .codepage = 0x79D270,      // __lc_codepage
    .mb_cur_max = 0x75D808,    // __mb_cur_max
    .pctype = 0x75D814,        // _pctype
};
