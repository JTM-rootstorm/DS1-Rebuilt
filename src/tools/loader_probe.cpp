// Maps DungeonSiege.exe into a guest arena and prints what the runtime will
// have to provide. First sanity check for the loader; also the quickest way
// to see the full import surface the HLE layer must cover.
//
//   ds1_loader_probe "<game dir>/DungeonSiege.exe" [--imports]

#include <cstdio>
#include <cstring>
#include <map>

#include "runtime/guest_memory.h"
#include "runtime/pe_image.h"

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s DungeonSiege.exe [--imports]\n", argv[0]);
    return 2;
  }
  bool list_imports = argc > 2 && std::strcmp(argv[2], "--imports") == 0;

  ds1::GuestMemory mem;
  if (!mem.Reserve()) return 1;
  ds1::PeImage img;
  std::string err;
  if (!img.Load(argv[1], mem, &err)) {
    std::fprintf(stderr, "load failed: %s\n", err.c_str());
    return 1;
  }

  std::printf("guest arena at host %p\n", static_cast<void*>(mem.base()));
  std::printf("image %#010x..%#010x entry %#010x\n", img.image_base,
              img.image_base + img.image_size, img.entry);
  std::printf("large-address-aware: %s\n",
              (img.characteristics & 0x20) ? "yes" : "no");
  std::printf("stack reserve/commit %#x/%#x heap %#x/%#x\n",
              img.stack_reserve, img.stack_commit, img.heap_reserve,
              img.heap_commit);
  for (const auto& s : img.sections)
    std::printf("  %-8s %#010x +%#x\n", s.name.c_str(), s.va, s.vsize);

  // FuBi walks the export directory of the in-memory image, so the headers
  // must be mapped where GetModuleHandle(NULL) points.
  std::printf("headers mapped: %s\n",
              mem.read<uint16_t>(img.image_base) == 0x5A4D ? "MZ ok" : "BAD");

  if (img.has_tls) {
    int ncb = 0;
    for (ds1::gaddr cb = img.tls.callbacks_va; cb && mem.read<uint32_t>(cb);
         cb += 4)
      ncb++;
    std::printf("tls template %#010x..%#010x (+%#x zero) index@%#010x "
                "callbacks=%d\n",
                img.tls.raw_start, img.tls.raw_end, img.tls.zero_fill,
                img.tls.index_va, ncb);
  }

  std::map<std::string, int> per_dll;
  for (const auto& i : img.imports) per_dll[i.dll]++;
  std::printf("imports: %zu across %zu dlls\n", img.imports.size(),
              per_dll.size());
  for (const auto& [dll, n] : per_dll) std::printf("  %-14s %d\n", dll.c_str(), n);

  size_t in_text = 0;
  const auto& text = img.sections.front();
  for (const auto& e : img.exports)
    if (e.va >= text.va && e.va < text.va + text.vsize) in_text++;
  std::printf("exports: %zu (%zu in %s)\n", img.exports.size(), in_text,
              text.name.c_str());

  if (list_imports)
    for (const auto& i : img.imports)
      std::printf("%#010x -> thunk %#010x  %s!%s\n", i.iat_va, i.thunk_va,
                  i.dll.c_str(), i.name.c_str());
  return 0;
}
