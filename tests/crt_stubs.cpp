// The game's side of SwARc's msvc_crt_oracle (CMakeLists.txt): what the
// CRT's originals call outside the CRT, replaced by tests/crt_stubs.manifest,
// and the oracle's Setup().

#include <cstdio>
#include <cstdlib>

#include "../src/crt_locale.h"
#include "runtime/guest_memory.h"
#include "x86/lifted.h"

namespace recomp::gen {

// The game's allocator (0x403BB0, cdecl malloc) and its free (0x403BA0),
// which _strlwr and _strupr use for their buffer: a bump allocator, whose
// memory the oracle does not compare.
constexpr uint32_t kHeap = 0x2E000000, kHeapSize = 0x100000;
uint32_t g_heap_next = kHeap;
void f_00403BB0(Ctx& c) {
  const uint32_t n = (*reinterpret_cast<uint32_t*>(g_guest_base + c.esp + 4) + 15) & ~15u;
  if (g_heap_next + n > kHeap + kHeapSize) g_heap_next = kHeap;
  c.eax = g_heap_next;
  g_heap_next += n;
  c.esp += 4;
}
void f_00403BA0(Ctx& c) { c.esp += 4; }
// The CRT's _lock: only taken while setlocale runs (its _unlock is lifted
// with the rest; it would reach LeaveCriticalSection, which Dispatch
// refuses).
void f_0040B408(Ctx&) {
  std::printf("unexpected _lock\n");
  std::abort();
}
// _errno (0x40E228): the thread's errno lives in its CRT data (__getptd,
// through TlsGetValue); here one int. Only originals set it (the native
// strtol hands overflows to the original), so it is not compared.
constexpr uint32_t kErrno = kHeap + kHeapSize;
void f_0040E228(Ctx& c) {
  c.eax = kErrno;
  c.esp += 4;
}
void f_0040A1CE(Ctx&) {
  std::printf("unexpected _exit\n");
  std::abort();
}

}  // namespace recomp::gen

namespace recomp::native::msvc_crt::oracle {

void Setup(GuestMemory& mem) {
  mem.Commit(gen::kHeap, gen::kHeapSize + 0x1000, Prot::RW);  // and errno
  vc6::SetLocale(kCrtLocale);
}

}  // namespace recomp::native::msvc_crt::oracle
