#pragma once
// Turns one discovered Function into C++ source. The output uses only the
// helpers in src/runtime/lifted.h and calls other lifted functions by
// name (f_XXXXXXXX), so files can be compiled independently.

#include <cstdint>
#include <map>
#include <set>
#include <string>

#include "function.h"

namespace lifter {

struct EmitStats {
  uint64_t insns = 0;
  std::map<std::string, uint64_t> unimplemented;  // mnemonic -> count
};

std::string FnName(uint32_t va);

class Emitter {
 public:
  Emitter(const Program& p, const Decoder& dec) : p_(p), dec_(dec) {}

  // Appends the function definition to *out. Every function it references
  // (calls, tail calls) is added to *refs for forward declarations.
  void EmitFunction(const Function& f, std::string* out,
                    std::set<uint32_t>* refs, EmitStats* stats);

  // Straight-line x87 code becomes guarded runs (on by default).
  void set_x87_runs(bool on) { x87_runs_ = on; }

 private:
  std::map<uint32_t, uint32_t> FindX87Runs(const Function& f,
                                           const std::set<uint32_t>& labels) const;
  void EmitX87Run(const Function& f, std::map<uint32_t, Insn>::const_iterator first,
                  std::map<uint32_t, Insn>::const_iterator last, std::string* out,
                  std::set<uint32_t>* refs, EmitStats* stats) const;
  bool x87_runs_ = true;
  const Program& p_;
  const Decoder& dec_;
};

}  // namespace lifter
