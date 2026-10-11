#include "runtime/guest_memory.h"

#include <sys/mman.h>

#include <cstdio>

namespace ds1 {

namespace {

// Fixed host placement keeps base() constant across runs, which makes guest
// pointers in logs and core dumps easy to read. Falls back to anywhere.
constexpr uintptr_t kPreferredBase = 0x100000000ull;

int ToPosix(Prot p) {
  int r = 0;
  if (static_cast<int>(p) & static_cast<int>(Prot::R)) r |= PROT_READ;
  if (static_cast<int>(p) & static_cast<int>(Prot::W)) r |= PROT_WRITE;
  // Guest code is never executed directly; X only documents intent.
  return r;
}

void PageRange(gaddr va, uint32_t size, uint64_t* start, uint64_t* len) {
  uint64_t s = va & ~uint64_t(GuestMemory::kPageSize - 1);
  uint64_t e = (uint64_t(va) + size + GuestMemory::kPageSize - 1) &
               ~uint64_t(GuestMemory::kPageSize - 1);
  *start = s;
  *len = e - s;
}

}  // namespace

bool GuestMemory::Reserve() {
  int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
  void* p = mmap(reinterpret_cast<void*>(kPreferredBase), kSize, PROT_NONE,
                 flags | MAP_FIXED_NOREPLACE, -1, 0);
  if (p == MAP_FAILED) {
    p = mmap(nullptr, kSize, PROT_NONE, flags, -1, 0);
    if (p == MAP_FAILED) {
      std::perror("GuestMemory::Reserve mmap");
      return false;
    }
  }
  base_ = static_cast<uint8_t*>(p);
  return true;
}

void GuestMemory::Release() {
  if (base_) munmap(base_, kSize);
  base_ = nullptr;
}

bool GuestMemory::Commit(gaddr va, uint32_t size, Prot prot) {
  // Pages start PROT_NONE and are zero-filled by the kernel on first touch,
  // so committing is just granting access.
  return Protect(va, size, prot);
}

bool GuestMemory::Protect(gaddr va, uint32_t size, Prot prot) {
  uint64_t s, len;
  PageRange(va, size, &s, &len);
  if (mprotect(base_ + s, len, ToPosix(prot)) != 0) {
    std::perror("GuestMemory::Protect mprotect");
    return false;
  }
  return true;
}

void GuestMemory::Decommit(gaddr va, uint32_t size) {
  uint64_t s, len;
  PageRange(va, size, &s, &len);
  madvise(base_ + s, len, MADV_DONTNEED);
  mprotect(base_ + s, len, PROT_NONE);
}

}  // namespace ds1
