#pragma once
// Exact model of the x87 FPU for lifted code (see ARCHITECTURE.md, "x87:
// full 80-bit model"). Portable C++ on top of Berkeley SoftFloat 3e; never
// uses the host FPU, long double or intrinsics.
//
// Conventions:
// - "st(i)" is relative to TOP; physical register = (top + i) & 7.
// - Every operation honours the control word: PC (bits 8-9) rounds the
//   significand of add/sub/mul/div/sqrt results to 24/53/64 bits with the
//   full 15-bit exponent range kept; RC (bits 10-11) applies to every
//   rounding (arithmetic, stores to m32/m64/int, frndint).
// - Exceptions are assumed masked (Windows default 0x027F, game uses
//   masked modes). Flags are accumulated sticky in the status word. If an
//   exception occurs while its mask bit is clear, unmasked_fault is set so
//   the runtime can report it; the masked response is still delivered.
// - Stack overflow/underflow follow the masked response: IE + SF set, C1
//   = 1 on overflow / 0 on underflow, result is the real indefinite.
// - Compare results go to C0/C2/C3 of the status word (for fnstsw+sahf);
//   the FCOMI family returns EFLAGS bits instead.

#include <cstdint>

namespace ds1::x87 {

// 80-bit extended value, same layout as the x87 memory format and as
// SoftFloat's extFloat80M: 64-bit significand (explicit integer bit), then
// sign (bit 15) and biased exponent (bits 0-14).
struct F80 {
  uint64_t sig;
  uint16_t se;
};

struct State {
  F80 r[8];        // physical registers R0..R7
  uint16_t cw;     // control word
  uint16_t sw;     // status word, TOP field (bits 11-13) kept in `top`
  uint16_t tw;     // full tag word: 2 bits per physical register
                   // (00 valid, 01 zero, 10 special, 11 empty)
  uint8_t top;     // 0..7
  bool unmasked_fault;  // an unmasked exception was raised (see above)
  // Last instruction / operand pointers, for fnstenv/fnsave. The lifter
  // stores the guest VA of each x87 instruction that has a memory operand
  // here; others may leave them stale (no game code inspects them).
  uint32_t fip, fdp;
  uint16_t fop;
};

// Test hook: add/sub/mul/div of normal operands take an integer fast
// path; true sends them through SoftFloat, its reference.
extern bool g_softfloat_only;

enum class Op : uint8_t { Add, Mul, Sub, SubR, Div, DivR };
// Sub: dst - src, SubR: src - dst, Div: dst / src, DivR: src / dst, where
// dst is the register being written (Intel manual operand order).

enum class Const : uint8_t { One, L2T, L2E, Pi, LG2, LN2, Zero };

// FCOMI/FUCOMI result as EFLAGS bits.
struct EFlagsResult {
  bool zf, pf, cf;
};

// --- state -----------------------------------------------------------
void Init(State& s);              // FNINIT: cw 0x037F, sw 0, all empty
uint16_t StatusWord(const State& s);  // FNSTSW: sw with TOP merged in
uint16_t ControlWord(const State& s);
void SetControlWord(State& s, uint16_t cw);  // FLDCW
void ClearExceptions(State& s);   // FNCLEX
uint16_t TagWord(const State& s);

// --- loads (push) ----------------------------------------------------
void LoadF32(State& s, uint32_t bits);   // FLD m32 (exact)
void LoadF64(State& s, uint64_t bits);   // FLD m64 (exact)
void LoadF80(State& s, F80 v);           // FLD m80
void LoadSt(State& s, int i);            // FLD st(i)
void LoadI16(State& s, int16_t v);       // FILD m16
void LoadI32(State& s, int32_t v);       // FILD m32
void LoadI64(State& s, int64_t v);       // FILD m64
void LoadConst(State& s, Const k);       // FLD1/FLDZ/FLDPI/... (RC-aware
                                         // like hardware for pi, l2e, ...)

// --- stores (st0 -> destination; caller pops for the P forms) --------
uint32_t StoreF32(State& s);   // FST m32: rounded with RC
uint64_t StoreF64(State& s);   // FST m64: rounded with RC
F80 StoreF80(State& s);        // FSTP m80 (exact)
void StoreSt(State& s, int i); // FST st(i)
int16_t StoreI16(State& s);    // FIST m16: RC, indefinite 0x8000 + IE
int32_t StoreI32(State& s);    // FIST m32: RC, indefinite 0x80000000
int64_t StoreI64(State& s);    // FISTP m64: RC, indefinite INT64_MIN
void Pop(State& s);            // mark st0 empty, ++TOP

// --- arithmetic ------------------------------------------------------
void ArithF32(State& s, Op op, uint32_t bits);  // st0 = st0 op m32
void ArithF64(State& s, Op op, uint64_t bits);  // st0 = st0 op m64
void ArithI16(State& s, Op op, int16_t v);      // FIADD m16 etc.
void ArithI32(State& s, Op op, int32_t v);      // FIADD m32 etc.
void ArithSt0Sti(State& s, Op op, int i);       // st0 = st0 op st(i)
void ArithStiSt0(State& s, Op op, int i);       // st(i) = st(i) op st0
                                                // (caller pops for ...P)

// --- compares (caller pops for FCOMP/FCOMPP/FUCOMPP) ------------------
// quiet = FUCOM semantics (only SNaN raises IE); otherwise FCOM.
void CompareSt(State& s, int i, bool quiet);
void CompareF32(State& s, uint32_t bits);
void CompareF64(State& s, uint64_t bits);
void CompareI16(State& s, int16_t v);   // FICOM m16
void CompareI32(State& s, int32_t v);   // FICOM m32
EFlagsResult CompareStEFlags(State& s, int i, bool quiet);  // FCOMI/FUCOMI
void Test(State& s);    // FTST
void Examine(State& s); // FXAM

// --- unary / misc ----------------------------------------------------
void Chs(State& s);
void Abs(State& s);
void Sqrt(State& s);     // PC-rounded
void RndInt(State& s);   // RC
void Scale(State& s);    // FSCALE
void Xtract(State& s);   // FXTRACT (pushes)
void Prem(State& s);     // FPREM (C0/C1/C3 quotient bits, C2 partial)
void Prem1(State& s);    // FPREM1
void Sin(State& s);      // FSIN   (C2=1 and unchanged if |x| >= 2^63)
void Cos(State& s);      // FCOS
void SinCos(State& s);   // FSINCOS (pushes)
void Ptan(State& s);     // FPTAN (pushes 1.0)
void Patan(State& s);    // FPATAN: st1 = atan2(st1, st0), pop
void F2xm1(State& s);    // st0 = 2^st0 - 1
void Yl2x(State& s);     // st1 = st1 * log2(st0), pop
void Yl2xp1(State& s);   // st1 = st1 * log2(st0 + 1), pop
void Xch(State& s, int i);      // FXCH st(i)
void CopySt(State& s, int i);   // st0 = st(i) (FCMOVcc when taken)
void Free(State& s, int i);     // FFREE st(i)
void IncStp(State& s);
void DecStp(State& s);

// --- environment (32-bit protected-mode formats) ---------------------
void StoreEnv(State& s, uint8_t out[28]);       // FNSTENV, then masks all
void LoadEnv(State& s, const uint8_t in[28]);   // FLDENV
void Save(State& s, uint8_t out[108]);          // FNSAVE, then FNINIT
void Restore(State& s, const uint8_t in[108]);  // FRSTOR

}  // namespace ds1::x87
