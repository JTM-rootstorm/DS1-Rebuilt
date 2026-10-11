#pragma once
// The guest (32-bit x86) address space lives in one 4 GiB host reservation.
// Guest address A is host address base() + A. Pages are reserved PROT_NONE
// and committed on demand, so stray guest accesses fault instead of
// silently hitting host memory.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ds1 {

using gaddr = uint32_t;

enum class Prot : int { None = 0, R = 1, W = 2, X = 4, RW = 3, RX = 5, RWX = 7 };

class GuestMemory {
 public:
  static constexpr uint64_t kSize = 1ull << 32;
  static constexpr uint32_t kPageSize = 0x1000;

  bool Reserve();
  void Release();

  // Commit [va, va+size) rounded out to pages. Returns false on failure.
  bool Commit(gaddr va, uint32_t size, Prot prot);
  bool Protect(gaddr va, uint32_t size, Prot prot);
  void Decommit(gaddr va, uint32_t size);

  uint8_t* base() const { return base_; }

  template <typename T = uint8_t>
  T* ptr(gaddr va) const {
    return reinterpret_cast<T*>(base_ + va);
  }
  gaddr to_guest(const void* host) const {
    return static_cast<gaddr>(static_cast<const uint8_t*>(host) - base_);
  }

  // x86 is little-endian like the host, so loads/stores are plain copies.
  // memcpy keeps unaligned guest accesses well-defined.
  template <typename T>
  T read(gaddr va) const {
    T v;
    std::memcpy(&v, base_ + va, sizeof(T));
    return v;
  }
  template <typename T>
  void write(gaddr va, T v) const {
    std::memcpy(base_ + va, &v, sizeof(T));
  }

 private:
  uint8_t* base_ = nullptr;
};

}  // namespace ds1
