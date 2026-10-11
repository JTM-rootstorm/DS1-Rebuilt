#pragma once
// Everything the lifter knows about the binary: the mapped image, function
// entries, names, no-return functions and switch tables. Inputs are the EXE
// itself plus the TSVs exported from Ghidra into symbols/.

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "runtime/guest_memory.h"
#include "runtime/pe_image.h"

namespace lifter {

struct Program {
  ds1::GuestMemory mem;
  ds1::PeImage img;
  uint32_t text_lo = 0, text_hi = 0;  // [lo, hi) of .text

  std::set<uint32_t> entries;                 // function entry points
  std::map<uint32_t, std::string> names;      // demangled or Ghidra names
  std::set<uint32_t> noreturn;                // functions that never return
  std::map<uint32_t, std::vector<uint32_t>> jumptables;  // jmp va -> targets
  std::set<uint32_t> noreturn_iat;            // IAT slots of ExitProcess etc.
  std::map<uint32_t, std::string> probes;     // manifest: probe on entry
  std::set<uint32_t> resume_sites;            // manifest: calls/jmps that
                                              // abandon the stack (SEH)

  bool Load(const std::string& exe, const std::string& symdir,
            std::string* err);

  // Applies lifter.manifest (entry/name/probe/resume lines).
  bool LoadManifest(const std::string& path, std::string* err);

  bool InText(uint32_t va) const { return va >= text_lo && va < text_hi; }
  const uint8_t* Bytes(uint32_t va) const { return mem.ptr<uint8_t>(va); }
  size_t BytesLeft(uint32_t va) const { return text_hi - va; }
};

}  // namespace lifter
