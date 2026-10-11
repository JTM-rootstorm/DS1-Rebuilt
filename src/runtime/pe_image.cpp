#include "runtime/pe_image.h"

#include <cstring>
#include <fstream>
#include <iterator>

namespace ds1 {

namespace {

constexpr uint32_t kScnMemExecute = 0x20000000;
constexpr uint32_t kScnMemWrite = 0x80000000;

template <typename T>
T Rd(const std::vector<uint8_t>& d, size_t off) {
  T v{};
  if (off + sizeof(T) <= d.size()) std::memcpy(&v, d.data() + off, sizeof(T));
  return v;
}

Prot SectionProt(uint32_t ch) {
  // Guest code is lifted, never executed, so .text only needs to be
  // readable (jump tables and constants live there).
  return (ch & kScnMemWrite) ? Prot::RW : Prot::R;
}

std::string CStr(const GuestMemory& mem, gaddr va) {
  return std::string(mem.ptr<const char>(va));
}

}  // namespace

bool PeImage::Load(const std::string& path, GuestMemory& mem,
                   std::string* err) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    *err = "cannot open " + path;
    return false;
  }
  std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());

  if (Rd<uint16_t>(d, 0) != 0x5A4D) {
    *err = "missing MZ header";
    return false;
  }
  uint32_t pe = Rd<uint32_t>(d, 0x3C);
  if (Rd<uint32_t>(d, pe) != 0x00004550 || Rd<uint16_t>(d, pe + 4) != 0x14C) {
    *err = "not a PE32 i386 image";
    return false;
  }
  uint16_t nsec = Rd<uint16_t>(d, pe + 6);
  characteristics = Rd<uint16_t>(d, pe + 22);
  uint16_t opt_size = Rd<uint16_t>(d, pe + 20);
  size_t oh = pe + 24;
  if (Rd<uint16_t>(d, oh) != 0x10B) {
    *err = "optional header is not PE32";
    return false;
  }
  entry = Rd<uint32_t>(d, oh + 16);
  image_base = Rd<uint32_t>(d, oh + 28);
  image_size = Rd<uint32_t>(d, oh + 56);
  uint32_t headers_size = Rd<uint32_t>(d, oh + 60);
  stack_reserve = Rd<uint32_t>(d, oh + 72);
  stack_commit = Rd<uint32_t>(d, oh + 76);
  heap_reserve = Rd<uint32_t>(d, oh + 80);
  heap_commit = Rd<uint32_t>(d, oh + 84);
  entry += image_base;
  auto dir = [&](int i) {
    return std::pair<uint32_t, uint32_t>{Rd<uint32_t>(d, oh + 96 + i * 8),
                                         Rd<uint32_t>(d, oh + 100 + i * 8)};
  };

  // Map everything RW first so imports can be patched, then tighten.
  if (!mem.Commit(image_base, image_size, Prot::RW)) {
    *err = "cannot commit image range";
    return false;
  }
  std::memcpy(mem.ptr(image_base), d.data(),
              std::min<size_t>(headers_size, d.size()));

  size_t sh = oh + opt_size;
  for (uint16_t i = 0; i < nsec; i++) {
    size_t o = sh + i * 40;
    char name[9] = {};
    std::memcpy(name, d.data() + o, 8);
    uint32_t vsize = Rd<uint32_t>(d, o + 8);
    uint32_t va = Rd<uint32_t>(d, o + 12);
    uint32_t rsize = Rd<uint32_t>(d, o + 16);
    uint32_t rptr = Rd<uint32_t>(d, o + 20);
    uint32_t ch = Rd<uint32_t>(d, o + 36);
    uint32_t n = std::min(vsize ? vsize : rsize, rsize);
    if (rptr + n > d.size()) {
      *err = std::string("section ") + name + " runs past end of file";
      return false;
    }
    std::memcpy(mem.ptr(image_base + va), d.data() + rptr, n);
    sections.push_back({name, image_base + va, vsize, ch});
  }

  // Imports: point every IAT slot at its own thunk address.
  if (!mem.Commit(kThunkBase, kThunkRegionSize, Prot::R)) {
    *err = "cannot commit thunk region";
    return false;
  }
  auto [imp_rva, imp_size] = dir(1);
  for (gaddr desc = image_base + imp_rva; imp_size; desc += 20) {
    uint32_t ilt = mem.read<uint32_t>(desc);
    uint32_t name_rva = mem.read<uint32_t>(desc + 12);
    uint32_t iat = mem.read<uint32_t>(desc + 16);
    if (!name_rva) break;
    std::string dll = CStr(mem, image_base + name_rva);
    gaddr lookup = image_base + (ilt ? ilt : iat);
    for (gaddr slot = image_base + iat;; slot += 4, lookup += 4) {
      uint32_t v = mem.read<uint32_t>(lookup);
      if (!v) break;
      std::string sym = (v & 0x80000000)
                            ? "#" + std::to_string(v & 0xFFFF)
                            : CStr(mem, image_base + v + 2);
      gaddr thunk = kThunkBase + uint32_t(imports.size()) * kThunkStride;
      if (thunk >= kThunkBase + kThunkRegionSize) {
        *err = "thunk region exhausted";
        return false;
      }
      mem.write<uint32_t>(slot, thunk);
      imports.push_back({dll, sym, slot, thunk});
    }
  }

  auto [exp_rva, exp_size] = dir(0);
  if (exp_size) {
    gaddr e = image_base + exp_rva;
    uint32_t ord_base = mem.read<uint32_t>(e + 16);
    uint32_t nfuncs = mem.read<uint32_t>(e + 20);
    uint32_t nnames = mem.read<uint32_t>(e + 24);
    gaddr funcs = image_base + mem.read<uint32_t>(e + 28);
    gaddr names = image_base + mem.read<uint32_t>(e + 32);
    gaddr ords = image_base + mem.read<uint32_t>(e + 36);
    std::vector<std::string> by_index(nfuncs);
    for (uint32_t i = 0; i < nnames; i++) {
      uint16_t idx = mem.read<uint16_t>(ords + i * 2);
      if (idx < nfuncs)
        by_index[idx] = CStr(mem, image_base + mem.read<uint32_t>(names + i * 4));
    }
    for (uint32_t i = 0; i < nfuncs; i++) {
      uint32_t rva = mem.read<uint32_t>(funcs + i * 4);
      if (rva) exports.push_back({image_base + rva, ord_base + i, by_index[i]});
    }
  }

  auto [tls_rva, tls_size] = dir(9);
  if (tls_size) {
    gaddr t = image_base + tls_rva;
    tls.raw_start = mem.read<uint32_t>(t);
    tls.raw_end = mem.read<uint32_t>(t + 4);
    tls.index_va = mem.read<uint32_t>(t + 8);
    tls.callbacks_va = mem.read<uint32_t>(t + 12);
    tls.zero_fill = mem.read<uint32_t>(t + 16);
    has_tls = true;
  }

  mem.Protect(image_base, headers_size, Prot::R);
  for (const auto& s : sections)
    mem.Protect(s.va, s.vsize, SectionProt(s.characteristics));
  return true;
}

const PeImport* PeImage::ImportByThunk(gaddr thunk) const {
  if (thunk < kThunkBase || (thunk - kThunkBase) % kThunkStride) return nullptr;
  size_t i = (thunk - kThunkBase) / kThunkStride;
  return i < imports.size() ? &imports[i] : nullptr;
}

}  // namespace ds1
