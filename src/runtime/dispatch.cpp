// Indirect control flow and the fatal paths of lifted code.
//
// Dispatch() maps a guest address to its lifted function through a dense
// table over .text, or to an HLE handler for thunk addresses. Anything
// else is fatal: there is no interpreter fallback (CLAUDE.md).

#include <execinfo.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <signal.h>
#include <unistd.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "runtime/hle/hle.h"
#include "runtime/lifted.h"
#include "runtime/log.h"
#include "runtime/runtime.h"

namespace ds1 {

uint8_t* g_guest_base = nullptr;
bool g_trace = false;

namespace {

uint32_t g_text_lo = 0, g_text_size = 0;
std::vector<GuestFn> g_table;

void DumpGuestState(const Ctx& c) {
  std::fprintf(stderr,
               "  eax=%08X ecx=%08X edx=%08X ebx=%08X\n"
               "  esp=%08X ebp=%08X esi=%08X edi=%08X\n"
               "  flags: cf=%d zf=%d sf=%d of=%d df=%d  fpu: cw=%04X sw=%04X\n",
               c.eax, c.ecx, c.edx, c.ebx, c.esp, c.ebp, c.esi, c.edi, c.f.cf,
               c.f.zf, c.f.sf, c.f.of, c.f.df, x87::ControlWord(c.fpu),
               x87::StatusWord(c.fpu));
  if (!g_proc) return;
  // Guest backtrace along the EBP chain (VC6 code mostly keeps frames).
  std::fprintf(stderr, "  guest backtrace (ebp chain):\n");
  uint32_t ebp = c.ebp;
  for (int i = 0; i < 24; i++) {
    if (!g_proc->mem.IsCommitted(ebp) || !g_proc->mem.IsCommitted(ebp + 4))
      break;
    uint32_t ret = Rd32(ebp + 4);
    std::fprintf(stderr, "    #%-2d ret %08X  (frame %08X)\n", i, ret, ebp);
    uint32_t next = Rd32(ebp);
    if (next <= ebp) break;
    ebp = next;
  }
  std::fprintf(stderr, "  stack top:");
  for (int i = 0; i < 8; i++)
    if (g_proc->mem.IsCommitted(c.esp + 4 * i))
      std::fprintf(stderr, " %08X", Rd32(c.esp + 4 * i));
  std::fprintf(stderr, "\n");
}

}  // namespace

void FatalF(const char* fmt, ...) {
  std::fflush(stdout);
  std::fprintf(stderr, "FATAL: ");
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(stderr, fmt, ap);
  va_end(ap);
  std::fprintf(stderr, "\n");
  if (Thread* t = CurrentThread()) {
    std::fprintf(stderr, "  guest thread %04X\n", t->tid);
    DumpGuestState(t->ctx);
  }
  std::fflush(stderr);
  std::_Exit(3);
}

void WarnF(const char* fmt, ...) {
  std::fprintf(stderr, "warning: ");
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(stderr, fmt, ap);
  va_end(ap);
}

void TraceF(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(stderr, fmt, ap);
  va_end(ap);
}

// Host faults (SIGSEGV/SIGBUS) in guest memory are guest access
// violations: they go to the guest SEH chain (the game demand-pages Tank
// data from an exception filter). Anything unhandled, or a fault outside
// the arena (a runtime bug), is reported with the host call chain (lifted
// functions are named f_<guest va>) and exits.
namespace hle {
bool RaiseAccessViolation(Ctx& c, bool write, uint32_t addr);
extern thread_local int t_hle_depth;
}

namespace {
thread_local bool t_in_fault = false;

void FaultHandler(int sig, siginfo_t* si, void* uc) {
  char buf[256];
  uintptr_t a = reinterpret_cast<uintptr_t>(si->si_addr);
  uintptr_t base = reinterpret_cast<uintptr_t>(g_guest_base);
  // Guest access violations go to the guest's SEH chain first: the game
  // commits demand-paged resource memory from an exception filter.
  if (a - base < (1ull << 32) && CurrentThread() && !t_in_fault &&
      hle::t_hle_depth == 0) {
    bool write = false;
#if defined(__linux__) && defined(__x86_64__)
    write = (static_cast<ucontext_t*>(uc)->uc_mcontext.gregs[REG_ERR] & 2) != 0;
#endif
    t_in_fault = true;
    bool cont = hle::RaiseAccessViolation(CurrentThread()->ctx, write,
                                          uint32_t(a - base));
    t_in_fault = false;
    if (cont) return;  // retry the faulting instruction
  }
  (void)uc;
  int n;
  if (a - base < (1ull << 32))
    n = std::snprintf(buf, sizeof buf,
                      "FATAL: %s: guest memory fault at guest %08lX\n",
                      sig == SIGSEGV ? "SIGSEGV" : "SIGBUS",
                      static_cast<unsigned long>(a - base));
  else
    n = std::snprintf(buf, sizeof buf,
                      "FATAL: %s: host fault at %p (runtime bug)\n",
                      sig == SIGSEGV ? "SIGSEGV" : "SIGBUS", si->si_addr);
  (void)!write(2, buf, size_t(n));
  void* frames[48];
  int nf = backtrace(frames, 48);
  backtrace_symbols_fd(frames, nf, 2);
  _exit(4);
}
}  // namespace

// Each host thread running guest code needs its own signal stack: SEH
// handlers for guest faults run lifted code on it, so it is large.
void InstallSignalStack() {
  constexpr size_t kSize = 16u << 20;
  void* p = mmap(nullptr, kSize, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  stack_t ss{};
  ss.ss_sp = p;
  ss.ss_size = kSize;
  sigaltstack(&ss, nullptr);
}

namespace {
void InstallFaultHandler() {
  InstallSignalStack();
  struct sigaction sa{};
  sa.sa_sigaction = FaultHandler;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigaction(SIGSEGV, &sa, nullptr);
  sigaction(SIGBUS, &sa, nullptr);
}
}  // namespace

void InitDispatch() {
  InstallFaultHandler();
  for (const auto& s : g_proc->img.sections)
    if (s.name == ".text") {
      g_text_lo = s.va;
      g_text_size = s.vsize;
    }
  g_table.assign(g_text_size, nullptr);
  for (uint32_t i = 0; i < kLiftedFunctionCount; i++) {
    const LiftedFn& f = kLiftedFunctions[i];
    if (f.va - g_text_lo < g_text_size) g_table[f.va - g_text_lo] = f.fn;
  }
}

void Dispatch(Ctx& c, uint32_t target, uint32_t va) {
  if (va == 0x40A0A3u || va == 0x0u)
    DS1_TRACE("  dispatch %08X from %08X\n", target, va);
  if (target - g_text_lo < g_text_size) {
    if (GuestFn fn = g_table[target - g_text_lo]) {
      fn(c);
      return;
    }
    FatalF("indirect branch to unlifted address %08X from %08X "
           "(add it to the manifest as a function entry)", target, va);
  }
  if (target - kThunkBase < kThunkRegionSize) {
    hle::CallThunk(c, target);
    return;
  }
  FatalF("indirect branch to %08X from %08X: not code", target, va);
}

void Unimplemented(Ctx& c, uint32_t va, const char* what) {
  (void)c;
  FatalF("unimplemented instruction at %08X: %s", va, what);
}

void BadJump(Ctx& c, uint32_t va, uint32_t target) {
  (void)c;
  FatalF("branch at %08X to %08X outside lifted code", va, target);
}

void Unreachable(Ctx& c, uint32_t va) {
  (void)c;
  FatalF("returned from a no-return call at %08X", va);
}

void Trap(Ctx& c, uint32_t va) {
  (void)c;
  // int3 in shipped code is an assert/DebugBreak. Raising
  // EXCEPTION_BREAKPOINT through SEH is M2 work; stop loudly for now.
  FatalF("breakpoint/trap instruction at %08X", va);
}

void RaiseDivideError(Ctx& c, uint32_t va) {
  (void)c;
  FatalF("integer divide error (#DE) at %08X", va);
}

void Cpuid(Ctx& c) {
  // Report a plain Pentium III-class CPU: the binary has no SSE code, and
  // a stable answer keeps behaviour identical on every host.
  switch (c.eax) {
    case 0:
      c.eax = 2;
      c.ebx = 0x756E6547;  // "GenuineIntel"
      c.edx = 0x49656E69;
      c.ecx = 0x6C65746E;
      break;
    case 1:
      c.eax = 0x00000683;  // family 6, model 8, stepping 3
      c.ebx = 0;
      c.ecx = 0;
      c.edx = 0x0183F9FF;  // fpu tsc cx8 cmov mmx fxsr (no sse)
      break;
    default:
      c.eax = c.ebx = c.ecx = c.edx = 0;
      break;
  }
}

uint64_t Rdtsc(Ctx& c) {
  (void)c;
  using namespace std::chrono;
  return uint64_t(
      duration_cast<nanoseconds>(steady_clock::now().time_since_epoch())
          .count());
}

void Probe(Ctx& c, uint32_t va, const char* name) {
  std::fprintf(stderr, "probe: %s (%08X) entered, args %08X %08X %08X %08X\n",
               name, va, Rd32(c.esp + 4), Rd32(c.esp + 8), Rd32(c.esp + 12),
               Rd32(c.esp + 16));
}

void X87Check(Ctx& c, uint32_t va) {
  if (c.fpu.unmasked_fault)
    FatalF("unmasked x87 exception at %08X (sw=%04X cw=%04X)", va,
           x87::StatusWord(c.fpu), x87::ControlWord(c.fpu));
}

}  // namespace ds1
