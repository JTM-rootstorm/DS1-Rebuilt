// UI scale: the 2D interface keeps its size on large screens.
// Replacements of lifted functions named with `replace` in
// lifter.manifest; FINDINGS.md "Widescreen".
//
// The UI is laid out in screen pixels: UIShell (singleton at 0x7A065C)
// holds the screen size (+0x60/+0x64), each interface is positioned for
// it from its intended resolution, and windows draw pre-transformed
// (XYZRHW) quads at those pixel positions. So at 4K the HUD is a quarter
// of the size it has at 1024x768. Here the shell lays out for a logical
// screen of the real one divided by the scale, the shell's draws are
// scaled up by it, and the pointer it is given is scaled down:
//   0x6DE255  UIShell: set the screen size -> the logical size;
//   0x6DFFF7  UIShell update (dt, mouse x, y) -> the logical position;
//   0x6E02B8, 0x6E046C, 0x6E0592  the shell's draws: while they run,
//   0x654160  Rapi's DrawPrimitive scales XYZRHW positions (in place,
//             restored after the call; the draw copies the vertices);
//   0x66A28F  the compass, drawn by the engine at the screen's top right
//             (0x66A370: x = viewport width - margin, y = margin): scaled
//             about that corner, so its size and margin scale alike.
// The scale is the screen height over 768 (the HUD at 1024x768), never
// below 1; DS1_UI_SCALE overrides it.

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "runtime/lifted.h"
#include "runtime/runtime.h"

namespace recomp::gen {

namespace {

constexpr double kReferenceHeight = 768;

double g_scale = 1;  // the shell's current scale
int g_ui_draw = 0;   // inside a draw to scale
float g_origin_x = 0;  // the point the draw is scaled about

double ScaleFor(int w, int h) {
  if (w <= 0 || h <= 0) return 1;
  if (const char* e = Env("UI_SCALE"); e && *e) {
    const double s = std::strtod(e, nullptr);
    if (s >= 1) return s;
  }
  return h > kReferenceHeight ? h / kReferenceHeight : 1;
}

// Bytes per vertex of an FVF with XYZRHW positions.
uint32_t RhwStride(uint32_t fvf) {
  uint32_t n = 16;
  if (fvf & 0x10) n += 12;  // NORMAL
  if (fvf & 0x20) n += 4;   // PSIZE
  if (fvf & 0x40) n += 4;   // DIFFUSE
  if (fvf & 0x80) n += 4;   // SPECULAR
  static constexpr uint32_t kTexSize[4] = {8, 12, 16, 4};  // TEXCOORDSIZE2, 3, 4, 1
  for (uint32_t i = 0, k = (fvf >> 8) & 0xF; i < k; i++) n += kTexSize[(fvf >> (16 + 2 * i)) & 3];
  return n;
}

float F32(uint32_t va) { return std::bit_cast<float>(Rd32(va)); }
void WrF32(uint32_t va, float v) { Wr32(va, std::bit_cast<uint32_t>(v)); }

}  // namespace

void f_006DE255_lifted(Ctx& c);
void f_006DFFF7_lifted(Ctx& c);
void f_006E02B8_lifted(Ctx& c);
void f_006E046C_lifted(Ctx& c);
void f_006E0592_lifted(Ctx& c);
void f_0066A28F_lifted(Ctx& c);
void f_00654160_lifted(Ctx& c);

// UIShell: screen size (thiscall: width, height).
void f_006DE255(Ctx& c) {
  const int w = int32_t(Rd32(c.esp + 4)), h = int32_t(Rd32(c.esp + 8));
  g_scale = ScaleFor(w, h);
  if (g_scale != 1) {
    Wr32(c.esp + 4, uint32_t(std::lround(w / g_scale)));
    Wr32(c.esp + 8, uint32_t(std::lround(h / g_scale)));
  }
  f_006DE255_lifted(c);
}

// UIShell update (thiscall: double dt, mouse x, mouse y).
void f_006DFFF7(Ctx& c) {
  if (g_scale != 1) {
    Wr32(c.esp + 12, uint32_t(int32_t(std::floor(int32_t(Rd32(c.esp + 12)) / g_scale))));
    Wr32(c.esp + 16, uint32_t(int32_t(std::floor(int32_t(Rd32(c.esp + 16)) / g_scale))));
  }
  f_006DFFF7_lifted(c);
}

// Runs a draw with its 2D quads scaled about (origin_x, 0).
void ScaledDraw(Ctx& c, void (*draw)(Ctx&), float origin_x) {
  const float saved = g_origin_x;
  g_origin_x = origin_x;
  g_ui_draw++;
  draw(c);
  g_ui_draw--;
  g_origin_x = saved;
}

void f_006E02B8(Ctx& c) { ScaledDraw(c, f_006E02B8_lifted, 0); }
void f_006E046C(Ctx& c) { ScaledDraw(c, f_006E046C_lifted, 0); }
void f_006E0592(Ctx& c) { ScaledDraw(c, f_006E0592_lifted, 0); }

// The compass (thiscall; +0 margin, +4 centre x = viewport width -
// margin): scaled about the screen's top right corner.
void f_0066A28F(Ctx& c) {
  const float right = float(int32_t(Rd32(c.ecx + 4)) + int32_t(Rd32(c.ecx)));
  ScaledDraw(c, f_0066A28F_lifted, right);
}

// Rapi DrawPrimitive (thiscall: type, vertices, count, FVF, textures,
// texture count). Pixel edges stay on pixel edges: D3D's pixel centres
// are integers, so x' = o + (x + 0.5 - o) * s - 0.5 about origin o.
void f_00654160(Ctx& c) {
  const uint32_t verts = Rd32(c.esp + 8), count = Rd32(c.esp + 12), fvf = Rd32(c.esp + 16);
  if (!g_ui_draw || g_scale == 1 || (fvf & 0xE) != 4 || !verts || !count)
    return f_00654160_lifted(c);
  const uint32_t stride = RhwStride(fvf);
  std::vector<uint32_t> saved(size_t(count) * 2);
  const float s = float(g_scale), ox = g_origin_x;
  for (uint32_t i = 0; i < count; i++) {
    const uint32_t v = verts + i * stride;
    saved[2 * i] = Rd32(v);
    saved[2 * i + 1] = Rd32(v + 4);
    WrF32(v, ox + (F32(v) + 0.5f - ox) * s - 0.5f);
    WrF32(v + 4, (F32(v + 4) + 0.5f) * s - 0.5f);
  }
  f_00654160_lifted(c);
  for (uint32_t i = 0; i < count; i++) {
    Wr32(verts + i * stride, saved[2 * i]);
    Wr32(verts + i * stride + 4, saved[2 * i + 1]);
  }
}

}  // namespace recomp::gen
