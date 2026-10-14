// Wide resolutions for the options menu. Replacements of lifted
// functions named with `replace` in lifter.manifest; FINDINGS.md
// "Widescreen".
//
// The menu (0x4B49B7) offers the display modes (EnumDisplayModes) whose
// "WxHxBPP" is listed in config:options:video:allowed_res, and the game
// checks a mode against its system_detail tier; both stop at 1024x768.
// Both get the larger 4:3 and 5:4 sizes, the wide ones and the desktop's
// own size; the menu still only shows those the display offers.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>

#include "runtime/display.h"
#include "runtime/lifted.h"
#include "runtime/runtime.h"

namespace recomp::gen {

namespace {

// std::vector<Resolution>::insert(iterator, const Resolution&), thiscall.
constexpr uint32_t kResolutionsInsert = 0x661EE7;
// String::assign(const char*, size_t), thiscall.
constexpr uint32_t kStringAssign = 0x402BE9;
// Return address of the menu's call reading allowed_res entries.
constexpr uint32_t kAllowedResRead = 0x4B4A31;

constexpr uint32_t kMinVideoMemory = 32u << 20;

struct Size {
  uint32_t w, h;
};

constexpr Size kSizes[] = {{1152, 864},  {1280, 960},  {1280, 1024}, {1600, 1200},
                           {1280, 720},  {1366, 768},  {1600, 900},  {1920, 1080},
                           {1920, 1200}, {2560, 1080}, {2560, 1440}, {3440, 1440},
                           {3840, 2160}};

// The sizes added after the desktop's: index -1 is the desktop.
bool SizeAt(int i, uint32_t* w, uint32_t* h) {
  if (i < 0) {
    int dw, dh;
    DesktopSize(&dw, &dh);
    *w = uint32_t(dw);
    *h = uint32_t(dh);
    return true;
  }
  if (i >= int(std::size(kSizes))) return false;
  *w = kSizes[i].w;
  *h = kSizes[i].h;
  return true;
}

int g_allowed_next = -1;  // next size to append to allowed_res

}  // namespace

void f_0051E28C_lifted(Ctx& c);
void f_00496307_lifted(Ctx& c);

// Next key/value of a gas block (thiscall: the iterator {block, index},
// key and value strings); returns 0 at the end. For the menu's
// allowed_res, the end comes after "WxHx32" for each added size.
void f_00496307(Ctx& c) {
  const uint32_t ret = Rd32(c.esp), value = Rd32(c.esp + 8);
  const bool allowed_res = ret == kAllowedResRead;
  if (allowed_res && Rd32(c.ecx + 4) == 0) g_allowed_next = -1;
  f_00496307_lifted(c);
  if (!allowed_res || c.eax) return;
  uint32_t w, h;
  if (!SizeAt(g_allowed_next++, &w, &h)) return;
  char text[32];
  const int n = std::snprintf(text, sizeof text, "%ux%ux32", w, h);
  const uint32_t ecx = c.ecx;
  c.esp -= 32;
  std::memcpy(GPtr(c.esp), text, size_t(n) + 1);
  c.ecx = value;
  CallGuest(c, kStringAssign, {c.esp, uint32_t(n)});
  c.esp += 32;
  c.ecx = ecx;
  c.eax = 1;
}

// Files a parsed system_detail tier (thiscall; the record is the third
// argument: +0 video memory in bytes, +8 detail, +0xC shadow texture
// size, +0x10 std::vector of {width, height, max_back_buffers}). There is
// a tier per video memory size (8, 12, 16, 32, 64 MB, two detail levels
// each); those of 32 MB and up get the added sizes, with the back buffer
// count of their largest entry.
void f_0051E28C(Ctx& c) {
  const uint32_t rec = Rd32(c.esp + 12);
  const uint32_t vec = rec + 0x10;
  if (Rd32(rec) >= kMinVideoMemory) {
    // The largest listed size's back buffer count.
    uint32_t back_buffers = 0, area = 0;
    for (uint32_t p = Rd32(vec + 4); p < Rd32(vec + 8); p += 12)
      if (Rd32(p) * Rd32(p + 4) > area) {
        area = Rd32(p) * Rd32(p + 4);
        back_buffers = Rd32(p + 8);
      }
    auto add = [&](uint32_t w, uint32_t h) {
      for (uint32_t p = Rd32(vec + 4); p < Rd32(vec + 8); p += 12)
        if (Rd32(p) == w && Rd32(p + 4) == h) return;
      const uint32_t ecx = c.ecx;
      c.esp -= 12;
      Wr32(c.esp, w);
      Wr32(c.esp + 4, h);
      Wr32(c.esp + 8, back_buffers);
      c.ecx = vec;
      CallGuest(c, kResolutionsInsert, {Rd32(vec + 8), c.esp});
      c.esp += 12;
      c.ecx = ecx;
    };
    uint32_t w, h;
    for (int i = -1; SizeAt(i, &w, &h); i++) add(w, h);
  }
  f_0051E28C_lifted(c);
}

}  // namespace recomp::gen
