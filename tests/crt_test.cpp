// The native CRT functions (src/crt.cpp) against the originals, lifted
// from the EXE at build time with `recomp_lift --only` (CMakeLists.txt).
//
//   crt_test EXE
//
// Each function runs on random arguments twice from the same guest memory
// and registers, once lifted and once natively. Compared: the result
// (EAX, and EDX where the function returns 64 bits), ESP, EBX, ESI, EDI
// and EBP, ECX where the original keeps it (_alloca_probe), every byte of
// the data area and of the stack at and above the returning ESP, and for
// _ftol the whole x87 state. The strings are short, drawn from a small
// alphabet (with bytes and units over 0x7F) so that searches and compares
// hit, at every alignment. Not compared: strcpy and strcat between
// overlapping strings (undefined in C; the originals copy forwards a
// dword at a time, the native code as memmove), and division by zero in
// _aulldiv/_aullrem, which the native code hands to the original.

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "runtime/guest_memory.h"
#include "win32/pe_image.h"
#include "x86/lifted.h"

namespace recomp {
uint8_t* g_guest_base = nullptr;

// The lifted originals (their own names: the manifest replaces them), and
// the lifted code's calls between them go to the originals.
#define CRT_FUNCTIONS(X)                                                                         \
  X(0040C8D0) X(0040C930) X(0040D130) X(00631C10) X(0040CC70) X(0040C7E0) X(0040C7F0)            \
  X(0040DA70) X(00632090) X(00410580) X(0040D7E0) X(0040D6A0) X(00631BE0) X(0040D760)            \
  X(00410610) X(00410650) X(00409DAE) X(00632024) X(0071EB34) X(00631E9E) X(00632049)            \
  X(00631D84) X(00411C30) X(0040E0D0) X(0040E140) X(00637090) X(0040D820) X(00631810)

namespace gen {
#define DECLARE(va)                 \
  void f_##va##_lifted(Ctx& c);     \
  void n_##va(Ctx& c);              \
  void f_##va(Ctx& c) { f_##va##_lifted(c); }
CRT_FUNCTIONS(DECLARE)
#undef DECLARE
}  // namespace gen

void Dispatch(Ctx&, uint32_t target, uint32_t va) {
  std::printf("unexpected call to %08X from %08X\n", target, va);
  std::abort();
}
void Resume(Ctx&, GuestResume r) {
  std::printf("unexpected resume to %08X\n", r.target);
  std::abort();
}
void Unimplemented(Ctx&, uint32_t va, const char* what) {
  std::printf("unimplemented %08X %s\n", va, what);
  std::abort();
}
void BadJump(Ctx&, uint32_t from, uint32_t to) {
  std::printf("bad jump %08X -> %08X\n", from, to);
  std::abort();
}
void Unreachable(Ctx&, uint32_t va) {
  std::printf("unreachable %08X\n", va);
  std::abort();
}
void Trap(Ctx&, uint32_t va) {
  std::printf("trap %08X\n", va);
  std::abort();
}
void RaiseDivideError(Ctx&, uint32_t va) {
  std::printf("divide error %08X\n", va);
  std::abort();
}
void Cpuid(Ctx&) { std::abort(); }
uint64_t Rdtsc(Ctx&) { std::abort(); }
void Probe(Ctx&, uint32_t, const char*) {}
void FatalF(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  std::vprintf(fmt, ap);
  va_end(ap);
  std::abort();
}

}  // namespace recomp

namespace {

using recomp::Ctx;
using Fn = void (*)(Ctx&);

recomp::GuestMemory g_mem;
std::mt19937 g_rng(20261107);

// Data at kData (the strings and buffers), the stack below kStackTop.
constexpr uint32_t kData = 0x2F000000, kDataSize = 0x4000;
constexpr uint32_t kStack = 0x2FE00000, kStackSize = 0x100000, kStackTop = kStack + kStackSize;
// The part of the stack a call can write: frames start in the top 4.3 KiB
// and _alloca_probe moves ESP down by at most 256 KiB.
constexpr uint32_t kStackUsed = 0x48000;

uint32_t Rand(uint32_t n) { return uint32_t(g_rng() % n); }

struct Result {
  Ctx c;
  std::vector<uint8_t> data, stack;
};

enum Regs : unsigned { kEdx = 1, kEcx = 2, kFpu = 4 };

struct Stats {
  long calls = 0, failures = 0;
};

bool SameFpu(const recomp::x87::State& a, const recomp::x87::State& b) {
  if (a.cw != b.cw || a.sw != b.sw || a.empty != b.empty || a.top != b.top) return false;
  for (int i = 0; i < 8; i++)
    if (!(a.empty >> i & 1) && (a.r[i].sig != b.r[i].sig || a.r[i].se != b.r[i].se)) return false;
  return true;
}

Result Run(Fn f, const Ctx& c0) {
  Result r;
  r.c = c0;
  f(r.c);
  r.data.assign(g_mem.ptr(kData), g_mem.ptr(kData) + kDataSize);
  r.stack.assign(g_mem.ptr(r.c.esp), g_mem.ptr(kStackTop));
  return r;
}

// Runs the original and the native function from the guest memory and
// registers as they are now (arguments already on the stack at c0.esp).
void Check(const char* name, Fn lifted, Fn native, const Ctx& c0, unsigned regs, Stats& s,
           const std::string& what) {
  std::vector<uint8_t> data(g_mem.ptr(kData), g_mem.ptr(kData) + kDataSize);
  std::vector<uint8_t> stack(g_mem.ptr(kStackTop - kStackUsed), g_mem.ptr(kStackTop));
  Result a = Run(lifted, c0);
  std::memcpy(g_mem.ptr(kData), data.data(), kDataSize);
  std::memcpy(g_mem.ptr(kStackTop - kStackUsed), stack.data(), kStackUsed);
  Result b = Run(native, c0);
  s.calls++;
  const Ctx &x = a.c, &y = b.c;
  bool ok = x.eax == y.eax && x.esp == y.esp && x.ebx == y.ebx && x.esi == y.esi &&
            x.edi == y.edi && x.ebp == y.ebp && a.data == b.data && a.stack == b.stack;
  if (regs & kEdx) ok &= x.edx == y.edx;
  if (regs & kEcx) ok &= x.ecx == y.ecx;
  if (regs & kFpu) ok &= SameFpu(x.fpu, y.fpu);
  if (!ok && s.failures++ < 30) {
    std::printf("%s(%s): eax %08X/%08X edx %08X/%08X ecx %08X/%08X esp %08X/%08X data %s stack %s\n",
                name, what.c_str(), x.eax, y.eax, x.edx, y.edx, x.ecx, y.ecx, x.esp, y.esp,
                a.data == b.data ? "same" : "differs", a.stack == b.stack ? "same" : "differs");
  }
}

// Random registers, the return address and the arguments on the stack.
Ctx Frame(std::initializer_list<uint32_t> args) {
  Ctx c{};
  c.eax = g_rng(), c.ecx = g_rng(), c.edx = g_rng(), c.ebx = g_rng();
  c.ebp = g_rng(), c.esi = g_rng(), c.edi = g_rng();
  c.esp = kStackTop - 0x1000 - 4 * Rand(64);
  recomp::x87::Init(c.fpu);
  uint32_t p = c.esp;
  g_mem.write<uint32_t>(p, 0xDEAD0000);
  for (uint32_t v : args) g_mem.write<uint32_t>(p += 4, v);
  return c;
}

// The data area: random bytes from a small alphabet, NULs among them.
const uint8_t kAlphabet[] = {0, 'a', 'b', 'c', 'A', 'B', 'Z', '/', 0x7F, 0x80, 0xC4, 0xFF};
uint8_t RandByte() { return kAlphabet[Rand(sizeof kAlphabet)]; }

void FillData() {
  for (uint32_t i = 0; i < kDataSize; i++) g_mem.write<uint8_t>(kData + i, RandByte());
}

// A NUL-terminated string of `len` bytes (no NUL inside) at a random
// offset in [lo, hi) of the data area; returns its address.
uint32_t PutString(uint32_t len, uint32_t lo, uint32_t hi) {
  const uint32_t at = kData + lo + Rand(hi - lo - len - 8);
  for (uint32_t i = 0; i < len; i++) {
    uint8_t b;
    do b = RandByte();
    while (!b);
    g_mem.write<uint8_t>(at + i, b);
  }
  g_mem.write<uint8_t>(at + len, 0);
  return at;
}
uint32_t PutWString(uint32_t len, uint32_t lo, uint32_t hi) {
  const uint32_t at = (kData + lo + Rand(hi - lo - 2 * len - 8)) & ~1u;
  static const uint16_t units[] = {'a', 'b', 'A', 0x80, 0xC4, 0x100, 0xD800, 0xFFFF};
  for (uint32_t i = 0; i < len; i++) g_mem.write<uint16_t>(at + 2 * i, units[Rand(8)]);
  g_mem.write<uint16_t>(at + 2 * len, 0);
  return at;
}
uint32_t Len() { return Rand(8) ? Rand(24) : Rand(400); }
uint32_t SmallChar() {
  // The character argument with random upper bits, which the originals ignore.
  return (g_rng() & 0xFFFFFF00) | RandByte();
}

#define FN(va) #va, recomp::gen::f_##va##_lifted, recomp::gen::n_##va

void Strings(Stats& s) {
  for (int i = 0; i < 20000; i++) {
    FillData();
    // Two strings in disjoint halves (dst in the first, with room after it).
    const uint32_t a = PutString(Len(), 0, 0x1800), b = PutString(Len(), 0x2000, 0x4000);
    const uint32_t a2 = Rand(4) ? PutString(Len(), 0x1800, 0x2000) : b;
    const uint32_t n = Rand(4) ? Rand(32) : Rand(600);
    const uint32_t ch = SmallChar();
    const std::string w = std::to_string(i);
    Check(FN(0040CC70), Frame({a}), 0, s, w);
    Check(FN(0040C7E0), Frame({a, b}), 0, s, w);
    Check(FN(0040C7F0), Frame({a, b}), 0, s, w);
    Check(FN(0040DA70), Frame({a, b, n}), 0, s, w);
    Check(FN(00632090), Frame({a, b, n}), 0, s, w);
    Check(FN(00410580), Frame({a, a2}), 0, s, w);
    Check(FN(00410580), Frame({a, a}), 0, s, w);
    Check(FN(0040D7E0), Frame({a, a2, n}), 0, s, w);
    Check(FN(0040D6A0), Frame({a, ch}), 0, s, w);
    Check(FN(0040D6A0), Frame({a, ch & 0xFFFFFF00}), 0, s, w);
    Check(FN(00631BE0), Frame({a, ch}), 0, s, w);
    Check(FN(0040D760), Frame({a, PutString(Rand(4), 0x3000, 0x4000)}), 0, s, w);
    Check(FN(0040D760), Frame({a, a + Rand(4)}), 0, s, w);
    Check(FN(00410610), Frame({a, PutString(Rand(5), 0x3000, 0x4000)}), 0, s, w);
    Check(FN(00410650), Frame({a, PutString(Rand(5), 0x3000, 0x4000)}), 0, s, w);
  }
}

void Memory(Stats& s) {
  for (int i = 0; i < 20000; i++) {
    FillData();
    const uint32_t n = Rand(4) ? Rand(80) : Rand(0x1000);
    const uint32_t d = kData + Rand(0x1800), src = kData + Rand(0x2000);  // may overlap
    const std::string w = std::to_string(i);
    Check(FN(0040C8D0), Frame({d, SmallChar(), n}), 0, s, w);
    Check(FN(0040C930), Frame({d, src, n}), 0, s, w);
    Check(FN(0040D130), Frame({d, src, n}), 0, s, w);
    Check(FN(00631C10), Frame({src, SmallChar(), n}), 0, s, w);
  }
}

void WideStrings(Stats& s) {
  for (int i = 0; i < 20000; i++) {
    FillData();
    const uint32_t a = PutWString(Len() / 2, 0, 0x1800), b = PutWString(Len() / 2, 0x2000, 0x4000);
    const uint32_t n = Rand(4) ? Rand(32) : Rand(300);
    const uint32_t ch = (g_rng() & 0xFFFF0000) | g_mem.read<uint16_t>(a + 2 * Rand(4));
    const std::string w = std::to_string(i);
    Check(FN(00409DAE), Frame({a}), 0, s, w);
    Check(FN(00632024), Frame({a, b}), 0, s, w);
    Check(FN(0071EB34), Frame({a, b, n}), 0, s, w);
    Check(FN(00631E9E), Frame({a, ch}), 0, s, w);
    Check(FN(00631E9E), Frame({a, ch & 0xFFFF0000}), 0, s, w);
    Check(FN(00632049), Frame({a, ch}), 0, s, w);
    Check(FN(00631D84), Frame({a, PutWString(Rand(4), 0x3000, 0x4000)}), 0, s, w);
  }
}

uint64_t Rand64() {
  // Mostly values whose halves are 0, small or all ones, where the
  // helpers take their shortcuts.
  auto half = [] {
    switch (Rand(4)) {
      case 0: return 0u;
      case 1: return Rand(16);
      case 2: return 0xFFFFFFFFu - Rand(16);
      default: return uint32_t(g_rng());
    }
  };
  return uint64_t(half()) << 32 | half();
}

void Integers(Stats& s) {
  for (int i = 0; i < 1000000; i++) {
    const uint64_t a = Rand64(), b = Rand64();
    const std::string w = std::to_string(i);
    auto args = [](uint64_t x, uint64_t y) {
      return Frame({uint32_t(x), uint32_t(x >> 32), uint32_t(y), uint32_t(y >> 32)});
    };
    Check(FN(00411C30), args(a, b), kEdx, s, w);
    if (b) {
      Check(FN(0040E0D0), args(a, b), kEdx, s, w);
      Check(FN(0040E140), args(a, b), kEdx, s, w);
    }
    Ctx c = Frame({});
    c.eax = uint32_t(a), c.edx = uint32_t(a >> 32);
    c.ecx = (g_rng() & 0xFFFFFF00) | (Rand(2) ? Rand(80) : Rand(256));
    Check(FN(00637090), c, kEdx, s, w);
  }
}

void AllocaProbe(Stats& s) {
  for (int i = 0; i < 20000; i++) {
    Ctx c = Frame({});
    c.eax = Rand(4) ? Rand(0x100) * 4 : Rand(0x40000) & ~3u;
    Check(FN(0040D820), c, kEcx, s, std::to_string(c.eax));
  }
}

// _ftol: ST0 from every class and range, under every rounding and
// precision mode, with random state below it on the stack.
recomp::x87::F80 RandF80() {
  using recomp::x87::F80;
  const uint16_t sign = Rand(2) ? 0x8000 : 0;
  switch (Rand(6)) {
    case 0: return {0, uint16_t(sign)};                                     // zero
    case 1: return {0x8000000000000000ull, uint16_t(sign | 0x7FFF)};        // infinity
    case 2: return {0xC000000000000000ull | g_rng(), uint16_t(sign | 0x7FFF)};  // NaN
    case 3: return {uint64_t(g_rng()) << 32 | g_rng(), uint16_t(sign)};     // denormal
    default: {
      // Normal, mostly around the 64-bit integer range.
      const uint16_t e = uint16_t(0x3FFF + (Rand(4) ? int(Rand(70)) - 4 : int(Rand(0x7FFE)) - 0x3FFF));
      return {0x8000000000000000ull | uint64_t(g_rng()) << 32 | g_rng(), uint16_t(sign | e)};
    }
  }
}

void Ftol(Stats& s) {
  for (int i = 0; i < 1000000; i++) {
    Ctx c = Frame({});
    for (int k = Rand(4); k > 0; k--) recomp::x87::LoadF80(c.fpu, RandF80());
    recomp::x87::LoadF80(c.fpu, RandF80());
    // Every precision and rounding control, exceptions masked (as the game
    // runs) and some sticky flags already set.
    recomp::x87::SetControlWord(c.fpu, uint16_t(0x003F | Rand(4) << 8 | Rand(4) << 10));
    c.fpu.sw |= uint16_t(Rand(0x40) & 0x3D);
    Check(FN(00631810), c, kEdx | kFpu, s, std::to_string(i));
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);  // messages before an abort
  if (argc < 2) {
    std::printf("usage: %s DungeonSiege.exe\n", argv[0]);
    return 2;
  }
  if (!g_mem.Reserve()) return 1;
  recomp::g_guest_base = g_mem.base();
  // The image: memcpy's jump tables are in it.
  recomp::PeImage exe;
  std::string err;
  if (!exe.Load(argv[1], g_mem, &err)) {
    std::printf("%s: %s\n", argv[1], err.c_str());
    return 1;
  }
  g_mem.Commit(kData, kDataSize, recomp::Prot::RW);
  g_mem.Commit(kStack, kStackSize, recomp::Prot::RW);
  Stats s;
  Strings(s);
  Memory(s);
  WideStrings(s);
  Integers(s);
  AllocaProbe(s);
  Ftol(s);
  std::printf("%ld calls: %ld mismatched\n", s.calls, s.failures);
  return s.failures ? 1 : 0;
}
