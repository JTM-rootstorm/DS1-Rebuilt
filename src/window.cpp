// The main window's screen rectangle. Replacements of lifted functions
// named with `replace` in lifter.manifest; FINDINGS.md "Widescreen".
//
// In fullscreen (the only mode this build runs) the game creates its
// window as a WS_POPUP covering the screen rectangle 0x43A408 returns
// (GetSystemMetrics SM_CXSCREEN/SM_CYSCREEN) and keeps that rectangle in
// its AppModule (+0xA0..+0xAC): every WM_ACTIVATEAPP moves the window
// back to it, and WM_GETMINMAXINFO makes its size the window's maximum.
// It asks before anything has brought up the host display, when the
// runtime can only answer with its 1024x768 placeholder, so the window
// stayed pinned at 1024x768 whatever the display mode. Here both screen
// rectangles are the host desktop, as on Windows: the desktop the game
// starts on.

#include "runtime/display.h"
#include "runtime/lifted.h"
#include "runtime/runtime.h"

namespace recomp::gen {

namespace {

// The primary screen's origin, which 0x43A408 adds (0, 0).
constexpr uint32_t kScreenX = 0x720650, kScreenY = 0x720654;

void WriteDesktopRect(uint32_t rect) {
  int w, h;
  DesktopSize(&w, &h);
  const uint32_t x = Rd32(kScreenX), y = Rd32(kScreenY);
  Wr32(rect + 0, x);
  Wr32(rect + 4, y);
  Wr32(rect + 8, x + uint32_t(w));
  Wr32(rect + 12, y + uint32_t(h));
}

}  // namespace

// RECT* (cdecl, out pointer on the stack, returned in EAX): the primary
// screen.
void f_0043A408(Ctx& c) {
  const uint32_t rect = Rd32(c.esp + 4);
  WriteDesktopRect(rect);
  c.eax = rect;
  c.esp += 4;
}

// The same for the virtual screen (all monitors; the original falls back
// to the primary screen when the system reports none). The game only
// shows itself on one.
void f_0043A44B(Ctx& c) {
  const uint32_t rect = Rd32(c.esp + 4);
  WriteDesktopRect(rect);
  c.eax = rect;
  c.esp += 4;
}

}  // namespace recomp::gen
