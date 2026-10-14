// The software cursor blended on the GPU. Replacements of lifted
// functions named with `replace` in lifter.manifest; FINDINGS.md
// "Software cursor on the GPU".
//
// 0x656E02 (thiscall: bool on_primary) draws the cursor (object at
// 0x7A0680; +0x30 the AppModule: +0x604 primary, +0x608 back buffer).
// The main thread calls it with false before each present, and it
// blends the cursor into the back buffer at R (pointer minus hotspot,
// clipped) through the cursor's save (+0x10C) and work (+0x110)
// system-memory surfaces:
//   0x6576EA(save, size, back buffer, R)   the pixels under the cursor
//   0x6576EA(work, size, save, size)
//   0x65780F(work, size, image, offset)    the image blended in (CPU)
//   0x6576EA(back buffer, R, work, size)   written back
// 0x6576EA (thiscall: dst surface, dst rect, src surface, src rect) is a
// Blt while the game's window is the foreground window and both rects
// are non-empty. The first one reads the frame back, so the main thread
// waited for all of the frame's GPU work every frame: CPU and GPU ran
// one after the other (49.9 fps at 5120x2880 instead of 60).
//
// Here, on that path, the save starts an asynchronous readback
// (gfx StartReadBack), the copy and the blend are skipped, and the
// write-back is the image blended on the GPU (BlitBlend; the game's
// d + ((s - d) * a >> 8) per channel, within one step of GPU blending).
// The game's cursor thread (0x656862) calls 0x656E02(true) every ~30 ms
// to move the cursor on the primary between frames (and, after 0.5 s
// without a frame, to show the busy cursor): it restores the old
// position from the save surface and saves the new one, reading the
// primary back. Each such read waited for the GPU while holding the gfx
// device, so with the mouse moving the frames that overlapped one took
// 30 ms instead of 16.7. It now runs only when the main thread has not
// drawn the cursor for 100 ms (loading, a stall); while frames come, the
// cursor moves with them. Its first Blt finishes the pending readback
// into the save surface. DS1_SOFT_CURSOR=1 keeps the game's own blending
// and its cursor thread as it was.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <mutex>

#include "runtime/hle/com.h"
#include "runtime/hle/ddraw_impl.h"
#include "runtime/lifted.h"
#include "runtime/runtime.h"

namespace recomp::gen {

namespace {

// The game's AppModule pointer (+0x70 its window).
constexpr uint32_t kAppModule = 0x79CF7C;
// GetForegroundWindow's import address.
constexpr uint32_t kGetForegroundWindow = 0x720368;

bool Soft() {
  static const bool soft = [] {
    const char* e = Env("SOFT_CURSOR");
    return e && *e && *e != '0';
  }();
  return soft;
}

// Inside a main-thread 0x656E02 that blends on the GPU.
thread_local bool t_gpu = false;

// When the main thread last drew the cursor, and how long after that the
// cursor thread may draw it.
std::atomic<int64_t> g_main_drawn{0};
constexpr int64_t kMainActiveNs = 100'000'000;

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::mutex g_mu;
// The save's readback in flight: into `rect` of the save surface.
struct Pending {
  uint32_t ticket = 0;
  uint32_t surface = 0;
  gfx::Rect rect{};
} g_pending;
// What 0x65780F was asked to blend: the image and the offset into it.
struct Blend {
  uint32_t image = 0;
  int32_t x = 0, y = 0;
} g_blend;

gfx::Rect ReadRect(uint32_t va) {
  return {int32_t(Rd32(va)), int32_t(Rd32(va + 4)), int32_t(Rd32(va + 8)),
          int32_t(Rd32(va + 12))};
}

bool Empty(const gfx::Rect& r) { return r.right == r.left || r.bottom == r.top; }

// Finishes (into the save surface) or drops (into = false) the pending
// readback. Caller holds g_mu.
void SettlePending(bool into) {
  if (!g_pending.ticket) return;
  gfx::Device* g = ddraw::Gfx();
  void* dst = nullptr;
  uint32_t pitch = 0;
  gfx::Format fmt = gfx::Format::Unknown;
  const gfx::Rect& r = g_pending.rect;
  if (auto* s = into ? com::As<ddraw::Surface>(g_pending.surface) : nullptr;
      s && s->mem && r.left >= 0 && r.top >= 0 && r.right <= s->width && r.bottom <= s->height &&
      !gfx::IsCompressed(s->fmt)) {
    dst = GPtr(s->mem + uint32_t(r.top) * s->pitch + uint32_t(r.left) * gfx::BitsPerPixel(s->fmt) / 8);
    pitch = s->pitch;
    fmt = s->fmt;
  }
  g->FinishReadBack(g_pending.ticket, dst, pitch, fmt);
  g_pending = {};
}

}  // namespace

void f_00656E02_lifted(Ctx& c);
void f_006576EA_lifted(Ctx& c);
void f_0065780F_lifted(Ctx& c);

// Draw the cursor (thiscall: bool on_primary).
void f_00656E02(Ctx& c) {
  if (Soft()) return f_00656E02_lifted(c);
  if (Rd32(c.esp + 4) & 0xFF) {
    if (NowNs() - g_main_drawn.load() < kMainActiveNs) {
      c.esp += 4 + 4;
      return;
    }
    return f_00656E02_lifted(c);
  }
  {
    std::lock_guard<std::mutex> l(g_mu);
    g_blend = {};
  }
  t_gpu = true;
  f_00656E02_lifted(c);
  t_gpu = false;
  g_main_drawn.store(NowNs());
}

// Blt between cursor surfaces (thiscall: dst, dst rect, src, src rect).
void f_006576EA(Ctx& c) {
  if (!t_gpu) {
    {
      std::lock_guard<std::mutex> l(g_mu);
      SettlePending(true);
    }
    return f_006576EA_lifted(c);
  }
  const uint32_t dst = Rd32(c.esp + 4), src = Rd32(c.esp + 12);
  const gfx::Rect dr = ReadRect(Rd32(c.esp + 8)), sr = ReadRect(Rd32(c.esp + 16));
  const uint32_t ecx = c.ecx;
  c.esp += 4 + 16;
  const uint32_t app = Rd32(kAppModule);
  if (Empty(dr) || Empty(sr)) return;
  if (CallGuest(c, Rd32(kGetForegroundWindow), {}) != Rd32(app + 0x70)) return;
  const uint32_t back = Rd32(Rd32(ecx + 0x30) + 0x608);
  gfx::Device* g = ddraw::Gfx();
  std::lock_guard<std::mutex> l(g_mu);
  if (src == back) {
    // The save: read back later, if the cursor thread needs it.
    SettlePending(false);
    g_pending = {g->StartReadBack(sr), dst, dr};
  } else if (dst == back && g_blend.image) {
    const uint32_t img = g_blend.image;
    const int32_t iw = int32_t(Rd32(img)), ih = int32_t(Rd32(img + 4));
    const int32_t w = dr.right - dr.left, h = dr.bottom - dr.top;
    if (g_blend.x < 0 || g_blend.y < 0 || g_blend.x + w > iw || g_blend.y + h > ih) return;
    g->BlitBlend(GPtr(Rd32(img + 12)), uint32_t(iw) * 4,
                 {g_blend.x, g_blend.y, g_blend.x + w, g_blend.y + h}, dr);
  }
}

// Blend the cursor image into a surface (thiscall: surface, rect, image
// {+0 width, +4 height, +0xC ARGB pixels}, offset into the image).
void f_0065780F(Ctx& c) {
  if (!t_gpu) return f_0065780F_lifted(c);
  const uint32_t img = Rd32(c.esp + 12), off = Rd32(c.esp + 16);
  c.esp += 4 + 16;
  std::lock_guard<std::mutex> l(g_mu);
  g_blend = {img, int32_t(Rd32(off)), int32_t(Rd32(off + 4))};
}

}  // namespace recomp::gen
