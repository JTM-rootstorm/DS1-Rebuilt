// Process and thread bring-up: arena, image, PEB/TEB, TLS, stacks, and the
// host threads that run guest code.

#include <pthread.h>
#include <sys/mman.h>
#include <ucontext.h>

#include <thread>
#include <algorithm>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>

#include "runtime/hle/hle.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/lifted.h"
#include "runtime/log.h"
#include "runtime/runtime.h"
#include "runtime/watchdog.h"

namespace ds1 {

void InitDispatch();
void InstallSignalStack();

Process* g_proc = nullptr;

namespace {

thread_local Thread* t_current = nullptr;
std::mutex g_threads_mu;
std::map<uint32_t, std::unique_ptr<Thread>> g_threads;  // by tid
uint32_t g_next_teb = kTebTop;

// Host stack for each guest thread. Guest call depth maps 1:1 onto host
// call depth, and lifted frames are larger than the originals, so be
// generous; the memory is only touched as needed.
constexpr size_t kHostStack = 256u << 20;

constexpr uint32_t kTlsArraySlots = 64;

}  // namespace

Thread* CurrentThread() { return t_current; }

uint32_t GuestProcessorCount() {
  unsigned n = std::thread::hardware_concurrency();
  return std::clamp<unsigned>(n, 2, 8);
}

std::string GStr(uint32_t va) {
  if (!va) return {};
  return std::string(g_proc->mem.ptr<const char>(va));
}

std::u16string GWStr(uint32_t va) {
  std::u16string s;
  if (!va) return s;
  for (;; va += 2) {
    char16_t ch = Rd16(va);
    if (!ch) break;
    s.push_back(ch);
  }
  return s;
}

void PutStr(uint32_t va, const std::string& s) {
  std::memcpy(GPtr(va), s.c_str(), s.size() + 1);
}

uint32_t CallGuest(Ctx& c, uint32_t va, std::initializer_list<uint32_t> args) {
  uint32_t saved = c.esp;
  // Guest code runs here: faults are guest access violations again.
  struct DepthGuard {
    int saved = hle::t_hle_depth;
    DepthGuard() { hle::t_hle_depth = 0; }
    ~DepthGuard() { hle::t_hle_depth = saved; }
  } guard;
  for (auto it = std::rbegin(args); it != std::rend(args); ++it) {
    c.esp -= 4;
    Wr32(c.esp, *it);
  }
  c.esp -= 4;
  Wr32(c.esp, kReturnToHost);
  Dispatch(c, va, kReturnToHost);
  c.esp = saved;  // stdcall callees popped their args, cdecl didn't
  return c.eax;
}

Thread* CreateGuestThread(uint32_t stack_size) {
  auto t = std::make_unique<Thread>();
  Process& p = *g_proc;

  stack_size = std::max<uint32_t>(stack_size, 0x100000);
  uint32_t stack = p.vm.Reserve(0, stack_size);
  if (!stack) FatalF("cannot reserve a %u byte guest stack", stack_size);
  p.vm.Commit(stack, stack_size, Prot::RW);
  t->stack_lo = stack;
  t->stack_hi = stack + stack_size;

  {
    std::lock_guard<std::mutex> l(g_threads_mu);
    g_next_teb -= kTebStride;
    t->teb = g_next_teb;
  }
  t->tid = p.next_tid.fetch_add(4);
  p.mem.Commit(t->teb, kTebStride, Prot::RW);
  uint32_t teb = t->teb;
  Wr32(teb + 0x00, 0xFFFFFFFF);  // SEH chain: empty
  Wr32(teb + 0x04, t->stack_hi);
  Wr32(teb + 0x08, t->stack_lo);
  Wr32(teb + 0x18, teb);         // Self
  Wr32(teb + 0x20, kGuestPid);
  Wr32(teb + 0x24, t->tid);
  Wr32(teb + 0x30, kPebVa);

  // Static TLS of the image (__declspec(thread)): a per-thread copy of the
  // template, found through TEB+0x2C[tls_index].
  uint32_t tls_array = HeapAllocProcess(kTlsArraySlots * 4, true);
  Wr32(teb + 0x2C, tls_array);
  if (p.img.has_tls) {
    const PeTls& tls = p.img.tls;
    uint32_t raw = tls.raw_end - tls.raw_start;
    uint32_t block = HeapAllocProcess(raw + tls.zero_fill + 16, true);
    std::memcpy(GPtr(block), GPtr(tls.raw_start), raw);
    Wr32(tls_array + 4 * Rd32(tls.index_va), block);
  }

  Ctx& c = t->ctx;
  std::memset(&c, 0, sizeof c);
  c.esp = t->stack_hi - 16;
  c.fs_base = teb;
  c.thread = t.get();
  x87::Init(c.fpu);
  x87::SetControlWord(c.fpu, 0x027F);  // Windows default for new threads

  Thread* raw_t = t.get();
  std::lock_guard<std::mutex> l(g_threads_mu);
  g_threads[raw_t->tid] = std::move(t);
  return raw_t;
}

Thread* FindThread(uint32_t tid) {
  std::lock_guard<std::mutex> l(g_threads_mu);
  auto it = g_threads.find(tid);
  return it == g_threads.end() ? nullptr : it->second.get();
}

struct HostThreadStart {
  Thread* t;
  uint32_t entry;
  std::vector<uint32_t> args;
  uint32_t exit_code = 0;
};

static void* HostThreadMain(void* arg) {
  auto* s = static_cast<HostThreadStart*>(arg);
  t_current = s->t;
  InstallSignalStack();
  RegisterGuestHostThread("guest", s->t->tid);
  Ctx& c = s->t->ctx;
  uint32_t r = 0;
  try {
    switch (s->args.size()) {
      case 0: r = CallGuest(c, s->entry, {}); break;
      case 1: r = CallGuest(c, s->entry, {s->args[0]}); break;
      default: FatalF("thread start with %zu args", s->args.size());
    }
  } catch (const GuestThreadExit& e) {
    r = e.code;
  }
  s->exit_code = r;
  OnGuestThreadFinished(s->t->tid, r);
  return nullptr;
}

// Runs `entry(args)` on a new host thread bound to guest thread t. If
// `join`, waits and returns the guest return value.
uint32_t StartHostThread(Thread* t, uint32_t entry,
                         std::vector<uint32_t> args, bool join) {
  auto* s = new HostThreadStart{t, entry, std::move(args)};
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, kHostStack);
  pthread_t th;
  if (pthread_create(&th, &attr, HostThreadMain, s) != 0)
    FatalF("pthread_create failed");
  pthread_attr_destroy(&attr);
  if (!join) {
    pthread_detach(th);
    return 0;
  }
  pthread_join(th, nullptr);
  uint32_t r = s->exit_code;
  delete s;
  return r;
}

// A host-driven thread that may call into guest code (Miles' service
// thread delivering callbacks): bound to guest thread t, with the same big
// host stack and signal stack as other guest threads. Outside CallGuest it
// counts as HLE code, so a host fault there is a runtime bug.
struct ServiceStart {
  Thread* t;
  std::function<void()> fn;
};

static void* ServiceThreadMain(void* arg) {
  std::unique_ptr<ServiceStart> s(static_cast<ServiceStart*>(arg));
  t_current = s->t;
  InstallSignalStack();
  RegisterGuestHostThread("service", s->t->tid);
  hle::t_hle_depth = 1;
  s->fn();
  return nullptr;
}

void StartServiceThread(Thread* t, std::function<void()> fn) {
  auto* s = new ServiceStart{t, std::move(fn)};
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, kHostStack);
  pthread_t th;
  if (pthread_create(&th, &attr, ServiceThreadMain, s) != 0)
    FatalF("pthread_create failed");
  pthread_attr_destroy(&attr);
  pthread_detach(th);
}

// The guest main thread runs on the process main thread (SDL, and Cocoa on
// macOS, require UI calls there) but on a big private stack, switched to
// with makecontext/swapcontext.
namespace {
ucontext_t g_host_main_ctx, g_guest_main_ctx;
HostThreadStart* g_main_start = nullptr;

void GuestMainTrampoline() { HostThreadMain(g_main_start); }
}  // namespace

static uint32_t RunOnMainThread(Thread* t, uint32_t entry) {
  void* stack = mmap(nullptr, kHostStack, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (stack == MAP_FAILED) FatalF("cannot allocate the main guest stack");
  g_main_start = new HostThreadStart{t, entry, {}};
  getcontext(&g_guest_main_ctx);
  g_guest_main_ctx.uc_stack.ss_sp = stack;
  g_guest_main_ctx.uc_stack.ss_size = kHostStack;
  g_guest_main_ctx.uc_link = &g_host_main_ctx;  // resume here on return
  makecontext(&g_guest_main_ctx, GuestMainTrampoline, 0);
  swapcontext(&g_host_main_ctx, &g_guest_main_ctx);
  uint32_t r = g_main_start->exit_code;
  delete g_main_start;
  munmap(stack, kHostStack);
  return r;
}

int RunProcess(const std::string& exe_path, const std::string& game_dir,
               const std::string& game_args) {
  g_proc = new Process;
  Process& p = *g_proc;
  p.game_dir = game_dir;
  p.game_args = game_args;
  p.guest_dir = kGuestGameDir;
  StartWatchdog();
  if (!p.mem.Reserve()) FatalF("cannot reserve the guest arena");
  g_guest_base = p.mem.base();
  std::string err;
  if (!p.img.Load(exe_path, p.mem, &err)) FatalF("%s", err.c_str());

  p.vm.Init(&p.mem, 0x00010000, kThunkBase);
  p.vm.MarkReserved(p.img.image_base, p.img.image_size);
  p.vm.MarkReserved(hle::kModuleRegion, hle::kModuleRegionSize);

  // PEB: only the fields Win32 code commonly peeks at.
  p.mem.Commit(kPebVa, 0x1000, Prot::RW);
  Wr32(kPebVa + 0x08, p.img.image_base);
  Wr32(kPebVa + 0x64, GuestProcessorCount());  // NumberOfProcessors
  Wr32(kPebVa + 0xA4, 5);      // OSMajorVersion (Windows XP)
  Wr32(kPebVa + 0xA8, 1);      // OSMinorVersion
  Wr16(kPebVa + 0xAC, 2600);   // OSBuildNumber
  Wr32(kPebVa + 0xB0, 2);      // OSPlatformId: VER_PLATFORM_WIN32_NT

  // The image's TLS index: one module, so slot 0.
  if (p.img.has_tls) Wr32(p.img.tls.index_va, 0);

  InitDispatch();
  InitKernel32State();
  hle::BindImports();

  Thread* main = CreateGuestThread(p.img.stack_reserve);
  return int(RunOnMainThread(main, p.img.entry));
}

}  // namespace ds1
