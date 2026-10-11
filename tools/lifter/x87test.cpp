// --x87-test: generates the inputs of tests/lift_x87_test.cpp. Every
// distinct x87 instruction form in the EXE is lifted on its own, next to
// its raw bytes, so the test can run both from the same FPU state and
// compare. Memory operands are rewritten to [eax] (ModRM mod=0 rm=0): the
// same bytes address [rax] when executed natively in 64-bit mode.

#include "x87test.h"

#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "emit.h"
#include "function.h"

namespace lifter {
namespace {

bool IsSegOrAddrPrefix(uint8_t b) {
  return b == 0x26 || b == 0x2E || b == 0x36 || b == 0x3E || b == 0x64 ||
         b == 0x65 || b == 0x67;
}

struct Form {
  std::vector<uint8_t> bytes;
  std::string text;
  int mem_bytes = 0;
  uint32_t example_va = 0;
};

}  // namespace

int WriteX87Test(const Program& p, const Decoder& dec, const std::string& out,
                 const char* prologue) {
  // Collect forms from every known function.
  std::map<std::vector<uint8_t>, Form> forms;
  for (uint32_t e : p.entries) {
    Function f;
    BuildFunction(p, dec, e, &f);
    for (const auto& [va, in] : f.insns) {
      if (!in.ok || in.d.meta.category != ZYDIS_CATEGORY_X87_ALU) continue;
      const auto m = in.d.mnemonic;
      // Environment images differ between 32- and 64-bit mode (pointer
      // fields), so they are not comparable natively.
      if (m == ZYDIS_MNEMONIC_FNSTENV || m == ZYDIS_MNEMONIC_FLDENV ||
          m == ZYDIS_MNEMONIC_FNSAVE || m == ZYDIS_MNEMONIC_FRSTOR)
        continue;
      const uint8_t* raw = p.Bytes(va);
      std::vector<uint8_t> b;
      int mem_bytes = 0;
      bool has_modrm = in.d.attributes & ZYDIS_ATTRIB_HAS_MODRM;
      int modrm_off = has_modrm ? in.d.raw.modrm.offset : in.d.length;
      for (int i = 0; i < modrm_off; i++)
        if (i >= in.d.raw.prefix_count || !IsSegOrAddrPrefix(raw[i]))
          b.push_back(raw[i]);
      if (has_modrm) {
        if (in.d.raw.modrm.mod == 3) {
          b.push_back(raw[modrm_off]);
        } else {
          b.push_back(uint8_t((in.d.raw.modrm.reg << 3) | 0));  // [eax]
          for (int i = 0; i < in.d.operand_count_visible; i++)
            if (in.ops[i].type == ZYDIS_OPERAND_TYPE_MEMORY)
              mem_bytes = in.ops[i].size / 8;
        }
      }
      if (forms.count(b)) continue;
      Form fm;
      fm.bytes = b;
      fm.text = dec.Format(in);
      fm.mem_bytes = mem_bytes;
      fm.example_va = va;
      forms[b] = fm;
    }
  }

  // A scratch program holding each form at its own 16-byte slot, + ret.
  Program q;
  if (!q.mem.Reserve()) return 1;
  const uint32_t base = 0x00401000;
  uint32_t size = uint32_t(forms.size()) * 16 + 0x1000;
  q.mem.Commit(base, size, ds1::Prot::RWX);
  q.text_lo = base;
  q.text_hi = base + size;
  std::vector<std::pair<uint32_t, const Form*>> slots;
  uint32_t va = base;
  for (const auto& [k, fm] : forms) {
    std::vector<uint8_t> code = fm.bytes;
    code.push_back(0xC3);  // ret
    for (size_t i = 0; i < code.size(); i++) q.mem.write<uint8_t>(va + uint32_t(i), code[i]);
    q.entries.insert(va);
    slots.push_back({va, &fm});
    va += 16;
  }

  Emitter em(q, dec);
  std::string body;
  std::set<uint32_t> refs;
  EmitStats st;
  for (const auto& [sva, fm] : slots) {
    Function f;
    BuildFunction(q, dec, sva, &f);
    em.EmitFunction(f, &body, &refs, &st);
  }

  std::string o = prologue;
  for (const auto& [sva, fm] : slots) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "void f_%08X(::ds1::Ctx& c);\n", sva);
    o += buf;
  }
  o += body;
  o += "}  // namespace ds1::gen\n\n#include \"lift_x87_forms.h\"\n";
  o += "extern const X87Form kX87Forms[] = {\n";
  for (const auto& [sva, fm] : slots) {
    std::string bytes;
    for (uint8_t x : fm->bytes) {
      char h[8];
      std::snprintf(h, sizeof h, "0x%02X,", x);
      bytes += h;
    }
    std::string text = fm->text;
    for (char& ch : text)
      if (ch == '"' || ch == '\\') ch = '\'';
    char buf[512];
    std::snprintf(buf, sizeof buf,
                  "  {&ds1::gen::f_%08X, {%s}, %zu, %d, \"%s\", 0x%08X},\n", sva,
                  bytes.c_str(), fm->bytes.size(), fm->mem_bytes, text.c_str(),
                  fm->example_va);
    o += buf;
  }
  o += "};\nextern const int kX87FormCount = " + std::to_string(slots.size()) + ";\n";

  FILE* fp = std::fopen(out.c_str(), "wb");
  if (!fp) return 1;
  std::fwrite(o.data(), 1, o.size(), fp);
  std::fclose(fp);
  std::printf("x87 forms: %zu -> %s\n", slots.size(), out.c_str());
  return 0;
}

}  // namespace lifter
