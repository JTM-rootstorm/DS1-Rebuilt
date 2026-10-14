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
// The software cursor (0x656E02, on the main thread and the game's cursor
// thread) blends a cursor image ({+0 width, +4 height, +0xC ARGB pixels})
// onto the screen at the pointer minus its hotspot:
//   0x656D49  set the cursor (texture, hotspot x, y) -> hotspot scaled;
//             its call from 0x656E02 (0x656F15) restores an earlier
//             request, already scaled, and is left alone;
//   0x65754B  take on a cursor image -> a copy scaled by the same factor
//             (bilinear, premultiplied alpha; kept per image and scale).
// When the scale changes (menus to game), the last request is set again.
// The scale is the screen height over 768 (the HUD at 1024x768), never
// below 1; DS1_UI_SCALE overrides it.

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <mutex>
#include <tuple>
#include <vector>

#include "runtime/hle/kernel32_state.h"
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

// The software cursor object (0x656E02's `this`).
constexpr uint32_t kCursor = 0x7A0680;
// Return address of 0x656E02's own call of 0x656D49.
constexpr uint32_t kCursorRestoreCall = 0x656F15;
constexpr uint32_t kCursorSet = 0x656D49;

// The last cursor the game asked for (unscaled) and the scale its
// hotspot and image were given.
struct CursorRequest {
  uint32_t texture = 0, hx = 0, hy = 0;
  bool valid = false;
};
CursorRequest g_cursor;
double g_cursor_scale = 1;

std::mutex g_images_mu;
std::map<std::tuple<uint32_t, uint32_t, uint32_t, double>, uint32_t> g_images;

// A copy of cursor image `img` scaled by s: bilinear on premultiplied
// colour, so transparent pixels do not bleed into the edges.
uint32_t ScaledImage(uint32_t img, double s) {
  const uint32_t w = Rd32(img), h = Rd32(img + 4), px = Rd32(img + 12);
  if (!w || !h || !px || w > 1024 || h > 1024) return img;
  std::lock_guard<std::mutex> l(g_images_mu);
  const auto key = std::make_tuple(img, w, h, s);
  if (auto it = g_images.find(key); it != g_images.end()) return it->second;
  const uint32_t w2 = uint32_t(std::lround(w * s)), h2 = uint32_t(std::lround(h * s));
  const uint32_t out = HeapAllocProcess(16 + w2 * h2 * 4, true);
  if (!out) return img;
  Wr32(out, w2);
  Wr32(out + 4, h2);
  Wr32(out + 8, Rd32(img + 8));
  Wr32(out + 12, out + 16);
  auto at = [&](uint32_t x, uint32_t y, float* c) {
    const uint32_t p = Rd32(px + (y * w + x) * 4);
    const float a = float(p >> 24) / 255.0f;
    c[0] = a;
    for (int i = 0; i < 3; i++) c[i + 1] = float((p >> (16 - 8 * i)) & 0xFF) * a;
  };
  for (uint32_t y = 0; y < h2; y++)
    for (uint32_t x = 0; x < w2; x++) {
      const double u = std::clamp((x + 0.5) / s - 0.5, 0.0, double(w - 1));
      const double v = std::clamp((y + 0.5) / s - 0.5, 0.0, double(h - 1));
      const uint32_t x0 = uint32_t(u), y0 = uint32_t(v);
      const uint32_t x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
      const float fx = float(u - x0), fy = float(v - y0);
      float c00[4], c10[4], c01[4], c11[4], c[4];
      at(x0, y0, c00);
      at(x1, y0, c10);
      at(x0, y1, c01);
      at(x1, y1, c11);
      for (int i = 0; i < 4; i++)
        c[i] = (c00[i] * (1 - fx) + c10[i] * fx) * (1 - fy) + (c01[i] * (1 - fx) + c11[i] * fx) * fy;
      uint32_t p = 0;
      if (c[0] > 0) {
        p = uint32_t(std::lround(c[0] * 255.0f)) << 24;
        for (int i = 0; i < 3; i++)
          p |= uint32_t(std::clamp(std::lround(c[i + 1] / c[0]), 0L, 255L)) << (16 - 8 * i);
      }
      Wr32(out + 16 + (y * w2 + x) * 4, p);
    }
  g_images.emplace(key, out);
  return out;
}

}  // namespace

void f_006DE255_lifted(Ctx& c);
void f_006DFFF7_lifted(Ctx& c);
void f_006E02B8_lifted(Ctx& c);
void f_006E046C_lifted(Ctx& c);
void f_006E0592_lifted(Ctx& c);
void f_0066A28F_lifted(Ctx& c);
void f_00654160_lifted(Ctx& c);
void f_00656D49_lifted(Ctx& c);
void f_0065754B_lifted(Ctx& c);

// UIShell: screen size (thiscall: width, height).
void f_006DE255(Ctx& c) {
  const int w = int32_t(Rd32(c.esp + 4)), h = int32_t(Rd32(c.esp + 8));
  g_scale = ScaleFor(w, h);
  // A cursor set at another scale (in the menus): set it again.
  if (g_cursor.valid && g_scale != g_cursor_scale && Rd32(kCursor)) {
    const uint32_t ecx = c.ecx;
    c.ecx = Rd32(kCursor);
    CallGuest(c, kCursorSet, {g_cursor.texture, g_cursor.hx, g_cursor.hy});
    c.ecx = ecx;
  }
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

// Set the cursor (thiscall: texture, hotspot x, y).
void f_00656D49(Ctx& c) {
  if (Rd32(c.esp) != kCursorRestoreCall) {
    g_cursor = {Rd32(c.esp + 4), Rd32(c.esp + 8), Rd32(c.esp + 12), true};
    g_cursor_scale = g_scale;
    if (g_scale != 1) {
      Wr32(c.esp + 8, uint32_t(std::lround(int32_t(g_cursor.hx) * g_scale)));
      Wr32(c.esp + 12, uint32_t(std::lround(int32_t(g_cursor.hy) * g_scale)));
    }
  }
  f_00656D49_lifted(c);
}

// The cursor takes on an image (thiscall: image).
void f_0065754B(Ctx& c) {
  const uint32_t img = Rd32(c.esp + 4);
  if (img && g_cursor_scale != 1) Wr32(c.esp + 4, ScaledImage(img, g_cursor_scale));
  f_0065754B_lifted(c);
}

}  // namespace recomp::gen
