#pragma once
// Process-level state owned by the kernel32 HLE: heaps, handles, the
// command line and environment, and guest <-> host path mapping.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "runtime/guest_alloc.h"

namespace ds1 {

inline constexpr uint32_t kGuestPid = 0x1000;
// Where the game believes it is installed.
inline constexpr const char* kGuestGameDir = "C:\\Dungeon Siege";

void InitKernel32State();

uint32_t HeapAllocProcess(uint32_t size, bool zero);
GuestHeap* FindHeap(uint32_t handle);

// Kernel object handles. Values are small multiples of 4, as on Windows.
struct KObject {
  enum class Type { File, Event, Mutex, Semaphore, Thread, Mapping, Find,
                    StdIo };
  explicit KObject(Type t) : type(t) {}
  virtual ~KObject() = default;
  Type type;
};

uint32_t NewHandle(std::shared_ptr<KObject> obj);
int FileFd(KObject* o);  // host fd of a File/StdIo object, or -1
std::shared_ptr<KObject> GetHandle(uint32_t h);
bool CloseHandleValue(uint32_t h);

// Guest path ("C:\Dungeon Siege\Resources\Logic.dsres", or relative to the
// guest current directory) to a host path, resolving case-insensitively
// against what exists on disk. Paths outside known roots map below the
// per-user data directory.
std::string HostPath(const std::string& guest_path);
std::string GuestCurrentDir();

// Thrown by ExitThread to unwind a guest thread's host stack back to its
// start routine; caught in process.cpp.
struct GuestThreadExit {
  uint32_t code;
};
void OnGuestThreadFinished(uint32_t tid, uint32_t code);

struct Thread;
Thread* FindThread(uint32_t tid);
uint32_t StartHostThread(Thread* t, uint32_t entry, std::vector<uint32_t> args,
                         bool join);

}  // namespace ds1
