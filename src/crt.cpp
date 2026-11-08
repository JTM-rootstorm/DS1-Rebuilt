// Functions of the EXE's statically linked VC6 CRT, natively. Replacements
// of lifted functions named with `replace` in lifter.manifest; FINDINGS.md
// "CRT". tests/crt_test.cpp checks each against the original.
//
// They work on guest memory in place. A fault on an uncommitted guest page
// (the tank views page themselves in from an exception filter) reaches the
// game's filters as it does from lifted code, and the faulting access is
// retried. Byte arguments (the character of memset, strchr, memchr) are
// the low byte of their int, as in the originals; comparisons are of
// unsigned chars and return -1, 0 or 1 (strcmp, strncmp).

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "x86/lifted.h"

namespace recomp::gen {

void f_0040E0D0_lifted(Ctx& c);
void f_0040E140_lifted(Ctx& c);

namespace {

using lift::Rd;
using lift::Wr;

uint32_t Arg(const Ctx& c, int n) { return Rd<uint32_t>(g_guest_base, c.esp + 4 + 4 * n); }
uint8_t* P(uint32_t va) { return g_guest_base + va; }
const char* S(uint32_t va) { return reinterpret_cast<const char*>(g_guest_base + va); }
uint16_t W(uint32_t va) { return Rd<uint16_t>(g_guest_base, va); }

// A cdecl function returning v: the caller pops the arguments.
void Ret(Ctx& c, uint32_t v) {
  c.eax = v;
  c.esp += 4;
}

// The 64-bit helpers: two 64-bit arguments (low dword first), popped by
// the callee (ret 16); the result in EDX:EAX.
uint64_t Arg64(const Ctx& c, int n) { return uint64_t(Arg(c, 2 * n + 1)) << 32 | Arg(c, 2 * n); }
void Ret64(Ctx& c, uint64_t v) {
  c.eax = uint32_t(v);
  c.edx = uint32_t(v >> 32);
  c.esp += 4 + 16;
}

int Sign(int d) { return (d > 0) - (d < 0); }

}  // namespace

// memset(dst, c, n) -> dst (0x40C8D0).
void f_0040C8D0(Ctx& c) {
  const uint32_t dst = Arg(c, 0);
  std::memset(P(dst), uint8_t(Arg(c, 1)), Arg(c, 2));
  Ret(c, dst);
}

// memcpy(dst, src, n) -> dst (0x40C930). VC6 builds memcpy from memmove's
// source: overlapping copies are as memmove's.
void f_0040C930(Ctx& c) {
  const uint32_t dst = Arg(c, 0);
  std::memmove(P(dst), P(Arg(c, 1)), Arg(c, 2));
  Ret(c, dst);
}

// memmove(dst, src, n) -> dst (0x40D130; the same code as memcpy, so the
// signature names both memcpy).
void f_0040D130(Ctx& c) {
  const uint32_t dst = Arg(c, 0);
  std::memmove(P(dst), P(Arg(c, 1)), Arg(c, 2));
  Ret(c, dst);
}

// memchr(buf, c, n) -> the first byte equal to c, or 0 (0x631C10).
void f_00631C10(Ctx& c) {
  const uint32_t buf = Arg(c, 0), n = Arg(c, 2);
  const void* hit = std::memchr(P(buf), uint8_t(Arg(c, 1)), n);
  Ret(c, hit ? buf + uint32_t(static_cast<const uint8_t*>(hit) - P(buf)) : 0);
}

// strlen(s) (0x40CC70).
void f_0040CC70(Ctx& c) { Ret(c, uint32_t(std::strlen(S(Arg(c, 0))))); }

// strcpy(dst, src) -> dst (0x40C7E0) and strcat(dst, src) -> dst
// (0x40C7F0): VC6's strcat.asm, whose signatures also match the names
// _mbscpy and _mbscat.
void f_0040C7E0(Ctx& c) {
  const uint32_t dst = Arg(c, 0), src = Arg(c, 1);
  std::memmove(P(dst), P(src), std::strlen(S(src)) + 1);
  Ret(c, dst);
}
void f_0040C7F0(Ctx& c) {
  const uint32_t dst = Arg(c, 0), src = Arg(c, 1);
  const uint32_t end = dst + uint32_t(std::strlen(S(dst)));
  std::memmove(P(end), P(src), std::strlen(S(src)) + 1);
  Ret(c, dst);
}

// strncpy(dst, src, n) -> dst (0x40DA70): src up to its NUL or n bytes,
// the rest of the n bytes zeroed.
void f_0040DA70(Ctx& c) {
  const uint32_t dst = Arg(c, 0), src = Arg(c, 1), n = Arg(c, 2);
  const uint32_t len = uint32_t(strnlen(S(src), n));
  std::memmove(P(dst), P(src), len);
  std::memset(P(dst + len), 0, n - len);
  Ret(c, dst);
}

// strncat(dst, src, n) -> dst (0x632090): at most n bytes of src, then a
// NUL; nothing written when n is 0.
void f_00632090(Ctx& c) {
  const uint32_t dst = Arg(c, 0), src = Arg(c, 1), n = Arg(c, 2);
  if (n) {
    const uint32_t end = dst + uint32_t(std::strlen(S(dst)));
    const uint32_t len = uint32_t(strnlen(S(src), n));
    std::memmove(P(end), P(src), len);
    P(end)[len] = 0;
  }
  Ret(c, dst);
}

// strcmp(a, b) -> -1, 0 or 1 (0x410580).
void f_00410580(Ctx& c) { Ret(c, uint32_t(Sign(std::strcmp(S(Arg(c, 0)), S(Arg(c, 1)))))); }

// strncmp(a, b, n) -> -1, 0 or 1 (0x40D7E0).
void f_0040D7E0(Ctx& c) {
  Ret(c, uint32_t(Sign(std::strncmp(S(Arg(c, 0)), S(Arg(c, 1)), Arg(c, 2)))));
}

// strchr(s, c) -> the first c in s, its NUL included, or 0 (0x40D6A0).
void f_0040D6A0(Ctx& c) {
  const uint32_t s = Arg(c, 0);
  const char* hit = std::strchr(S(s), char(Arg(c, 1)));
  Ret(c, hit ? s + uint32_t(hit - S(s)) : 0);
}

// strrchr(s, c) -> the last c in s, its NUL included, or 0 (0x631BE0).
void f_00631BE0(Ctx& c) {
  const uint32_t s = Arg(c, 0);
  const char* hit = std::strrchr(S(s), char(Arg(c, 1)));
  Ret(c, hit ? s + uint32_t(hit - S(s)) : 0);
}

// strstr(s, sub) -> the first sub in s (s itself for an empty sub), or 0
// (0x40D760).
void f_0040D760(Ctx& c) {
  const uint32_t s = Arg(c, 0);
  const char* hit = std::strstr(S(s), S(Arg(c, 1)));
  Ret(c, hit ? s + uint32_t(hit - S(s)) : 0);
}

// strcspn(s, set) (0x410610) and strpbrk(s, set) -> the first byte of s
// in set, or 0 (0x410650).
void f_00410610(Ctx& c) { Ret(c, uint32_t(std::strcspn(S(Arg(c, 0)), S(Arg(c, 1))))); }
void f_00410650(Ctx& c) {
  const uint32_t s = Arg(c, 0);
  const char* hit = std::strpbrk(S(s), S(Arg(c, 1)));
  Ret(c, hit ? s + uint32_t(hit - S(s)) : 0);
}

// The wide-character functions (16-bit wchar_t), as VC6's wcs*.c.

// wcslen(s) (0x409DAE).
void f_00409DAE(Ctx& c) {
  const uint32_t s = Arg(c, 0);
  uint32_t p = s;
  while (W(p)) p += 2;
  Ret(c, (p - s) / 2);
}

// wcscpy(dst, src) -> dst (0x632024): unit by unit, front to back.
void f_00632024(Ctx& c) {
  const uint32_t dst = Arg(c, 0);
  uint32_t d = dst, s = Arg(c, 1);
  uint16_t ch;
  do {
    ch = W(s);
    Wr<uint16_t>(g_guest_base, d, ch);
    s += 2, d += 2;
  } while (ch);
  Ret(c, dst);
}

// wcsncpy(dst, src, n) -> dst (0x71EB34): src up to its NUL or n units,
// the rest of the n units zeroed.
void f_0071EB34(Ctx& c) {
  const uint32_t dst = Arg(c, 0);
  uint32_t d = dst, s = Arg(c, 1), n = Arg(c, 2);
  for (; n; n--, d += 2) {
    const uint16_t ch = W(s);
    Wr<uint16_t>(g_guest_base, d, ch);
    if (!ch) break;
    s += 2;
  }
  // The NUL is copied and counted; the units after it are zeroed.
  if (n) {
    for (n--, d += 2; n; n--, d += 2) Wr<uint16_t>(g_guest_base, d, 0);
  }
  Ret(c, dst);
}

// wcschr(s, c) -> the first c in s, its NUL included, or 0 (0x631E9E).
void f_00631E9E(Ctx& c) {
  uint32_t p = Arg(c, 0);
  const uint16_t ch = uint16_t(Arg(c, 1));
  while (W(p) && W(p) != ch) p += 2;
  Ret(c, W(p) == ch ? p : 0);
}

// wcsrchr(s, c) -> the last c in s, its NUL included, or 0 (0x632049).
void f_00632049(Ctx& c) {
  const uint32_t s = Arg(c, 0);
  const uint16_t ch = uint16_t(Arg(c, 1));
  uint32_t p = s;
  while (W(p)) p += 2;
  while (p != s && W(p) != ch) p -= 2;
  Ret(c, W(p) == ch ? p : 0);
}

// wcscspn(s, set) (0x631D84).
void f_00631D84(Ctx& c) {
  const uint32_t s = Arg(c, 0), set = Arg(c, 1);
  uint32_t p = s;
  for (; W(p); p += 2) {
    uint32_t q = set;
    while (W(q) && W(q) != W(p)) q += 2;
    if (W(q)) break;
  }
  Ret(c, (p - s) / 2);
}

// The compiler's 64-bit integer helpers.

// _allmul(a, b) -> a * b, mod 2^64 (0x411C30).
void f_00411C30(Ctx& c) { Ret64(c, Arg64(c, 0) * Arg64(c, 1)); }

// _aulldiv(a, b) -> a / b (0x40E0D0) and _aullrem(a, b) -> a % b
// (0x40E140), unsigned. A zero divisor faults in the original's div; the
// original runs then, to raise it from there.
void f_0040E0D0(Ctx& c) {
  const uint64_t b = Arg64(c, 1);
  if (!b) return f_0040E0D0_lifted(c);
  Ret64(c, Arg64(c, 0) / b);
}
void f_0040E140(Ctx& c) {
  const uint64_t b = Arg64(c, 1);
  if (!b) return f_0040E140_lifted(c);
  Ret64(c, Arg64(c, 0) % b);
}

// _allshl: EDX:EAX <<= CL, 0 from 64 on (0x637090). Register arguments,
// nothing on the stack.
void f_00637090(Ctx& c) {
  const uint32_t n = c.ecx & 0xFF;
  const uint64_t v = n < 64 ? (uint64_t(c.edx) << 32 | c.eax) << n : 0;
  c.eax = uint32_t(v);
  c.edx = uint32_t(v >> 32);
  c.esp += 4;
}

// _alloca_probe / _chkstk (0x40D820): allocates EAX bytes of stack for its
// caller. It touches each page from the top down (Windows commits a stack
// a guard page at a time), returns with ESP lowered by EAX, ECX kept and
// the return address in EAX. Its own pushes are kept too: ECX at the old
// ESP-4, the return address just below the new ESP.
void f_0040D820(Ctx& c) {
  const uint32_t ret = Rd<uint32_t>(g_guest_base, c.esp);
  const uint32_t top = c.esp + 4, bottom = top - c.eax;
  Wr<uint32_t>(g_guest_base, c.esp - 4, c.ecx);
  for (uint32_t left = c.eax, p = top; left >= 0x1000; left -= 0x1000) {
    p -= 0x1000;
    (void)*reinterpret_cast<volatile uint32_t*>(P(p));
  }
  (void)*reinterpret_cast<volatile uint32_t*>(P(bottom));
  Wr<uint32_t>(g_guest_base, bottom - 4, ret);
  c.eax = ret;
  c.esp = bottom;
}

// _ftol: ST0 to a 64-bit integer, truncated, popped; EDX:EAX (0x631810).
// The original stores it with fistp under RC=chop and puts the control
// word back; the x87 model does the same, so out-of-range values, NaNs and
// the exception flags are the hardware's.
void f_00631810(Ctx& c) {
  x87::State& s = c.fpu;
  const uint16_t cw = x87::ControlWord(s);
  x87::SetControlWord(s, cw | 0x0C00);
  const int64_t v = x87::StoreI64(s);
  x87::Pop(s);
  x87::SetControlWord(s, cw);
  c.eax = uint32_t(v);
  c.edx = uint32_t(uint64_t(v) >> 32);
  c.esp += 4;
}

}  // namespace recomp::gen
