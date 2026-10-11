#include "emit.h"

#include <cstdarg>
#include <cstdio>

namespace lifter {

namespace {

std::string Fmt(const char* f, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, f);
  std::vsnprintf(buf, sizeof buf, f, ap);
  va_end(ap);
  return buf;
}

std::string Hex(uint32_t v) { return Fmt("0x%Xu", v); }

const char* Gpr32Name(ZydisRegister r) {
  switch (r) {
    case ZYDIS_REGISTER_EAX: return "eax";
    case ZYDIS_REGISTER_ECX: return "ecx";
    case ZYDIS_REGISTER_EDX: return "edx";
    case ZYDIS_REGISTER_EBX: return "ebx";
    case ZYDIS_REGISTER_ESP: return "esp";
    case ZYDIS_REGISTER_EBP: return "ebp";
    case ZYDIS_REGISTER_ESI: return "esi";
    case ZYDIS_REGISTER_EDI: return "edi";
    default: return nullptr;
  }
}

bool IsHi8(ZydisRegister r) {
  return r == ZYDIS_REGISTER_AH || r == ZYDIS_REGISTER_CH ||
         r == ZYDIS_REGISTER_DH || r == ZYDIS_REGISTER_BH;
}

// Segment register reads return the selectors a Win32 process sees.
const char* SegValue(ZydisRegister r) {
  switch (r) {
    case ZYDIS_REGISTER_CS: return "0x1Bu";
    case ZYDIS_REGISTER_DS:
    case ZYDIS_REGISTER_ES:
    case ZYDIS_REGISTER_SS: return "0x23u";
    case ZYDIS_REGISTER_FS: return "0x3Bu";
    case ZYDIS_REGISTER_GS: return "0x0u";
    default: return nullptr;
  }
}

int StIndex(ZydisRegister r) {
  if (r >= ZYDIS_REGISTER_ST0 && r <= ZYDIS_REGISTER_ST7)
    return r - ZYDIS_REGISTER_ST0;
  return -1;
}

const char* CondExpr(ZydisMnemonic m) {
  switch (m) {
    case ZYDIS_MNEMONIC_JO: case ZYDIS_MNEMONIC_SETO: case ZYDIS_MNEMONIC_CMOVO:
      return "F.of";
    case ZYDIS_MNEMONIC_JNO: case ZYDIS_MNEMONIC_SETNO:
    case ZYDIS_MNEMONIC_CMOVNO:
      return "!F.of";
    case ZYDIS_MNEMONIC_JB: case ZYDIS_MNEMONIC_SETB: case ZYDIS_MNEMONIC_CMOVB:
    case ZYDIS_MNEMONIC_FCMOVB:
      return "F.cf";
    case ZYDIS_MNEMONIC_JNB: case ZYDIS_MNEMONIC_SETNB:
    case ZYDIS_MNEMONIC_CMOVNB: case ZYDIS_MNEMONIC_FCMOVNB:
      return "!F.cf";
    case ZYDIS_MNEMONIC_JZ: case ZYDIS_MNEMONIC_SETZ: case ZYDIS_MNEMONIC_CMOVZ:
    case ZYDIS_MNEMONIC_FCMOVE:
      return "F.zf";
    case ZYDIS_MNEMONIC_JNZ: case ZYDIS_MNEMONIC_SETNZ:
    case ZYDIS_MNEMONIC_CMOVNZ: case ZYDIS_MNEMONIC_FCMOVNE:
      return "!F.zf";
    case ZYDIS_MNEMONIC_JBE: case ZYDIS_MNEMONIC_SETBE:
    case ZYDIS_MNEMONIC_CMOVBE: case ZYDIS_MNEMONIC_FCMOVBE:
      return "(F.cf || F.zf)";
    case ZYDIS_MNEMONIC_JNBE: case ZYDIS_MNEMONIC_SETNBE:
    case ZYDIS_MNEMONIC_CMOVNBE: case ZYDIS_MNEMONIC_FCMOVNBE:
      return "!(F.cf || F.zf)";
    case ZYDIS_MNEMONIC_JS: case ZYDIS_MNEMONIC_SETS: case ZYDIS_MNEMONIC_CMOVS:
      return "F.sf";
    case ZYDIS_MNEMONIC_JNS: case ZYDIS_MNEMONIC_SETNS:
    case ZYDIS_MNEMONIC_CMOVNS:
      return "!F.sf";
    case ZYDIS_MNEMONIC_JP: case ZYDIS_MNEMONIC_SETP: case ZYDIS_MNEMONIC_CMOVP:
    case ZYDIS_MNEMONIC_FCMOVU:
      return "F.pf";
    case ZYDIS_MNEMONIC_JNP: case ZYDIS_MNEMONIC_SETNP:
    case ZYDIS_MNEMONIC_CMOVNP: case ZYDIS_MNEMONIC_FCMOVNU:
      return "!F.pf";
    case ZYDIS_MNEMONIC_JL: case ZYDIS_MNEMONIC_SETL: case ZYDIS_MNEMONIC_CMOVL:
      return "(F.sf != F.of)";
    case ZYDIS_MNEMONIC_JNL: case ZYDIS_MNEMONIC_SETNL:
    case ZYDIS_MNEMONIC_CMOVNL:
      return "(F.sf == F.of)";
    case ZYDIS_MNEMONIC_JLE: case ZYDIS_MNEMONIC_SETLE:
    case ZYDIS_MNEMONIC_CMOVLE:
      return "(F.zf || F.sf != F.of)";
    case ZYDIS_MNEMONIC_JNLE: case ZYDIS_MNEMONIC_SETNLE:
    case ZYDIS_MNEMONIC_CMOVNLE:
      return "(!F.zf && F.sf == F.of)";
    default:
      return nullptr;
  }
}

const char* MemType(int bytes) {
  switch (bytes) {
    case 1: return "uint8_t";
    case 2: return "uint16_t";
    case 4: return "uint32_t";
    case 8: return "uint64_t";
    default: return nullptr;
  }
}

const char* X87OpName(ZydisMnemonic m) {
  switch (m) {
    case ZYDIS_MNEMONIC_FADD: case ZYDIS_MNEMONIC_FADDP:
    case ZYDIS_MNEMONIC_FIADD:
      return "Add";
    case ZYDIS_MNEMONIC_FMUL: case ZYDIS_MNEMONIC_FMULP:
    case ZYDIS_MNEMONIC_FIMUL:
      return "Mul";
    case ZYDIS_MNEMONIC_FSUB: case ZYDIS_MNEMONIC_FSUBP:
    case ZYDIS_MNEMONIC_FISUB:
      return "Sub";
    case ZYDIS_MNEMONIC_FSUBR: case ZYDIS_MNEMONIC_FSUBRP:
    case ZYDIS_MNEMONIC_FISUBR:
      return "SubR";
    case ZYDIS_MNEMONIC_FDIV: case ZYDIS_MNEMONIC_FDIVP:
    case ZYDIS_MNEMONIC_FIDIV:
      return "Div";
    case ZYDIS_MNEMONIC_FDIVR: case ZYDIS_MNEMONIC_FDIVRP:
    case ZYDIS_MNEMONIC_FIDIVR:
      return "DivR";
    default:
      return nullptr;
  }
}

// Emits the code for one instruction. Each instruction gets its own { }
// scope so temporaries never clash.
class InsnEmitter {
 public:
  InsnEmitter(const Program& p, const Function& f, const Insn& in,
              std::string* out, std::set<uint32_t>* refs, EmitStats* stats)
      : p_(p), f_(f), in_(in), d_(in.d), ops_(in.ops), out_(*out),
        refs_(*refs), stats_(*stats) {}

  void Emit();

 private:
  // ---- output -----------------------------------------------------------
  void L(const std::string& s) { out_ += "    " + s + "\n"; }
  void Unimpl() {
    stats_.unimplemented[ZydisMnemonicGetString(d_.mnemonic)]++;
    L(Fmt("LIFT_SPILL; ::ds1::Unimplemented(c, %s, \"%s\");",
          Hex(in_.va).c_str(), ZydisMnemonicGetString(d_.mnemonic)));
  }

  // ---- operands -----------------------------------------------------------
  int Nvis() const { return d_.operand_count_visible; }
  const ZydisDecodedOperand& Op(int i) const { return ops_[i]; }
  int Bytes(int i) const { return Op(i).size / 8; }
  bool IsReg(int i) const { return Op(i).type == ZYDIS_OPERAND_TYPE_REGISTER; }
  bool IsMem(int i) const { return Op(i).type == ZYDIS_OPERAND_TYPE_MEMORY; }
  bool IsImm(int i) const { return Op(i).type == ZYDIS_OPERAND_TYPE_IMMEDIATE; }

  // Effective address of a memory operand (segment base included, except
  // for LEA, which never adds one).
  std::string AddrExpr(int i, bool segment = true) const {
    const auto& m = Op(i).mem;
    std::string s;
    auto add = [&](const std::string& t) {
      if (!s.empty()) s += " + ";
      s += t;
    };
    if (segment && m.segment == ZYDIS_REGISTER_FS) add("FS");
    if (m.base != ZYDIS_REGISTER_NONE) add(Gpr32Name(m.base));
    if (m.index != ZYDIS_REGISTER_NONE)
      add(m.scale > 1 ? Fmt("%s * %du", Gpr32Name(m.index), m.scale)
                      : std::string(Gpr32Name(m.index)));
    if (m.disp.has_displacement || s.empty())
      add(Hex(static_cast<uint32_t>(m.disp.value)));
    return "uint32_t(" + s + ")";
  }

  bool AddrOk(int i) const {
    const auto& m = Op(i).mem;
    if (m.base != ZYDIS_REGISTER_NONE && !Gpr32Name(m.base)) return false;
    if (m.index != ZYDIS_REGISTER_NONE && !Gpr32Name(m.index)) return false;
    return m.segment == ZYDIS_REGISTER_NONE || m.segment == ZYDIS_REGISTER_DS ||
           m.segment == ZYDIS_REGISTER_ES || m.segment == ZYDIS_REGISTER_SS ||
           m.segment == ZYDIS_REGISTER_CS || m.segment == ZYDIS_REGISTER_FS;
  }

  // Declares m<i> = address for every explicit memory operand.
  bool PrepareMem() {
    for (int i = 0; i < Nvis(); i++) {
      if (!IsMem(i)) continue;
      if (!AddrOk(i)) return false;
      L(Fmt("const uint32_t m%d = %s;", i, AddrExpr(i).c_str()));
    }
    return true;
  }

  std::string ReadReg(ZydisRegister r) const {
    if (const char* n = Gpr32Name(r)) return n;
    if (const char* sv = SegValue(r)) return sv;
    ZydisRegister big =
        ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LEGACY_32, r);
    const char* n = Gpr32Name(big);
    if (!n) return "/*bad reg*/0u";
    int w = ZydisRegisterGetWidth(ZYDIS_MACHINE_MODE_LEGACY_32, r);
    if (w == 16) return Fmt("(%s & 0xFFFFu)", n);
    if (IsHi8(r)) return Fmt("((%s >> 8) & 0xFFu)", n);
    return Fmt("(%s & 0xFFu)", n);
  }

  std::string WriteReg(ZydisRegister r, const std::string& v) const {
    if (const char* n = Gpr32Name(r)) return Fmt("%s = uint32_t(%s);", n, v.c_str());
    if (SegValue(r)) return "/* segment register write ignored */";
    ZydisRegister big =
        ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LEGACY_32, r);
    const char* n = Gpr32Name(big);
    int w = ZydisRegisterGetWidth(ZYDIS_MACHINE_MODE_LEGACY_32, r);
    if (w == 16)
      return Fmt("%s = ::ds1::lift::SetLo16(%s, %s);", n, n, v.c_str());
    if (IsHi8(r))
      return Fmt("%s = ::ds1::lift::SetHi8(%s, %s);", n, n, v.c_str());
    return Fmt("%s = ::ds1::lift::SetLo8(%s, %s);", n, n, v.c_str());
  }

  // Value of operand i, zero-extended to uint32_t, masked to `bytes`.
  std::string Read(int i, int bytes = 0) const {
    if (!bytes) bytes = Bytes(i);
    const auto& o = Op(i);
    switch (o.type) {
      case ZYDIS_OPERAND_TYPE_REGISTER:
        return ReadReg(o.reg.value);
      case ZYDIS_OPERAND_TYPE_MEMORY:
        return Fmt("uint32_t(::ds1::lift::Rd<%s>(B, m%d))", MemType(bytes), i);
      case ZYDIS_OPERAND_TYPE_IMMEDIATE: {
        uint64_t v = o.imm.value.u;
        if (bytes == 1) v &= 0xFF;
        if (bytes == 2) v &= 0xFFFF;
        return Hex(static_cast<uint32_t>(v));
      }
      default:
        return "/*bad operand*/0u";
    }
  }

  std::string Write(int i, const std::string& v, int bytes = 0) const {
    if (!bytes) bytes = Bytes(i);
    const auto& o = Op(i);
    if (o.type == ZYDIS_OPERAND_TYPE_REGISTER) return WriteReg(o.reg.value, v);
    return Fmt("::ds1::lift::Wr<%s>(B, m%d, %s(%s));", MemType(bytes), i,
               MemType(bytes), v.c_str());
  }

  // ---- control flow -------------------------------------------------------
  std::string TailCall(uint32_t t) {
    refs_.insert(t);
    return Fmt("{ LIFT_SPILL; %s(c); return; }", FnName(t).c_str());
  }
  std::string GotoOrTail(uint32_t t) {
    if (t != f_.entry && p_.entries.count(t)) return TailCall(t);
    if (!f_.insns.count(t))  // target outside .text: never decoded
      return Fmt("{ LIFT_SPILL; ::ds1::BadJump(c, %s, %s); }",
                 Hex(in_.va).c_str(), Hex(t).c_str());
    return Fmt("goto L_%08X;", t);
  }
  void Push32(const std::string& v) {
    L(Fmt("esp -= 4; ::ds1::lift::Wr<uint32_t>(B, esp, %s);", v.c_str()));
  }

  void EmitAlu();
  void EmitShift();
  void EmitString();
  void EmitX87();
  void EmitLocked();

  const Program& p_;
  const Function& f_;
  const Insn& in_;
  const ZydisDecodedInstruction& d_;
  const ZydisDecodedOperand* ops_;
  std::string& out_;
  std::set<uint32_t>& refs_;
  EmitStats& stats_;
};

void InsnEmitter::EmitAlu() {
  int n = Bytes(0);
  std::string a = Read(0), b = Nvis() > 1 ? Read(1, n) : "";
  const char* fn = nullptr;
  bool store = true;
  std::string r;
  switch (d_.mnemonic) {
    case ZYDIS_MNEMONIC_ADD: fn = "Add"; break;
    case ZYDIS_MNEMONIC_ADC: fn = "Adc"; break;
    case ZYDIS_MNEMONIC_SUB: fn = "Sub"; break;
    case ZYDIS_MNEMONIC_SBB: fn = "Sbb"; break;
    case ZYDIS_MNEMONIC_CMP: fn = "Sub"; store = false; break;
    case ZYDIS_MNEMONIC_AND:
      r = Fmt("::ds1::lift::Logic<%d>(F, %s & %s)", n, a.c_str(), b.c_str());
      break;
    case ZYDIS_MNEMONIC_OR:
      r = Fmt("::ds1::lift::Logic<%d>(F, %s | %s)", n, a.c_str(), b.c_str());
      break;
    case ZYDIS_MNEMONIC_XOR:
      r = Fmt("::ds1::lift::Logic<%d>(F, %s ^ %s)", n, a.c_str(), b.c_str());
      break;
    case ZYDIS_MNEMONIC_TEST:
      r = Fmt("::ds1::lift::Logic<%d>(F, %s & %s)", n, a.c_str(), b.c_str());
      store = false;
      break;
    case ZYDIS_MNEMONIC_INC: r = Fmt("::ds1::lift::Inc<%d>(F, %s)", n, a.c_str()); break;
    case ZYDIS_MNEMONIC_DEC: r = Fmt("::ds1::lift::Dec<%d>(F, %s)", n, a.c_str()); break;
    case ZYDIS_MNEMONIC_NEG: r = Fmt("::ds1::lift::Neg<%d>(F, %s)", n, a.c_str()); break;
    case ZYDIS_MNEMONIC_NOT: r = Fmt("(~%s)", a.c_str()); break;
    default: break;
  }
  if (fn) r = Fmt("::ds1::lift::%s<%d>(F, %s, %s)", fn, n, a.c_str(), b.c_str());
  if (store) {
    L("const uint32_t r = " + r + ";");
    L(Write(0, "r"));
  } else {
    L("(void)" + r + ";");
  }
}

void InsnEmitter::EmitShift() {
  int n = Bytes(0);
  std::string cnt = Nvis() > 1 ? Read(1, 1) : "1u";
  const char* fn = nullptr;
  switch (d_.mnemonic) {
    case ZYDIS_MNEMONIC_SHL: fn = "Shl"; break;
    case ZYDIS_MNEMONIC_SHR: fn = "Shr"; break;
    case ZYDIS_MNEMONIC_SAR: fn = "Sar"; break;
    case ZYDIS_MNEMONIC_ROL: fn = "Rol"; break;
    case ZYDIS_MNEMONIC_ROR: fn = "Ror"; break;
    case ZYDIS_MNEMONIC_RCL: fn = "Rcl"; break;
    case ZYDIS_MNEMONIC_RCR: fn = "Rcr"; break;
    default: break;
  }
  L(Fmt("const uint32_t r = ::ds1::lift::%s<%d>(F, %s, %s);", fn, n,
        Read(0).c_str(), cnt.c_str()));
  L(Write(0, "r"));
}

void InsnEmitter::EmitString() {
  int n = 0;
  const char* kind = nullptr;
  switch (d_.mnemonic) {
    case ZYDIS_MNEMONIC_MOVSB: n = 1; kind = "movs"; break;
    case ZYDIS_MNEMONIC_MOVSW: n = 2; kind = "movs"; break;
    case ZYDIS_MNEMONIC_MOVSD: n = 4; kind = "movs"; break;
    case ZYDIS_MNEMONIC_STOSB: n = 1; kind = "stos"; break;
    case ZYDIS_MNEMONIC_STOSW: n = 2; kind = "stos"; break;
    case ZYDIS_MNEMONIC_STOSD: n = 4; kind = "stos"; break;
    case ZYDIS_MNEMONIC_LODSB: n = 1; kind = "lods"; break;
    case ZYDIS_MNEMONIC_LODSW: n = 2; kind = "lods"; break;
    case ZYDIS_MNEMONIC_LODSD: n = 4; kind = "lods"; break;
    case ZYDIS_MNEMONIC_SCASB: n = 1; kind = "scas"; break;
    case ZYDIS_MNEMONIC_SCASW: n = 2; kind = "scas"; break;
    case ZYDIS_MNEMONIC_SCASD: n = 4; kind = "scas"; break;
    case ZYDIS_MNEMONIC_CMPSB: n = 1; kind = "cmps"; break;
    case ZYDIS_MNEMONIC_CMPSW: n = 2; kind = "cmps"; break;
    case ZYDIS_MNEMONIC_CMPSD: n = 4; kind = "cmps"; break;
    default: break;
  }
  const char* t = MemType(n);
  std::string acc = n == 4 ? "eax" : n == 2 ? "(eax & 0xFFFFu)" : "(eax & 0xFFu)";
  std::string set_acc = n == 4   ? "eax = v;"
                        : n == 2 ? "eax = ::ds1::lift::SetLo16(eax, v);"
                                 : "eax = ::ds1::lift::SetLo8(eax, v);";
  std::string step = Fmt("const uint32_t d = F.df ? uint32_t(-%d) : %du;", n, n);
  std::string body;
  std::string kindstr = kind;
  // esi is DS:esi, edi is ES:edi; both flat. An fs: override on the source
  // (never seen in this binary) is not supported.
  if (kindstr == "movs")
    body = Fmt("::ds1::lift::Wr<%s>(B, edi, ::ds1::lift::Rd<%s>(B, esi)); "
               "esi += d; edi += d;", t, t);
  else if (kindstr == "stos")
    body = Fmt("::ds1::lift::Wr<%s>(B, edi, %s(%s)); edi += d;", t, t,
               acc.c_str());
  else if (kindstr == "lods")
    body = Fmt("{ const uint32_t v = ::ds1::lift::Rd<%s>(B, esi); %s } "
               "esi += d;", t, set_acc.c_str());
  else if (kindstr == "scas")
    body = Fmt("(void)::ds1::lift::Sub<%d>(F, %s, ::ds1::lift::Rd<%s>(B, edi)); "
               "edi += d;", n, acc.c_str(), t);
  else
    body = Fmt("(void)::ds1::lift::Sub<%d>(F, ::ds1::lift::Rd<%s>(B, esi), "
               "::ds1::lift::Rd<%s>(B, edi)); esi += d; edi += d;", n, t, t);

  bool rep = d_.attributes & ZYDIS_ATTRIB_HAS_REP;
  bool repe = d_.attributes & ZYDIS_ATTRIB_HAS_REPE;
  bool repne = d_.attributes & ZYDIS_ATTRIB_HAS_REPNE;
  L(step);
  if (kindstr == "scas" || kindstr == "cmps") {
    if (repe)
      L("while (ecx) { " + body + " ecx--; if (!F.zf) break; }");
    else if (repne)
      L("while (ecx) { " + body + " ecx--; if (F.zf) break; }");
    else
      L(body);
  } else if (rep || repe || repne) {
    L("while (ecx) { " + body + " ecx--; }");
  } else {
    L(body);
  }
}

// lock-prefixed read-modify-write on memory: compare-and-swap loop so the
// update is atomic on any host (and ordered: seq_cst, see ARCHITECTURE.md
// memory-ordering notes).
void InsnEmitter::EmitLocked() {
  int n = Bytes(0);
  const char* t = MemType(n);
  std::string src = Nvis() > 1 ? Read(1, n) : "0u";
  std::string calc;
  switch (d_.mnemonic) {
    case ZYDIS_MNEMONIC_ADD: calc = Fmt("::ds1::lift::Add<%d>(G, a, b)", n); break;
    case ZYDIS_MNEMONIC_ADC: calc = Fmt("::ds1::lift::Adc<%d>(G, a, b)", n); break;
    case ZYDIS_MNEMONIC_SUB: calc = Fmt("::ds1::lift::Sub<%d>(G, a, b)", n); break;
    case ZYDIS_MNEMONIC_SBB: calc = Fmt("::ds1::lift::Sbb<%d>(G, a, b)", n); break;
    case ZYDIS_MNEMONIC_AND: calc = Fmt("::ds1::lift::Logic<%d>(G, a & b)", n); break;
    case ZYDIS_MNEMONIC_OR: calc = Fmt("::ds1::lift::Logic<%d>(G, a | b)", n); break;
    case ZYDIS_MNEMONIC_XOR: calc = Fmt("::ds1::lift::Logic<%d>(G, a ^ b)", n); break;
    case ZYDIS_MNEMONIC_INC: calc = Fmt("::ds1::lift::Inc<%d>(G, a)", n); break;
    case ZYDIS_MNEMONIC_DEC: calc = Fmt("::ds1::lift::Dec<%d>(G, a)", n); break;
    case ZYDIS_MNEMONIC_NEG: calc = Fmt("::ds1::lift::Neg<%d>(G, a)", n); break;
    case ZYDIS_MNEMONIC_NOT: calc = Fmt("(%s)(~a)", t); break;
    case ZYDIS_MNEMONIC_XADD:
      calc = Fmt("::ds1::lift::Add<%d>(G, a, b)", n);
      break;
    default:
      Unimpl();
      return;
  }
  L(Fmt("%s* p = ::ds1::lift::AtomicPtr<%s>(B, m0);", t, t));
  L(Fmt("const uint32_t b = %s;", src.c_str()));
  L(Fmt("%s a = __atomic_load_n(p, __ATOMIC_SEQ_CST);", t));
  L(Fmt("%s r; ::ds1::Flags G;", t));
  L(Fmt("do { G = F; r = %s(%s); } while (!__atomic_compare_exchange_n("
        "p, &a, r, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));",
        t, calc.c_str()));
  L("F = G;");
  if (d_.mnemonic == ZYDIS_MNEMONIC_XADD) L(Write(1, "a"));
}

void InsnEmitter::EmitX87() {
  const auto m = d_.mnemonic;
  auto st = [&](int i) { return StIndex(Op(i).reg.value); };
  auto pop = [&]() { L("::ds1::x87::Pop(c.fpu);"); };
  // Visible operands: memory operand, or st(i) registers. Find the memory
  // operand index if any.
  int mem = -1;
  for (int i = 0; i < Nvis(); i++)
    if (IsMem(i)) mem = i;
  auto rd_mem = [&](int bytes) {
    return Fmt("::ds1::lift::Rd<%s>(B, m%d)", MemType(bytes), mem);
  };

  if (const char* op = X87OpName(m)) {
    bool p = m == ZYDIS_MNEMONIC_FADDP || m == ZYDIS_MNEMONIC_FMULP ||
             m == ZYDIS_MNEMONIC_FSUBP || m == ZYDIS_MNEMONIC_FSUBRP ||
             m == ZYDIS_MNEMONIC_FDIVP || m == ZYDIS_MNEMONIC_FDIVRP;
    bool integer = m == ZYDIS_MNEMONIC_FIADD || m == ZYDIS_MNEMONIC_FIMUL ||
                   m == ZYDIS_MNEMONIC_FISUB || m == ZYDIS_MNEMONIC_FISUBR ||
                   m == ZYDIS_MNEMONIC_FIDIV || m == ZYDIS_MNEMONIC_FIDIVR;
    std::string o = Fmt("::ds1::x87::Op::%s", op);
    if (mem >= 0) {
      int b = Bytes(mem);
      if (integer)
        L(Fmt("::ds1::x87::ArithI%d(c.fpu, %s, int%d_t(%s));", b * 8,
              o.c_str(), b * 8, rd_mem(b).c_str()));
      else
        L(Fmt("::ds1::x87::ArithF%d(c.fpu, %s, %s);", b * 8, o.c_str(),
              rd_mem(b).c_str()));
      return;
    }
    int dst = Nvis() >= 2 ? st(0) : 1;
    int src = Nvis() >= 2 ? st(1) : 0;
    if (dst == 0)
      L(Fmt("::ds1::x87::ArithSt0Sti(c.fpu, %s, %d);", o.c_str(), src));
    else
      L(Fmt("::ds1::x87::ArithStiSt0(c.fpu, %s, %d);", o.c_str(), dst));
    if (p) pop();
    return;
  }

  switch (m) {
    case ZYDIS_MNEMONIC_FLD:
      if (mem < 0) { L(Fmt("::ds1::x87::LoadSt(c.fpu, %d);", st(0))); return; }
      switch (Bytes(mem)) {
        case 4: L(Fmt("::ds1::x87::LoadF32(c.fpu, %s);", rd_mem(4).c_str())); return;
        case 8: L(Fmt("::ds1::x87::LoadF64(c.fpu, %s);", rd_mem(8).c_str())); return;
        case 10: L(Fmt("::ds1::x87::LoadF80(c.fpu, ::ds1::lift::RdF80(B, m%d));", mem)); return;
      }
      break;
    case ZYDIS_MNEMONIC_FILD:
      L(Fmt("::ds1::x87::LoadI%d(c.fpu, int%d_t(%s));", Bytes(mem) * 8,
            Bytes(mem) * 8, rd_mem(Bytes(mem)).c_str()));
      return;
    case ZYDIS_MNEMONIC_FST:
    case ZYDIS_MNEMONIC_FSTP:
      if (mem < 0) {
        L(Fmt("::ds1::x87::StoreSt(c.fpu, %d);", st(0)));
      } else {
        switch (Bytes(mem)) {
          case 4: L(Fmt("::ds1::lift::Wr<uint32_t>(B, m%d, ::ds1::x87::StoreF32(c.fpu));", mem)); break;
          case 8: L(Fmt("::ds1::lift::Wr<uint64_t>(B, m%d, ::ds1::x87::StoreF64(c.fpu));", mem)); break;
          case 10: L(Fmt("::ds1::lift::WrF80(B, m%d, ::ds1::x87::StoreF80(c.fpu));", mem)); break;
          default: Unimpl(); return;
        }
      }
      if (m == ZYDIS_MNEMONIC_FSTP) pop();
      return;
    case ZYDIS_MNEMONIC_FIST:
    case ZYDIS_MNEMONIC_FISTP: {
      int b = Bytes(mem);
      L(Fmt("::ds1::lift::Wr<uint%d_t>(B, m%d, uint%d_t(::ds1::x87::StoreI%d(c.fpu)));",
            b * 8, mem, b * 8, b * 8));
      if (m == ZYDIS_MNEMONIC_FISTP) pop();
      return;
    }
    case ZYDIS_MNEMONIC_FXCH:
      L(Fmt("::ds1::x87::Xch(c.fpu, %d);", Nvis() ? st(Nvis() - 1) : 1));
      return;
    case ZYDIS_MNEMONIC_FLD1: L("::ds1::x87::LoadConst(c.fpu, ::ds1::x87::Const::One);"); return;
    case ZYDIS_MNEMONIC_FLDZ: L("::ds1::x87::LoadConst(c.fpu, ::ds1::x87::Const::Zero);"); return;
    case ZYDIS_MNEMONIC_FLDPI: L("::ds1::x87::LoadConst(c.fpu, ::ds1::x87::Const::Pi);"); return;
    case ZYDIS_MNEMONIC_FLDL2E: L("::ds1::x87::LoadConst(c.fpu, ::ds1::x87::Const::L2E);"); return;
    case ZYDIS_MNEMONIC_FLDL2T: L("::ds1::x87::LoadConst(c.fpu, ::ds1::x87::Const::L2T);"); return;
    case ZYDIS_MNEMONIC_FLDLG2: L("::ds1::x87::LoadConst(c.fpu, ::ds1::x87::Const::LG2);"); return;
    case ZYDIS_MNEMONIC_FLDLN2: L("::ds1::x87::LoadConst(c.fpu, ::ds1::x87::Const::LN2);"); return;
    case ZYDIS_MNEMONIC_FCOM:
    case ZYDIS_MNEMONIC_FCOMP:
    case ZYDIS_MNEMONIC_FUCOM:
    case ZYDIS_MNEMONIC_FUCOMP: {
      bool quiet = m == ZYDIS_MNEMONIC_FUCOM || m == ZYDIS_MNEMONIC_FUCOMP;
      if (mem >= 0) {
        if (Bytes(mem) == 4) L(Fmt("::ds1::x87::CompareF32(c.fpu, %s);", rd_mem(4).c_str()));
        else L(Fmt("::ds1::x87::CompareF64(c.fpu, %s);", rd_mem(8).c_str()));
      } else {
        // Forms: "fcom" (st1), "fcom st(i)", "fcom st0, st(i)".
        int i = Nvis() ? st(Nvis() - 1) : 1;
        L(Fmt("::ds1::x87::CompareSt(c.fpu, %d, %s);", i, quiet ? "true" : "false"));
      }
      if (m == ZYDIS_MNEMONIC_FCOMP || m == ZYDIS_MNEMONIC_FUCOMP) pop();
      return;
    }
    case ZYDIS_MNEMONIC_FCOMPP:
    case ZYDIS_MNEMONIC_FUCOMPP:
      L(Fmt("::ds1::x87::CompareSt(c.fpu, 1, %s);",
            m == ZYDIS_MNEMONIC_FUCOMPP ? "true" : "false"));
      pop();
      pop();
      return;
    case ZYDIS_MNEMONIC_FICOM:
    case ZYDIS_MNEMONIC_FICOMP:
      L(Fmt("::ds1::x87::CompareI%d(c.fpu, int%d_t(%s));", Bytes(mem) * 8,
            Bytes(mem) * 8, rd_mem(Bytes(mem)).c_str()));
      if (m == ZYDIS_MNEMONIC_FICOMP) pop();
      return;
    case ZYDIS_MNEMONIC_FCOMI:
    case ZYDIS_MNEMONIC_FCOMIP:
    case ZYDIS_MNEMONIC_FUCOMI:
    case ZYDIS_MNEMONIC_FUCOMIP: {
      bool quiet = m == ZYDIS_MNEMONIC_FUCOMI || m == ZYDIS_MNEMONIC_FUCOMIP;
      L(Fmt("const auto r = ::ds1::x87::CompareStEFlags(c.fpu, %d, %s);",
            st(Nvis() - 1), quiet ? "true" : "false"));
      L("F.zf = r.zf; F.pf = r.pf; F.cf = r.cf; F.of = F.sf = F.af = false;");
      if (m == ZYDIS_MNEMONIC_FCOMIP || m == ZYDIS_MNEMONIC_FUCOMIP) pop();
      return;
    }
    case ZYDIS_MNEMONIC_FTST: L("::ds1::x87::Test(c.fpu);"); return;
    case ZYDIS_MNEMONIC_FXAM: L("::ds1::x87::Examine(c.fpu);"); return;
    case ZYDIS_MNEMONIC_FCHS: L("::ds1::x87::Chs(c.fpu);"); return;
    case ZYDIS_MNEMONIC_FABS: L("::ds1::x87::Abs(c.fpu);"); return;
    case ZYDIS_MNEMONIC_FSQRT: L("::ds1::x87::Sqrt(c.fpu);"); return;
    case ZYDIS_MNEMONIC_FSIN: L("::ds1::x87::Sin(c.fpu);"); return;
    case ZYDIS_MNEMONIC_FCOS: L("::ds1::x87::Cos(c.fpu);"); return;
    case ZYDIS_MNEMONIC_FSINCOS: L("::ds1::x87::SinCos(c.fpu);"); return;
    case ZYDIS_MNEMONIC_FPTAN: L("::ds1::x87::Ptan(c.fpu);"); return;
    case ZYDIS_MNEMONIC_FPATAN: L("::ds1::x87::Patan(c.fpu);"); return;
    case ZYDIS_MNEMONIC_F2XM1: L("::ds1::x87::F2xm1(c.fpu);"); return;
    case ZYDIS_MNEMONIC_FYL2X: L("::ds1::x87::Yl2x(c.fpu);"); return;
    case ZYDIS_MNEMONIC_FYL2XP1: L("::ds1::x87::Yl2xp1(c.fpu);"); return;
    case ZYDIS_MNEMONIC_FSCALE: L("::ds1::x87::Scale(c.fpu);"); return;
    case ZYDIS_MNEMONIC_FPREM: L("::ds1::x87::Prem(c.fpu);"); return;
    case ZYDIS_MNEMONIC_FPREM1: L("::ds1::x87::Prem1(c.fpu);"); return;
    case ZYDIS_MNEMONIC_FRNDINT: L("::ds1::x87::RndInt(c.fpu);"); return;
    case ZYDIS_MNEMONIC_FXTRACT: L("::ds1::x87::Xtract(c.fpu);"); return;
    case ZYDIS_MNEMONIC_FNSTSW:
      if (mem >= 0)
        L(Fmt("::ds1::lift::Wr<uint16_t>(B, m%d, ::ds1::x87::StatusWord(c.fpu));", mem));
      else
        L("eax = ::ds1::lift::SetLo16(eax, ::ds1::x87::StatusWord(c.fpu));");
      return;
    case ZYDIS_MNEMONIC_FNSTCW:
      L(Fmt("::ds1::lift::Wr<uint16_t>(B, m%d, ::ds1::x87::ControlWord(c.fpu));", mem));
      return;
    case ZYDIS_MNEMONIC_FLDCW:
      L(Fmt("::ds1::x87::SetControlWord(c.fpu, %s);", rd_mem(2).c_str()));
      return;
    case ZYDIS_MNEMONIC_FNCLEX: L("::ds1::x87::ClearExceptions(c.fpu);"); return;
    case ZYDIS_MNEMONIC_FNINIT: L("::ds1::x87::Init(c.fpu);"); return;
    case ZYDIS_MNEMONIC_FFREE: L(Fmt("::ds1::x87::Free(c.fpu, %d);", st(0))); return;
    case ZYDIS_MNEMONIC_FDECSTP: L("::ds1::x87::DecStp(c.fpu);"); return;
    case ZYDIS_MNEMONIC_FINCSTP: L("::ds1::x87::IncStp(c.fpu);"); return;
    case ZYDIS_MNEMONIC_FNOP:
    case ZYDIS_MNEMONIC_FWAIT:
      return;
    case ZYDIS_MNEMONIC_FNSTENV:
      L(Fmt("::ds1::x87::StoreEnv(c.fpu, B + m%d);", mem));
      return;
    case ZYDIS_MNEMONIC_FLDENV:
      L(Fmt("::ds1::x87::LoadEnv(c.fpu, B + m%d);", mem));
      return;
    case ZYDIS_MNEMONIC_FNSAVE:
      L(Fmt("::ds1::x87::Save(c.fpu, B + m%d);", mem));
      return;
    case ZYDIS_MNEMONIC_FRSTOR:
      L(Fmt("::ds1::x87::Restore(c.fpu, B + m%d);", mem));
      return;
    case ZYDIS_MNEMONIC_FCMOVB: case ZYDIS_MNEMONIC_FCMOVBE:
    case ZYDIS_MNEMONIC_FCMOVE: case ZYDIS_MNEMONIC_FCMOVNB:
    case ZYDIS_MNEMONIC_FCMOVNBE: case ZYDIS_MNEMONIC_FCMOVNE:
    case ZYDIS_MNEMONIC_FCMOVNU: case ZYDIS_MNEMONIC_FCMOVU:
      L(Fmt("if (%s) ::ds1::x87::CopySt(c.fpu, %d);", CondExpr(m),
            st(Nvis() - 1)));
      return;
    default:
      break;
  }
  Unimpl();
}

void InsnEmitter::Emit() {
  const auto m = d_.mnemonic;
  const uint32_t next = in_.next();

  if (d_.meta.category == ZYDIS_CATEGORY_X87_ALU ||
      m == ZYDIS_MNEMONIC_FWAIT) {
    if (!PrepareMem()) return Unimpl();
    return EmitX87();
  }
  if (d_.meta.category == ZYDIS_CATEGORY_STRINGOP) return EmitString();

  // Far pointers and other operand widths we don't model (only seen in
  // data that merely decodes as code) become traps.
  for (int i = 0; i < Nvis(); i++)
    if (IsMem(i) && m != ZYDIS_MNEMONIC_LEA && !MemType(Bytes(i)))
      return Unimpl();

  bool locked = (d_.attributes & ZYDIS_ATTRIB_HAS_LOCK) && Nvis() && IsMem(0);

  switch (m) {
    // ---- control flow ---------------------------------------------------
    case ZYDIS_MNEMONIC_CALL: {
      if (IsImm(0)) {
        uint32_t t = RelTarget(in_);
        if (t == next) {  // call $+5; pop reg: reads EIP
          Push32(Hex(next));
          return;
        }
        Push32(Hex(next));
        if (!p_.InText(t)) {
          L(Fmt("LIFT_SPILL; ::ds1::BadJump(c, %s, %s);", Hex(in_.va).c_str(),
                Hex(t).c_str()));
          return;
        }
        refs_.insert(t);
        L(Fmt("LIFT_SPILL; %s(c); LIFT_RELOAD;", FnName(t).c_str()));
      } else {
        if (!PrepareMem()) return Unimpl();
        L("const uint32_t t = " + Read(0) + ";");
        Push32(Hex(next));
        L(Fmt("LIFT_SPILL; ::ds1::Dispatch(c, t, %s); LIFT_RELOAD;",
              Hex(in_.va).c_str()));
      }
      uint32_t tt;
      if (Classify(p_, f_, in_, &tt) == Flow::Stop)
        L(Fmt("::ds1::Unreachable(c, %s);", Hex(in_.va).c_str()));
      return;
    }
    case ZYDIS_MNEMONIC_JMP: {
      if (IsImm(0)) {
        L(GotoOrTail(RelTarget(in_)));
        return;
      }
      if (!PrepareMem()) return Unimpl();
      L("const uint32_t t = " + Read(0) + ";");
      auto jt = p_.jumptables.find(in_.va);
      if (jt != p_.jumptables.end()) {
        L("switch (t) {");
        for (uint32_t tgt : jt->second)
          L(Fmt("  case %s: %s", Hex(tgt).c_str(), GotoOrTail(tgt).c_str()));
        L(Fmt("  default: LIFT_SPILL; ::ds1::BadJump(c, %s, t);",
              Hex(in_.va).c_str()));
        L("}");
      } else {
        L(Fmt("LIFT_SPILL; ::ds1::Dispatch(c, t, %s); return;",
              Hex(in_.va).c_str()));
      }
      return;
    }
    case ZYDIS_MNEMONIC_RET: {
      uint32_t extra = Nvis() ? static_cast<uint32_t>(Op(0).imm.value.u) : 0;
      L(Fmt("esp += %uu; LIFT_SPILL; return;", 4 + extra));
      return;
    }
    case ZYDIS_MNEMONIC_JECXZ:
      L(Fmt("if (ecx == 0) %s", GotoOrTail(RelTarget(in_)).c_str()));
      return;
    case ZYDIS_MNEMONIC_JCXZ:
      L(Fmt("if ((ecx & 0xFFFFu) == 0) %s", GotoOrTail(RelTarget(in_)).c_str()));
      return;
    case ZYDIS_MNEMONIC_LOOP:
      L(Fmt("ecx -= 1; if (ecx != 0) %s", GotoOrTail(RelTarget(in_)).c_str()));
      return;
    case ZYDIS_MNEMONIC_LOOPE:
      L(Fmt("ecx -= 1; if (ecx != 0 && F.zf) %s",
            GotoOrTail(RelTarget(in_)).c_str()));
      return;
    case ZYDIS_MNEMONIC_LOOPNE:
      L(Fmt("ecx -= 1; if (ecx != 0 && !F.zf) %s",
            GotoOrTail(RelTarget(in_)).c_str()));
      return;
    case ZYDIS_MNEMONIC_INT3:
    case ZYDIS_MNEMONIC_INT:
    case ZYDIS_MNEMONIC_INT1:
    case ZYDIS_MNEMONIC_INTO:
    case ZYDIS_MNEMONIC_HLT:
    case ZYDIS_MNEMONIC_UD2:
      L(Fmt("LIFT_SPILL; ::ds1::Trap(c, %s); LIFT_RELOAD;", Hex(in_.va).c_str()));
      return;
    default:
      break;
  }

  if (const char* cond = CondExpr(m)) {
    if (d_.meta.category == ZYDIS_CATEGORY_COND_BR) {
      L(Fmt("if (%s) %s", cond, GotoOrTail(RelTarget(in_)).c_str()));
      return;
    }
    if (!PrepareMem()) return Unimpl();
    if (d_.meta.category == ZYDIS_CATEGORY_SETCC) {
      L(Write(0, Fmt("(%s) ? 1u : 0u", cond)));
      return;
    }
    // CMOVcc: the source is read even when the condition is false.
    L("const uint32_t v = " + Read(1) + ";");
    L(Fmt("if (%s) { %s }", cond, Write(0, "v").c_str()));
    return;
  }

  if (!PrepareMem()) return Unimpl();
  if (locked) return EmitLocked();

  switch (m) {
    case ZYDIS_MNEMONIC_NOP:
    case ZYDIS_MNEMONIC_PAUSE:
      return;
    case ZYDIS_MNEMONIC_MOV:
      L(Write(0, Read(1, Bytes(0))));
      return;
    case ZYDIS_MNEMONIC_MOVZX:
      L(Write(0, Read(1)));
      return;
    case ZYDIS_MNEMONIC_MOVSX:
      L(Write(0, Fmt("uint32_t(::ds1::lift::Sext<%d>(%s))", Bytes(1),
                     Read(1).c_str())));
      return;
    case ZYDIS_MNEMONIC_LEA:
      L(Write(0, AddrExpr(1, false)));
      return;
    case ZYDIS_MNEMONIC_PUSH: {
      L("const uint32_t v = " + Read(0, 4) + ";");
      if (Bytes(0) == 2 && !IsImm(0)) {
        L("esp -= 2; ::ds1::lift::Wr<uint16_t>(B, esp, uint16_t(v));");
      } else {
        Push32("v");
      }
      return;
    }
    case ZYDIS_MNEMONIC_POP: {
      // A memory destination's address uses ESP after the increment, so
      // recompute it here instead of using the pre-computed m0.
      L("const uint32_t v = ::ds1::lift::Rd<uint32_t>(B, esp); esp += 4;");
      if (IsMem(0))
        L(Fmt("::ds1::lift::Wr<uint32_t>(B, %s, v);", AddrExpr(0).c_str()));
      else
        L(Write(0, "v"));
      return;
    }
    case ZYDIS_MNEMONIC_PUSHAD:
      L("const uint32_t s = esp;");
      for (const char* r : {"eax", "ecx", "edx", "ebx", "s", "ebp", "esi", "edi"})
        Push32(r);
      return;
    case ZYDIS_MNEMONIC_POPAD:
      L("edi = ::ds1::lift::Rd<uint32_t>(B, esp); esi = ::ds1::lift::Rd<uint32_t>(B, esp + 4);");
      L("ebp = ::ds1::lift::Rd<uint32_t>(B, esp + 8); ebx = ::ds1::lift::Rd<uint32_t>(B, esp + 16);");
      L("edx = ::ds1::lift::Rd<uint32_t>(B, esp + 20); ecx = ::ds1::lift::Rd<uint32_t>(B, esp + 24);");
      L("eax = ::ds1::lift::Rd<uint32_t>(B, esp + 28); esp += 32;");
      return;
    case ZYDIS_MNEMONIC_PUSHFD:
      Push32("::ds1::PackEFlags(F)");
      return;
    case ZYDIS_MNEMONIC_POPFD:
      L("::ds1::UnpackEFlags(F, ::ds1::lift::Rd<uint32_t>(B, esp)); esp += 4;");
      return;
    case ZYDIS_MNEMONIC_XCHG:
      if (IsMem(0) || IsMem(1)) {
        int mi = IsMem(0) ? 0 : 1, ri = 1 - mi;
        const char* t = MemType(Bytes(mi));
        L(Fmt("const uint32_t v = __atomic_exchange_n(::ds1::lift::AtomicPtr<%s>(B, m%d), %s(%s), __ATOMIC_SEQ_CST);",
              t, mi, t, Read(ri).c_str()));
        L(Write(ri, "v"));
      } else {
        L("const uint32_t a = " + Read(0) + ", b = " + Read(1) + ";");
        L(Write(0, "b"));
        L(Write(1, "a"));
      }
      return;
    case ZYDIS_MNEMONIC_ADD: case ZYDIS_MNEMONIC_ADC: case ZYDIS_MNEMONIC_SUB:
    case ZYDIS_MNEMONIC_SBB: case ZYDIS_MNEMONIC_CMP: case ZYDIS_MNEMONIC_AND:
    case ZYDIS_MNEMONIC_OR: case ZYDIS_MNEMONIC_XOR: case ZYDIS_MNEMONIC_TEST:
    case ZYDIS_MNEMONIC_INC: case ZYDIS_MNEMONIC_DEC: case ZYDIS_MNEMONIC_NEG:
    case ZYDIS_MNEMONIC_NOT:
      return EmitAlu();
    case ZYDIS_MNEMONIC_SHL: case ZYDIS_MNEMONIC_SHR: case ZYDIS_MNEMONIC_SAR:
    case ZYDIS_MNEMONIC_ROL: case ZYDIS_MNEMONIC_ROR: case ZYDIS_MNEMONIC_RCL:
    case ZYDIS_MNEMONIC_RCR:
      return EmitShift();
    case ZYDIS_MNEMONIC_SHLD:
    case ZYDIS_MNEMONIC_SHRD:
      L(Fmt("const uint32_t r = ::ds1::lift::%s<%d>(F, %s, %s, %s);",
            m == ZYDIS_MNEMONIC_SHLD ? "Shld" : "Shrd", Bytes(0),
            Read(0).c_str(), Read(1).c_str(), Read(2, 1).c_str()));
      L(Write(0, "r"));
      return;
    case ZYDIS_MNEMONIC_IMUL:
      if (Nvis() == 1) {
        int n = Bytes(0);
        if (n == 1) {
          L("uint32_t lo, hi; ::ds1::lift::Imul1<1>(F, eax, " + Read(0) + ", lo, hi);");
          L("eax = ::ds1::lift::SetLo16(eax, lo | (hi << 8));");
        } else if (n == 2) {
          L("uint32_t lo, hi; ::ds1::lift::Imul1<2>(F, eax, " + Read(0) + ", lo, hi);");
          L("eax = ::ds1::lift::SetLo16(eax, lo); edx = ::ds1::lift::SetLo16(edx, hi);");
        } else {
          L("::ds1::lift::Imul1<4>(F, eax, " + Read(0) + ", eax, edx);");
        }
      } else {
        int n = Bytes(0);
        std::string a = Nvis() == 3 ? Read(1) : Read(0);
        std::string b = Nvis() == 3 ? Read(2, n) : Read(1, n);
        L(Fmt("const uint32_t r = ::ds1::lift::Imul2<%d>(F, %s, %s);", n,
              a.c_str(), b.c_str()));
        L(Write(0, "r"));
      }
      return;
    case ZYDIS_MNEMONIC_MUL: {
      int n = Bytes(0);
      if (n == 1) {
        L("uint32_t lo, hi; ::ds1::lift::Mul1<1>(F, eax, " + Read(0) + ", lo, hi);");
        L("eax = ::ds1::lift::SetLo16(eax, lo | (hi << 8));");
      } else if (n == 2) {
        L("uint32_t lo, hi; ::ds1::lift::Mul1<2>(F, eax, " + Read(0) + ", lo, hi);");
        L("eax = ::ds1::lift::SetLo16(eax, lo); edx = ::ds1::lift::SetLo16(edx, hi);");
      } else {
        L("::ds1::lift::Mul1<4>(F, eax, " + Read(0) + ", eax, edx);");
      }
      return;
    }
    case ZYDIS_MNEMONIC_DIV:
    case ZYDIS_MNEMONIC_IDIV: {
      int n = Bytes(0);
      const char* fn = m == ZYDIS_MNEMONIC_DIV ? "Div" : "Idiv";
      std::string fail = Fmt("{ LIFT_SPILL; ::ds1::RaiseDivideError(c, %s); LIFT_RELOAD; }",
                             Hex(in_.va).c_str());
      if (n == 1) {
        L("uint32_t lo = eax & 0xFFu, hi = (eax >> 8) & 0xFFu;");
        L(Fmt("if (::ds1::lift::%s<1>(lo, hi, %s)) eax = ::ds1::lift::SetLo16(eax, lo | (hi << 8)); else %s",
              fn, Read(0).c_str(), fail.c_str()));
      } else if (n == 2) {
        L("uint32_t lo = eax & 0xFFFFu, hi = edx & 0xFFFFu;");
        L(Fmt("if (::ds1::lift::%s<2>(lo, hi, %s)) { eax = ::ds1::lift::SetLo16(eax, lo); edx = ::ds1::lift::SetLo16(edx, hi); } else %s",
              fn, Read(0).c_str(), fail.c_str()));
      } else {
        L(Fmt("uint32_t lo = eax, hi = edx; if (::ds1::lift::%s<4>(lo, hi, %s)) { eax = lo; edx = hi; } else %s",
              fn, Read(0).c_str(), fail.c_str()));
      }
      return;
    }
    case ZYDIS_MNEMONIC_CDQ:
      L("edx = uint32_t(int32_t(eax) >> 31);");
      return;
    case ZYDIS_MNEMONIC_CWDE:
      L("eax = uint32_t(int32_t(int16_t(eax)));");
      return;
    case ZYDIS_MNEMONIC_CBW:
      L("eax = ::ds1::lift::SetLo16(eax, uint32_t(int32_t(int8_t(eax))));");
      return;
    case ZYDIS_MNEMONIC_CWD:
      L("edx = ::ds1::lift::SetLo16(edx, (eax & 0x8000u) ? 0xFFFFu : 0u);");
      return;
    case ZYDIS_MNEMONIC_LEAVE:
      L("esp = ebp; ebp = ::ds1::lift::Rd<uint32_t>(B, esp); esp += 4;");
      return;
    case ZYDIS_MNEMONIC_ENTER: {
      uint32_t size = static_cast<uint32_t>(Op(0).imm.value.u);
      uint32_t level = static_cast<uint32_t>(Op(1).imm.value.u) & 31;
      if (level) return Unimpl();
      Push32("ebp");
      L(Fmt("ebp = esp; esp -= %uu;", size));
      return;
    }
    case ZYDIS_MNEMONIC_CLD: L("F.df = false;"); return;
    case ZYDIS_MNEMONIC_STD: L("F.df = true;"); return;
    case ZYDIS_MNEMONIC_CLC: L("F.cf = false;"); return;
    case ZYDIS_MNEMONIC_STC: L("F.cf = true;"); return;
    case ZYDIS_MNEMONIC_CMC: L("F.cf = !F.cf;"); return;
    case ZYDIS_MNEMONIC_SAHF: L("::ds1::lift::Sahf(F, (eax >> 8) & 0xFFu);"); return;
    case ZYDIS_MNEMONIC_LAHF: L("eax = ::ds1::lift::SetHi8(eax, ::ds1::lift::Lahf(F));"); return;
    case ZYDIS_MNEMONIC_BSWAP:
      L(Write(0, "__builtin_bswap32(" + Read(0) + ")"));
      return;
    case ZYDIS_MNEMONIC_BSF:
    case ZYDIS_MNEMONIC_BSR:
      L(Fmt("const uint32_t r = ::ds1::lift::%s<%d>(F, %s, %s);",
            m == ZYDIS_MNEMONIC_BSF ? "Bsf" : "Bsr", Bytes(0), Read(0).c_str(),
            Read(1).c_str()));
      L(Write(0, "r"));
      return;
    case ZYDIS_MNEMONIC_BT: case ZYDIS_MNEMONIC_BTS: case ZYDIS_MNEMONIC_BTR:
    case ZYDIS_MNEMONIC_BTC: {
      int n = Bytes(0);
      // Memory operand with a register bit offset can address outside the
      // operand: the offset is signed and selects a byte relative to it.
      if (IsMem(0) && IsReg(1)) {
        L(Fmt("const int32_t off = int32_t(%s);", Read(1).c_str()));
        L("const uint32_t a = m0 + uint32_t(off >> 3);");
        L("const uint32_t bit = uint32_t(off) & 7u;");
        L("uint32_t v = ::ds1::lift::Rd<uint8_t>(B, a);");
      } else {
        L(Fmt("const uint32_t bit = %s & %du;", Read(1).c_str(), n * 8 - 1));
        L("uint32_t v = " + Read(0) + ";");
      }
      L("F.cf = (v >> bit) & 1u;");
      const char* upd = m == ZYDIS_MNEMONIC_BTS   ? "v |= 1u << bit;"
                        : m == ZYDIS_MNEMONIC_BTR ? "v &= ~(1u << bit);"
                        : m == ZYDIS_MNEMONIC_BTC ? "v ^= 1u << bit;"
                                                  : nullptr;
      if (upd) {
        L(upd);
        if (IsMem(0) && IsReg(1))
          L("::ds1::lift::Wr<uint8_t>(B, a, uint8_t(v));");
        else
          L(Write(0, "v"));
      }
      return;
    }
    case ZYDIS_MNEMONIC_CMPXCHG: {
      int n = Bytes(0);
      std::string acc = n == 4 ? "eax" : n == 2 ? "(eax & 0xFFFFu)" : "(eax & 0xFFu)";
      L("const uint32_t d = " + Read(0) + ";");
      L(Fmt("(void)::ds1::lift::Sub<%d>(F, %s, d);", n, acc.c_str()));
      L("if (F.zf) { " + Write(0, Read(1)) + " } else { " +
        (n == 4 ? std::string("eax = d;")
                : n == 2 ? std::string("eax = ::ds1::lift::SetLo16(eax, d);")
                         : std::string("eax = ::ds1::lift::SetLo8(eax, d);")) +
        " }");
      return;
    }
    case ZYDIS_MNEMONIC_XADD:
      L(Fmt("const uint32_t a = %s, b = %s;", Read(0).c_str(), Read(1).c_str()));
      L(Fmt("const uint32_t r = ::ds1::lift::Add<%d>(F, a, b);", Bytes(0)));
      L(Write(1, "a"));
      L(Write(0, "r"));
      return;
    case ZYDIS_MNEMONIC_XLAT:
      L("eax = ::ds1::lift::SetLo8(eax, ::ds1::lift::Rd<uint8_t>(B, ebx + (eax & 0xFFu)));");
      return;
    case ZYDIS_MNEMONIC_CPUID:
      L("LIFT_SPILL; ::ds1::Cpuid(c); LIFT_RELOAD;");
      return;
    case ZYDIS_MNEMONIC_RDTSC:
      L("{ LIFT_SPILL; const uint64_t t = ::ds1::Rdtsc(c); eax = uint32_t(t); edx = uint32_t(t >> 32); }");
      return;
    default:
      break;
  }
  Unimpl();
}

}  // namespace

std::string FnName(uint32_t va) { return Fmt("f_%08X", va); }

void Emitter::EmitFunction(const Function& f, std::string* out,
                           std::set<uint32_t>* refs, EmitStats* stats) {
  std::string& o = *out;
  auto nm = p_.names.find(f.entry);
  o += "// " + (nm != p_.names.end() ? nm->second : FnName(f.entry)) + "\n";
  o += "void " + FnName(f.entry) + "(::ds1::Ctx& c) {\n  LIFT_ENTER\n";
  auto pr = p_.probes.find(f.entry);
  if (pr != p_.probes.end())
    o += Fmt("  LIFT_SPILL; ::ds1::Probe(c, 0x%Xu, \"%s\"); LIFT_RELOAD;\n",
             f.entry, pr->second.c_str());

  // Labels: branch targets plus non-contiguous fallthroughs.
  std::set<uint32_t> labels = f.labels;
  for (auto it = f.insns.begin(); it != f.insns.end(); ++it) {
    auto nx = std::next(it);
    uint32_t t;
    Flow fl = Classify(p_, f, it->second, &t);
    if ((fl == Flow::Fall || fl == Flow::Branch) &&
        (nx == f.insns.end() || nx->first != it->second.next()))
      labels.insert(it->second.next());
  }

  for (auto it = f.insns.begin(); it != f.insns.end(); ++it) {
    const Insn& in = it->second;
    if (labels.count(in.va)) o += Fmt("L_%08X:\n", in.va);
    stats->insns++;
    o += Fmt("  { // %08X %s\n", in.va, dec_.Format(in).c_str());
    if (!in.ok) {
      o += Fmt("    LIFT_SPILL; ::ds1::Unimplemented(c, 0x%Xu, \"undecodable\");\n",
               in.va);
      stats->unimplemented["(undecodable)"]++;
    } else {
      InsnEmitter(p_, f, in, &o, refs, stats).Emit();
    }
    o += "  }\n";

    // Continue to the next instruction if it isn't laid out next.
    uint32_t t;
    Flow fl = Classify(p_, f, in, &t);
    if (fl == Flow::Fall || fl == Flow::Branch) {
      uint32_t nx = in.next();
      auto ni = std::next(it);
      if (nx != f.entry && p_.entries.count(nx)) {
        refs->insert(nx);
        o += Fmt("  LIFT_SPILL; %s(c); return;\n", FnName(nx).c_str());
      } else if (ni == f.insns.end() || ni->first != nx) {
        o += f.insns.count(nx)
                 ? Fmt("  goto L_%08X;\n", nx)
                 : Fmt("  LIFT_SPILL; ::ds1::BadJump(c, 0x%Xu, 0x%Xu);\n",
                       in.va, nx);
      }
    }
  }
  o += "}\n\n";
}

}  // namespace lifter
