#pragma once
// Process-wide runtime state and the services HLE code builds on: guest
// memory access, calling back into lifted code, guest threads.

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

#include "runtime/ctx.h"
#include "runtime/guest_alloc.h"
#include "runtime/guest_memory.h"
#include "runtime/pe_image.h"

namespace ds1 {

// Processors reported to the game (PEB, GetSystemInfo): never 1, see
// FINDINGS.md "New-game freeze".
uint32_t GuestProcessorCount();

// Fixed guest layout (see ARCHITECTURE.md). TEBs grow down from kTebTop.
inline constexpr uint32_t kPebVa = 0x7FFDF000;
inline constexpr uint32_t kTebTop = 0x7FFDE000;
inline constexpr uint32_t kTebStride = 0x1000;
// Return address pushed when the host calls into guest code; reaching it
// means the guest function returned to us.
inline constexpr uint32_t kReturnToHost = kThunkBase + kThunkRegionSize - 0x10;

struct Process {
  GuestMemory mem;
  PeImage img;
  GuestAllocator vm;     // VirtualAlloc-style regions
  std::string game_dir;  // host path of the game install, no trailing '/'
  std::string game_args; // appended to the guest command line
  uint32_t process_heap = 0;
  uint32_t cmdline_a = 0, cmdline_w = 0;
  uint32_t environ_a = 0, environ_w = 0;
  std::atomic<uint32_t> next_tid{0x100};
};

extern Process* g_proc;

struct Thread {
  Ctx ctx;
  uint32_t tid = 0;
  uint32_t teb = 0;
  uint32_t stack_lo = 0, stack_hi = 0;
};

// Per-host-thread pointer to the guest thread running on it.
Thread* CurrentThread();

// Guest memory helpers for HLE code.
inline uint8_t* GPtr(uint32_t va) { return g_proc->mem.ptr(va); }
inline uint32_t Rd32(uint32_t va) { return g_proc->mem.read<uint32_t>(va); }
inline uint16_t Rd16(uint32_t va) { return g_proc->mem.read<uint16_t>(va); }
inline void Wr32(uint32_t va, uint32_t v) { g_proc->mem.write<uint32_t>(va, v); }
inline void Wr16(uint32_t va, uint16_t v) { g_proc->mem.write<uint16_t>(va, v); }
inline void Wr8(uint32_t va, uint8_t v) { g_proc->mem.write<uint8_t>(va, v); }
std::string GStr(uint32_t va);                  // NUL-terminated char*
std::u16string GWStr(uint32_t va);              // NUL-terminated WCHAR*
void PutStr(uint32_t va, const std::string& s); // writes s + NUL

// Calls guest function `va` with stdcall/cdecl arguments from the host
// (WndProc, enumeration callbacks, thread start routines). Returns eax.
// Handles both conventions: the stack pointer is restored afterwards.
uint32_t CallGuest(Ctx& c, uint32_t va, std::initializer_list<uint32_t> args);

// Sets up the process (arena, image, PEB, heaps, command line) and runs
// the guest entry point on a new guest thread. Returns the exit code.
int RunProcess(const std::string& exe_path, const std::string& game_dir,
               const std::string& game_args = {});

// Creates the guest-side structures of a thread (TEB, TLS, stack) and
// initialises its Ctx. Does not start it.
Thread* CreateGuestThread(uint32_t stack_size);

// Per-user data directory: $XDG_DATA_HOME/ds1, else ~/.local/share/ds1
// (settings, saves, the guest's C: drive, crash and hang reports).
std::string DataDir();
// Creates `path` and any missing parents (mkdir -p).
void MakeDirs(const std::string& path);
// A new report file name, e.g. ReportPath("crash") is
// DataDir()/crashes/crash-<time>-<pid>.txt; creates the directory.
std::string ReportPath(const char* kind);

// Runs host function fn on a new detached host thread bound to guest thread
// t (from CreateGuestThread), so fn may CallGuest(t->ctx, ...).
void StartServiceThread(Thread* t, std::function<void()> fn);

}  // namespace ds1
