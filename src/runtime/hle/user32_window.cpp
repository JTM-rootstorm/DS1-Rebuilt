// user32: window classes, windows, message queues and the SDL3 event pump.
//
// Every top-level window gets an SDL window sized to its client area; child
// windows are bookkeeping only. SDL events are translated into WM_* messages
// on the queue of the thread that owns the window, and DispatchMessage calls
// the guest WndProc. Messages Windows sends synchronously (creation,
// activation, size) are delivered by calling the WndProc directly.

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "runtime/hle/hle.h"
#include "runtime/hle/input_state.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/hle/user32_window.h"
#include "runtime/hle/win32.h"
#include "runtime/log.h"

namespace ds1 {

namespace {

// ---- messages and styles -----------------------------------------------------
enum : uint32_t {
  WM_NULL = 0x0000, WM_CREATE = 0x0001, WM_DESTROY = 0x0002, WM_MOVE = 0x0003,
  WM_SIZE = 0x0005, WM_ACTIVATE = 0x0006, WM_SETFOCUS = 0x0007,
  WM_KILLFOCUS = 0x0008, WM_ENABLE = 0x000A, WM_SETTEXT = 0x000C,
  WM_GETTEXT = 0x000D, WM_GETTEXTLENGTH = 0x000E, WM_PAINT = 0x000F,
  WM_CLOSE = 0x0010, WM_QUIT = 0x0012, WM_ERASEBKGND = 0x0014,
  WM_SHOWWINDOW = 0x0018, WM_ACTIVATEAPP = 0x001C, WM_SETCURSOR = 0x0020,
  WM_GETMINMAXINFO = 0x0024, WM_WINDOWPOSCHANGING = 0x0046,
  WM_WINDOWPOSCHANGED = 0x0047, WM_NCCREATE = 0x0081, WM_NCDESTROY = 0x0082,
  WM_NCCALCSIZE = 0x0083, WM_NCHITTEST = 0x0084, WM_NCACTIVATE = 0x0086,
  WM_KEYDOWN = 0x0100, WM_KEYUP = 0x0101, WM_CHAR = 0x0102,
  WM_SYSKEYDOWN = 0x0104, WM_SYSKEYUP = 0x0105, WM_SYSCHAR = 0x0106,
  WM_SYSCOMMAND = 0x0112, WM_MOUSEMOVE = 0x0200, WM_LBUTTONDOWN = 0x0201,
  WM_LBUTTONUP = 0x0202, WM_LBUTTONDBLCLK = 0x0203, WM_RBUTTONDOWN = 0x0204,
  WM_RBUTTONUP = 0x0205, WM_RBUTTONDBLCLK = 0x0206, WM_MBUTTONDOWN = 0x0207,
  WM_MBUTTONUP = 0x0208, WM_MBUTTONDBLCLK = 0x0209, WM_MOUSEWHEEL = 0x020A,
};
constexpr uint32_t WS_CHILD = 0x40000000, WS_VISIBLE = 0x10000000,
                   WS_CAPTION = 0x00C00000,
                   WS_THICKFRAME = 0x00040000;
constexpr uint32_t CS_DBLCLKS = 0x0008;
constexpr int32_t GWL_WNDPROC = -4, GWL_HINSTANCE = -6, GWL_HWNDPARENT = -8,
                  GWL_STYLE = -16, GWL_EXSTYLE = -20, GWL_USERDATA = -21,
                  GWL_ID = -12;

struct WndClass {
  std::string name;  // lower case
  uint32_t style = 0, wndproc = 0, cls_extra = 0, wnd_extra = 0, hinst = 0;
  uint32_t icon = 0, cursor = 0, brush = 0;
  uint16_t atom = 0;
};

struct Window {
  uint32_t hwnd = 0;
  std::string cls;
  uint32_t wndproc = 0, style = 0, exstyle = 0, parent = 0, id = 0;
  uint32_t hinst = 0, userdata = 0, class_style = 0;
  int x = 0, y = 0, w = 0, h = 0;  // window rect (screen)
  std::string title;
  std::vector<uint8_t> extra;      // cbWndExtra bytes (GetWindowLong >= 0)
  uint32_t tid = 0;
  bool visible = false, enabled = true, destroyed = false;
  SDL_Window* sdl = nullptr;
};

struct Msg {
  uint32_t hwnd, msg, wparam, lparam, time;
  int32_t ptx, pty;
};

struct Queue {
  std::deque<Msg> msgs;
  bool quit = false;
  uint32_t quit_code = 0;
};

std::recursive_mutex g_mu;
std::map<std::string, WndClass> g_classes;
std::map<uint32_t, Window> g_windows;
std::map<uint32_t, Queue> g_queues;  // by tid
std::condition_variable_any g_queue_cv;
uint32_t g_next_hwnd = 0x00010010;
uint16_t g_next_atom = 0xC100;
uint32_t g_focus = 0, g_active = 0;
int g_cursor_count = 0;  // ShowCursor display counter
int32_t g_mouse_x = 0, g_mouse_y = 0;  // screen coordinates
bool g_sdl_up = false;
std::thread::id g_main_thread_id = std::this_thread::get_id();

const auto g_t0 = std::chrono::steady_clock::now();
uint32_t MsgTime() {
  using namespace std::chrono;
  return uint32_t(duration_cast<milliseconds>(steady_clock::now() - g_t0).count());
}

Window* Win(uint32_t hwnd) {
  auto it = g_windows.find(hwnd);
  return it == g_windows.end() || it->second.destroyed ? nullptr : &it->second;
}

// Client-area size for a window rect, mirroring what AdjustWindowRect added.
void ClientSize(const Window& w, int* cw, int* ch) {
  if (w.style & WS_CHILD || !(w.style & (WS_CAPTION | WS_THICKFRAME))) {
    *cw = w.w;
    *ch = w.h;
    return;
  }
  int border = (w.style & WS_THICKFRAME) ? 4 : 3;
  int caption = (w.style & WS_CAPTION) == WS_CAPTION ? 19 : 0;
  *cw = std::max(0, w.w - 2 * border);
  *ch = std::max(0, w.h - 2 * border - caption);
}

void PostTo(uint32_t tid, const Msg& m) {
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    g_queues[tid].msgs.push_back(m);
  }
  g_queue_cv.notify_all();
}

void Post(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
  uint32_t tid;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(hwnd);
    tid = w ? w->tid : CurrentThread()->tid;
  }
  PostTo(tid, Msg{hwnd, msg, wp, lp, MsgTime(), g_mouse_x, g_mouse_y});
}

// Synchronous delivery to the guest WndProc (SendMessage semantics).
uint32_t Send(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
  uint32_t proc;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(hwnd);
    if (!w) return 0;
    if (w->tid != CurrentThread()->tid)
      WarnF("SendMessage %X to hwnd %X owned by thread %X from thread %X: "
            "delivered on the calling thread\n",
            msg, hwnd, w->tid, CurrentThread()->tid);
    proc = w->wndproc;
  }
  Ctx& c = CurrentThread()->ctx;
  return CallGuest(c, proc, {hwnd, msg, wp, lp});
}

uint32_t MkLparam(int lo, int hi) {
  return (uint32_t(uint16_t(lo))) | (uint32_t(uint16_t(hi)) << 16);
}

// Top-level window under SDL window id.
uint32_t HwndForSdl(SDL_WindowID id) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  for (auto& [h, w] : g_windows)
    if (w.sdl && SDL_GetWindowID(w.sdl) == id && !w.destroyed) return h;
  return 0;
}

// ---- SDL key -> Windows VK ----------------------------------------------------

uint8_t VkFromScancode(SDL_Scancode sc) {
  if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return uint8_t('A' + (sc - SDL_SCANCODE_A));
  if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return uint8_t('1' + (sc - SDL_SCANCODE_1));
  if (sc == SDL_SCANCODE_0) return '0';
  if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) return uint8_t(0x70 + (sc - SDL_SCANCODE_F1));
  switch (sc) {
    case SDL_SCANCODE_RETURN: return 0x0D;
    case SDL_SCANCODE_ESCAPE: return 0x1B;
    case SDL_SCANCODE_BACKSPACE: return 0x08;
    case SDL_SCANCODE_TAB: return 0x09;
    case SDL_SCANCODE_SPACE: return 0x20;
    case SDL_SCANCODE_MINUS: return 0xBD;
    case SDL_SCANCODE_EQUALS: return 0xBB;
    case SDL_SCANCODE_LEFTBRACKET: return 0xDB;
    case SDL_SCANCODE_RIGHTBRACKET: return 0xDD;
    case SDL_SCANCODE_BACKSLASH: return 0xDC;
    case SDL_SCANCODE_SEMICOLON: return 0xBA;
    case SDL_SCANCODE_APOSTROPHE: return 0xDE;
    case SDL_SCANCODE_GRAVE: return 0xC0;
    case SDL_SCANCODE_COMMA: return 0xBC;
    case SDL_SCANCODE_PERIOD: return 0xBE;
    case SDL_SCANCODE_SLASH: return 0xBF;
    case SDL_SCANCODE_CAPSLOCK: return 0x14;
    case SDL_SCANCODE_PRINTSCREEN: return 0x2C;
    case SDL_SCANCODE_SCROLLLOCK: return 0x91;
    case SDL_SCANCODE_PAUSE: return 0x13;
    case SDL_SCANCODE_INSERT: return 0x2D;
    case SDL_SCANCODE_HOME: return 0x24;
    case SDL_SCANCODE_PAGEUP: return 0x21;
    case SDL_SCANCODE_DELETE: return 0x2E;
    case SDL_SCANCODE_END: return 0x23;
    case SDL_SCANCODE_PAGEDOWN: return 0x22;
    case SDL_SCANCODE_RIGHT: return 0x27;
    case SDL_SCANCODE_LEFT: return 0x25;
    case SDL_SCANCODE_DOWN: return 0x28;
    case SDL_SCANCODE_UP: return 0x26;
    case SDL_SCANCODE_NUMLOCKCLEAR: return 0x90;
    case SDL_SCANCODE_KP_DIVIDE: return 0x6F;
    case SDL_SCANCODE_KP_MULTIPLY: return 0x6A;
    case SDL_SCANCODE_KP_MINUS: return 0x6D;
    case SDL_SCANCODE_KP_PLUS: return 0x6B;
    case SDL_SCANCODE_KP_ENTER: return 0x0D;
    case SDL_SCANCODE_KP_1: return 0x61;
    case SDL_SCANCODE_KP_2: return 0x62;
    case SDL_SCANCODE_KP_3: return 0x63;
    case SDL_SCANCODE_KP_4: return 0x64;
    case SDL_SCANCODE_KP_5: return 0x65;
    case SDL_SCANCODE_KP_6: return 0x66;
    case SDL_SCANCODE_KP_7: return 0x67;
    case SDL_SCANCODE_KP_8: return 0x68;
    case SDL_SCANCODE_KP_9: return 0x69;
    case SDL_SCANCODE_KP_0: return 0x60;
    case SDL_SCANCODE_KP_PERIOD: return 0x6E;
    case SDL_SCANCODE_LCTRL: return 0xA2;
    case SDL_SCANCODE_RCTRL: return 0xA3;
    case SDL_SCANCODE_LSHIFT: return 0xA0;
    case SDL_SCANCODE_RSHIFT: return 0xA1;
    case SDL_SCANCODE_LALT: return 0xA4;
    case SDL_SCANCODE_RALT: return 0xA5;
    case SDL_SCANCODE_LGUI: return 0x5B;
    case SDL_SCANCODE_RGUI: return 0x5C;
    case SDL_SCANCODE_APPLICATION: return 0x5D;
    default: return 0;
  }
}

// Messages use the generic VK for modifiers; key state tracks both.
uint8_t GenericVk(uint8_t vk) {
  switch (vk) {
    case 0xA0: case 0xA1: return 0x10;
    case 0xA2: case 0xA3: return 0x11;
    case 0xA4: case 0xA5: return 0x12;
    default: return vk;
  }
}

uint32_t MouseKeys() {
  uint32_t mk = 0;
  if (g_key_down[0x01]) mk |= 0x01;  // MK_LBUTTON
  if (g_key_down[0x02]) mk |= 0x02;  // MK_RBUTTON
  if (g_key_down[0x10]) mk |= 0x04;  // MK_SHIFT
  if (g_key_down[0x11]) mk |= 0x08;  // MK_CONTROL
  if (g_key_down[0x04]) mk |= 0x10;  // MK_MBUTTON
  return mk;
}

void SetKey(uint8_t vk, bool down) {
  if (down && !g_key_down[vk]) {
    g_key_pressed_since[vk] = 1;
    g_key_toggled[vk] = !g_key_toggled[vk];
  }
  g_key_down[vk] = down;
}

void WindowOrigin(uint32_t hwnd, int* x, int* y) {
  *x = *y = 0;
  std::lock_guard<std::recursive_mutex> l(g_mu);
  if (Window* w = Win(hwnd)) {
    int cw, ch;
    ClientSize(*w, &cw, &ch);
    *x = w->x + (w->w - cw) / 2;
    *y = w->y + (w->h - ch) - (w->w - cw) / 2;
  }
}

void ActivateApp(uint32_t hwnd, bool active) {
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    if (active) {
      if (g_active == hwnd) return;  // already active: nothing to report
      g_active = g_focus = hwnd;
    } else {
      if (g_active != hwnd) return;
      g_active = g_focus = 0;
    }
  }
  Post(hwnd, WM_ACTIVATEAPP, active ? 1 : 0, 0);
  Post(hwnd, WM_ACTIVATE, active ? 1 : 0, 0);
  Post(hwnd, active ? WM_SETFOCUS : WM_KILLFOCUS, 0, 0);
}

void HandleSdlEvent(const SDL_Event& e) {
  switch (e.type) {
    case SDL_EVENT_QUIT: {
      std::lock_guard<std::recursive_mutex> l(g_mu);
      for (auto& [h, w] : g_windows)
        if (w.sdl && !w.destroyed) Post(h, WM_CLOSE, 0, 0);
      break;
    }
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
      if (uint32_t h = HwndForSdl(e.window.windowID)) Post(h, WM_CLOSE, 0, 0);
      break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
      if (uint32_t h = HwndForSdl(e.window.windowID)) ActivateApp(h, true);
      break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
      if (uint32_t h = HwndForSdl(e.window.windowID)) {
        // Release keys so nothing stays stuck while we're away.
        for (int vk = 0; vk < 256; vk++) g_key_down[vk] = 0;
        ActivateApp(h, false);
      }
      break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
      uint32_t h = HwndForSdl(e.key.windowID);
      uint8_t vk = VkFromScancode(e.key.scancode);
      if (!h || !vk) break;
      bool down = e.type == SDL_EVENT_KEY_DOWN;
      bool was_down = g_key_down[GenericVk(vk)];
      SetKey(vk, down);
      SetKey(GenericVk(vk), down);
      if (vk == 0x01 || vk == 0x02 || vk == 0x04) break;
      uint8_t gvk = GenericVk(vk);
      bool ext;
      uint8_t sc = ScanCodeForVk(vk, &ext);
      if (e.key.scancode == SDL_SCANCODE_KP_ENTER) ext = true;
      uint32_t lp = 1u | (uint32_t(sc) << 16) | (ext ? 1u << 24 : 0);
      if (!down) lp |= 0xC0000000u;
      else if (was_down) lp |= 0x40000000u;
      bool alt = g_key_down[0x12];
      bool sys = alt || gvk == 0x79 /* F10 */ || gvk == 0x12;
      if (alt) lp |= 1u << 29;
      uint32_t msg = down ? (sys ? WM_SYSKEYDOWN : WM_KEYDOWN)
                          : (sys ? WM_SYSKEYUP : WM_KEYUP);
      Post(h, msg, gvk, lp);
      break;
    }
    case SDL_EVENT_MOUSE_MOTION: {
      uint32_t h = HwndForSdl(e.motion.windowID);
      if (!h) break;
      int ox, oy;
      WindowOrigin(h, &ox, &oy);
      int x = int(e.motion.x), y = int(e.motion.y);
      g_mouse_x = ox + x;
      g_mouse_y = oy + y;
      Post(h, WM_MOUSEMOVE, MouseKeys(), MkLparam(x, y));
      break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
      uint32_t h = HwndForSdl(e.button.windowID);
      if (!h) break;
      bool down = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
      int x = int(e.button.x), y = int(e.button.y);
      uint32_t base;
      uint8_t vk;
      switch (e.button.button) {
        case SDL_BUTTON_LEFT: base = WM_LBUTTONDOWN; vk = 0x01; break;
        case SDL_BUTTON_RIGHT: base = WM_RBUTTONDOWN; vk = 0x02; break;
        case SDL_BUTTON_MIDDLE: base = WM_MBUTTONDOWN; vk = 0x04; break;
        default: return;
      }
      SetKey(vk, down);
      bool dbl = false;
      {
        std::lock_guard<std::recursive_mutex> l(g_mu);
        Window* w = Win(h);
        dbl = down && w && (w->class_style & CS_DBLCLKS) && e.button.clicks == 2;
      }
      uint32_t msg = !down ? base + 1 : dbl ? base + 2 : base;
      Post(h, msg, MouseKeys(), MkLparam(x, y));
      break;
    }
    case SDL_EVENT_MOUSE_WHEEL: {
      uint32_t h = HwndForSdl(e.wheel.windowID);
      if (!h) break;
      int delta = int(e.wheel.y * 120.0f);
      if (!delta) break;
      Post(h, WM_MOUSEWHEEL, MouseKeys() | (uint32_t(uint16_t(int16_t(delta))) << 16),
           MkLparam(g_mouse_x, g_mouse_y));
      break;
    }
    default:
      break;
  }
}

// Writes a guest MSG.
void PutMsg(uint32_t va, const Msg& m) {
  Wr32(va + 0, m.hwnd);
  Wr32(va + 4, m.msg);
  Wr32(va + 8, m.wparam);
  Wr32(va + 12, m.lparam);
  Wr32(va + 16, m.time);
  Wr32(va + 20, uint32_t(m.ptx));
  Wr32(va + 24, uint32_t(m.pty));
}

bool Matches(const Msg& m, uint32_t hwnd, uint32_t lo, uint32_t hi) {
  if (hwnd && m.hwnd != hwnd) return false;
  if (lo == 0 && hi == 0) return true;
  return m.msg >= lo && m.msg <= hi;
}

// Takes (or peeks) the first matching message for the current thread.
bool TakeMessage(uint32_t hwnd, uint32_t lo, uint32_t hi, bool remove, Msg* out) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Queue& q = g_queues[CurrentThread()->tid];
  for (auto it = q.msgs.begin(); it != q.msgs.end(); ++it)
    if (Matches(*it, hwnd, lo, hi)) {
      *out = *it;
      if (remove) q.msgs.erase(it);
      return true;
    }
  if (q.quit) {
    *out = Msg{0, WM_QUIT, q.quit_code, 0, MsgTime(), g_mouse_x, g_mouse_y};
    if (remove) q.quit = false;
    return true;
  }
  return false;
}

bool OnMainThread() { return std::this_thread::get_id() == g_main_thread_id; }

}  // namespace

// DS1_BACKGROUND=1 (used for automated test runs): never move or hide the
// host pointer and don't take focus when the window appears.
bool Background() {
  static const bool bg = [] {
    const char* e = std::getenv("DS1_BACKGROUND");
    return e && *e && *e != '0';
  }();
  return bg;
}

void EnsureSdlVideo() {
  if (g_sdl_up) return;
  if (!OnMainThread()) FatalF("SDL video requested off the main thread");
  if (Background()) SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
  // Leave SIGINT/SIGTERM alone: SDL would turn them into quit events the
  // game may ignore, making the process hard to stop.
  SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
  if (!SDL_Init(SDL_INIT_VIDEO)) FatalF("SDL_Init: %s", SDL_GetError());
  g_sdl_up = true;
  if (const SDL_DisplayMode* m = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay())) {
    g_display_w = m->w;
    g_display_h = m->h;
  }
}

void PumpHostEvents() {
  if (!g_sdl_up || !OnMainThread()) return;
  SDL_Event e;
  while (SDL_PollEvent(&e)) HandleSdlEvent(e);
}

SDL_Window* SdlWindowFor(uint32_t hwnd) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(hwnd);
  return w ? w->sdl : nullptr;
}

namespace hle {
namespace {

using namespace win32;

// ---- classes ---------------------------------------------------------------

HLE("user32.dll", RegisterClassA, 1) {
  uint32_t wc = Arg(c, 0);
  WndClass k;
  k.style = Rd32(wc + 0);
  k.wndproc = Rd32(wc + 4);
  k.cls_extra = Rd32(wc + 8);
  k.wnd_extra = Rd32(wc + 12);
  k.hinst = Rd32(wc + 16);
  k.icon = Rd32(wc + 20);
  k.cursor = Rd32(wc + 24);
  k.brush = Rd32(wc + 28);
  uint32_t name = Rd32(wc + 36);
  k.name = name < 0x10000 ? "#" + std::to_string(name) : Lower(GStr(name));
  std::lock_guard<std::recursive_mutex> l(g_mu);
  if (g_classes.count(k.name)) {
    SetLastError(1410);  // ERROR_CLASS_ALREADY_EXISTS
    return 0;
  }
  k.atom = g_next_atom++;
  g_classes[k.name] = k;
  return k.atom;
}

HLE("user32.dll", UnregisterClassA, 2) {
  uint32_t name = Arg(c, 0);
  std::string n = name < 0x10000 ? "#" + std::to_string(name) : Lower(GStr(name));
  std::lock_guard<std::recursive_mutex> l(g_mu);
  return g_classes.erase(n) ? 1 : 0;
}

HLE("user32.dll", SetClassLongA, 3) {
  // GCL_HCURSOR (-12), GCL_HICON (-14)... cosmetic; remember nothing.
  return 0;
}

// ---- windows ---------------------------------------------------------------

HLE("user32.dll", CreateWindowExA, 12) {
  uint32_t exstyle = Arg(c, 0), cls_va = Arg(c, 1), title_va = Arg(c, 2),
           style = Arg(c, 3), parent = Arg(c, 8), menu = Arg(c, 9),
           hinst = Arg(c, 10), param = Arg(c, 11);
  int32_t x = int32_t(Arg(c, 4)), y = int32_t(Arg(c, 5)), w = int32_t(Arg(c, 6)),
          h = int32_t(Arg(c, 7));
  std::string cls = cls_va < 0x10000 ? "#" + std::to_string(cls_va) : Lower(GStr(cls_va));
  Window win;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    auto it = g_classes.find(cls);
    if (it == g_classes.end()) {
      WarnF("CreateWindowExA: unknown class %s\n", cls.c_str());
      SetLastError(1407);  // ERROR_CANNOT_FIND_WND_CLASS
      return 0;
    }
    win.hwnd = g_next_hwnd;
    g_next_hwnd += 0x10;
    win.cls = cls;
    win.wndproc = it->second.wndproc;
    win.class_style = it->second.style;
    win.extra.assign(it->second.wnd_extra, 0);
  }
  constexpr int32_t kUseDefault = int32_t(0x80000000);
  if (x == kUseDefault) x = y = 0;
  if (w == kUseDefault) {
    w = 640;
    h = 480;
  }
  win.style = style;
  win.exstyle = exstyle;
  win.parent = parent;
  win.id = (style & WS_CHILD) ? menu : 0;
  win.hinst = hinst;
  win.x = x;
  win.y = y;
  win.w = w;
  win.h = h;
  win.title = title_va ? GStr(title_va) : "";
  win.tid = CurrentThread()->tid;
  if (!(style & WS_CHILD)) {
    EnsureSdlVideo();
    int cw, ch;
    ClientSize(win, &cw, &ch);
    win.sdl = SDL_CreateWindow(win.title.c_str(), std::max(cw, 1), std::max(ch, 1),
                               SDL_WINDOW_HIDDEN | SDL_WINDOW_VULKAN);
    if (!win.sdl) FatalF("SDL_CreateWindow: %s", SDL_GetError());
  }
  uint32_t hwnd = win.hwnd;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    g_windows[hwnd] = std::move(win);
  }
  DS1_TRACE("  CreateWindowExA(%s, \"%s\", style %08X, %d,%d %dx%d) -> %X\n",
            cls.c_str(), GStr(title_va).c_str(), style, x, y, w, h, hwnd);

  // CREATESTRUCTA on the guest stack area of the process heap.
  uint32_t cs = HeapAllocProcess(48, true);
  Wr32(cs + 0, param);
  Wr32(cs + 4, hinst);
  Wr32(cs + 8, menu);
  Wr32(cs + 12, parent);
  Wr32(cs + 16, uint32_t(h));
  Wr32(cs + 20, uint32_t(w));
  Wr32(cs + 24, uint32_t(y));
  Wr32(cs + 28, uint32_t(x));
  Wr32(cs + 32, style);
  Wr32(cs + 36, title_va);
  Wr32(cs + 40, cls_va);
  Wr32(cs + 44, exstyle);
  bool ok = Send(hwnd, WM_NCCREATE, 0, cs) != 0;
  if (ok) ok = int32_t(Send(hwnd, WM_CREATE, 0, cs)) != -1;
  FindHeap(g_proc->process_heap)->Free(cs);
  if (!ok) {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    if (Window* wp = Win(hwnd)) {
      if (wp->sdl) SDL_DestroyWindow(wp->sdl);
      wp->destroyed = true;
    }
    return 0;
  }
  if (style & WS_VISIBLE) {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    if (Window* wp = Win(hwnd)) {
      wp->visible = true;
      if (wp->sdl) SDL_ShowWindow(wp->sdl);
    }
  }
  return hwnd;
}

HLE("user32.dll", DestroyWindow, 1) {
  uint32_t hwnd = Arg(c, 0);
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    if (!Win(hwnd)) return 0;
  }
  Send(hwnd, WM_DESTROY, 0, 0);
  Send(hwnd, WM_NCDESTROY, 0, 0);
  std::lock_guard<std::recursive_mutex> l(g_mu);
  if (Window* w = Win(hwnd)) {
    if (w->sdl) SDL_DestroyWindow(w->sdl);
    w->sdl = nullptr;
    w->destroyed = true;
  }
  if (g_focus == hwnd) g_focus = 0;
  if (g_active == hwnd) g_active = 0;
  return 1;
}

HLE("user32.dll", IsWindow, 1) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  return Win(Arg(c, 0)) ? 1 : 0;
}

HLE("user32.dll", ShowWindow, 2) {
  uint32_t hwnd = Arg(c, 0), cmd = Arg(c, 1);
  bool was, show = cmd != 0;  // SW_HIDE = 0
  SDL_Window* sdl;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(hwnd);
    if (!w) return 0;
    was = w->visible;
    w->visible = show;
    sdl = w->sdl;
  }
  if (sdl) {
    if (show) {
      SDL_ShowWindow(sdl);
      if (cmd == 6 || cmd == 2 || cmd == 7) SDL_MinimizeWindow(sdl);
    } else {
      SDL_HideWindow(sdl);
    }
  }
  if (show != was) Send(hwnd, WM_SHOWWINDOW, show ? 1 : 0, 0);
  bool activate = false;
  {
    // A freshly shown top-level window becomes the active app window.
    // Record that before sending: the game's activation handler calls
    // ShowWindow again, which must then see it as already active.
    std::lock_guard<std::recursive_mutex> l(g_mu);
    if (show && sdl && g_active != hwnd) {
      g_active = g_focus = hwnd;
      activate = true;
    }
  }
  if (activate) {
    Send(hwnd, WM_ACTIVATEAPP, 1, 0);
    Send(hwnd, WM_ACTIVATE, 1, 0);
    Send(hwnd, WM_SETFOCUS, 0, 0);
  }
  return was ? 1 : 0;
}

HLE("user32.dll", UpdateWindow, 1) { return 1; }
HLE("user32.dll", InvalidateRect, 3) { return 1; }

HLE("user32.dll", IsWindowVisible, 1) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(Arg(c, 0));
  return w && w->visible ? 1 : 0;
}

HLE("user32.dll", IsWindowEnabled, 1) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(Arg(c, 0));
  return w && w->enabled ? 1 : 0;
}

HLE("user32.dll", EnableWindow, 2) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(Arg(c, 0));
  if (!w) return 0;
  bool was_disabled = !w->enabled;
  w->enabled = Arg(c, 1) != 0;
  return was_disabled ? 1 : 0;
}

HLE("user32.dll", IsIconic, 1) {
  SDL_Window* s = SdlWindowFor(Arg(c, 0));
  return s && (SDL_GetWindowFlags(s) & SDL_WINDOW_MINIMIZED) ? 1 : 0;
}

// Like Windows, WM_SIZE/WM_MOVE are only sent when the size/position
// actually changes (the game re-applies its rect from its WM_SIZE handler;
// sending unconditionally recurses forever).
void ApplyRect(uint32_t hwnd, int x, int y, int w, int h, bool move, bool size) {
  SDL_Window* sdl = nullptr;
  int cw = 0, ch = 0;
  bool moved = false, sized = false;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* win = Win(hwnd);
    if (!win) return;
    if (move && (win->x != x || win->y != y)) {
      win->x = x;
      win->y = y;
      moved = true;
    }
    if (size && (win->w != w || win->h != h)) {
      win->w = w;
      win->h = h;
      sized = true;
    }
    ClientSize(*win, &cw, &ch);
    sdl = win->sdl;
  }
  if (sdl && sized) SDL_SetWindowSize(sdl, std::max(cw, 1), std::max(ch, 1));
  if (sized) Send(hwnd, WM_SIZE, 0, MkLparam(cw, ch));
  if (moved) Send(hwnd, WM_MOVE, 0, MkLparam(x, y));
}

HLE("user32.dll", SetWindowPos, 7) {
  uint32_t hwnd = Arg(c, 0), flags = Arg(c, 6);
  bool nomove = flags & 0x2, nosize = flags & 0x1;
  ApplyRect(hwnd, int32_t(Arg(c, 2)), int32_t(Arg(c, 3)), int32_t(Arg(c, 4)),
            int32_t(Arg(c, 5)), !nomove, !nosize);
  if (flags & 0x40) {  // SWP_SHOWWINDOW
    if (SDL_Window* s = SdlWindowFor(hwnd)) SDL_ShowWindow(s);
    std::lock_guard<std::recursive_mutex> l(g_mu);
    if (Window* w = Win(hwnd)) w->visible = true;
  }
  return 1;
}

HLE("user32.dll", MoveWindow, 6) {
  ApplyRect(Arg(c, 0), int32_t(Arg(c, 1)), int32_t(Arg(c, 2)),
            int32_t(Arg(c, 3)), int32_t(Arg(c, 4)), true, true);
  return 1;
}

HLE("user32.dll", AdjustWindowRect, 3) {
  uint32_t rc = Arg(c, 0), style = Arg(c, 1);
  if (style & (WS_CAPTION | WS_THICKFRAME)) {
    int border = (style & WS_THICKFRAME) ? 4 : 3;
    int caption = (style & WS_CAPTION) == WS_CAPTION ? 19 : 0;
    Wr32(rc + 0, Rd32(rc + 0) - border);
    Wr32(rc + 4, Rd32(rc + 4) - border - caption);
    Wr32(rc + 8, Rd32(rc + 8) + border);
    Wr32(rc + 12, Rd32(rc + 12) + border);
  }
  return 1;
}

HLE("user32.dll", GetClientRect, 2) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(Arg(c, 0));
  uint32_t rc = Arg(c, 1);
  if (!w) return 0;
  int cw, ch;
  ClientSize(*w, &cw, &ch);
  Wr32(rc, 0);
  Wr32(rc + 4, 0);
  Wr32(rc + 8, uint32_t(cw));
  Wr32(rc + 12, uint32_t(ch));
  return 1;
}

HLE("user32.dll", GetWindowRect, 2) {
  uint32_t hwnd = Arg(c, 0), rc = Arg(c, 1);
  if (hwnd == 0x00010000) {  // desktop
    Wr32(rc, 0);
    Wr32(rc + 4, 0);
    Wr32(rc + 8, uint32_t(g_display_w));
    Wr32(rc + 12, uint32_t(g_display_h));
    return 1;
  }
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(hwnd);
  if (!w) return 0;
  Wr32(rc, uint32_t(w->x));
  Wr32(rc + 4, uint32_t(w->y));
  Wr32(rc + 8, uint32_t(w->x + w->w));
  Wr32(rc + 12, uint32_t(w->y + w->h));
  return 1;
}

HLE("user32.dll", ClientToScreen, 2) {
  int ox, oy;
  WindowOrigin(Arg(c, 0), &ox, &oy);
  uint32_t pt = Arg(c, 1);
  Wr32(pt, Rd32(pt) + uint32_t(ox));
  Wr32(pt + 4, Rd32(pt + 4) + uint32_t(oy));
  return 1;
}

HLE("user32.dll", ScreenToClient, 2) {
  int ox, oy;
  WindowOrigin(Arg(c, 0), &ox, &oy);
  uint32_t pt = Arg(c, 1);
  Wr32(pt, Rd32(pt) - uint32_t(ox));
  Wr32(pt + 4, Rd32(pt + 4) - uint32_t(oy));
  return 1;
}

HLE("user32.dll", MapWindowPoints, 4) {
  int fx, fy, tx, ty;
  WindowOrigin(Arg(c, 0), &fx, &fy);
  WindowOrigin(Arg(c, 1), &tx, &ty);
  uint32_t pts = Arg(c, 2), n = Arg(c, 3);
  int dx = fx - tx, dy = fy - ty;
  for (uint32_t i = 0; i < n * 2; i += 2) {
    Wr32(pts + 4 * i, Rd32(pts + 4 * i) + uint32_t(dx));
    Wr32(pts + 4 * i + 4, Rd32(pts + 4 * i + 4) + uint32_t(dy));
  }
  return MkLparam(dx, dy);
}

HLE("user32.dll", GetDesktopWindow, 0) { return 0x00010000; }

HLE("user32.dll", GetParent, 1) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(Arg(c, 0));
  return w ? w->parent : 0;
}

HLE("user32.dll", GetWindow, 2) {
  // GW_OWNER (4) / GW_CHILD (5) / GW_HWNDNEXT (2) ... only owner is used.
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(Arg(c, 0));
  if (!w) return 0;
  return Arg(c, 1) == 4 ? w->parent : 0;
}

HLE("user32.dll", GetLastActivePopup, 1) { return Arg(c, 0); }

HLE("user32.dll", FindWindowA, 2) {
  std::string cls = Arg(c, 0) ? Lower(GStr(Arg(c, 0))) : "";
  std::string title = Arg(c, 1) ? GStr(Arg(c, 1)) : "";
  std::lock_guard<std::recursive_mutex> l(g_mu);
  for (auto& [h, w] : g_windows) {
    if (w.destroyed || (w.style & WS_CHILD)) continue;
    if (Arg(c, 0) && w.cls != cls) continue;
    if (Arg(c, 1) && w.title != title) continue;
    return h;
  }
  return 0;
}

HLE("user32.dll", SetWindowTextA, 2) {
  std::string t = GStr(Arg(c, 1));
  SDL_Window* s = nullptr;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Window* w = Win(Arg(c, 0));
    if (!w) return 0;
    w->title = t;
    s = w->sdl;
  }
  if (s) SDL_SetWindowTitle(s, t.c_str());
  return 1;
}

HLE("user32.dll", GetWindowTextA, 3) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(Arg(c, 0));
  uint32_t buf = Arg(c, 1), cap = Arg(c, 2);
  if (!w || !cap) return 0;
  std::string t = w->title.substr(0, cap - 1);
  PutStr(buf, t);
  return uint32_t(t.size());
}

HLE("user32.dll", GetWindowTextLengthA, 1) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(Arg(c, 0));
  return w ? uint32_t(w->title.size()) : 0;
}

HLE("user32.dll", GetWindowLongA, 2) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(Arg(c, 0));
  int32_t idx = int32_t(Arg(c, 1));
  if (!w) return 0;
  switch (idx) {
    case GWL_WNDPROC: return w->wndproc;
    case GWL_HINSTANCE: return w->hinst;
    case GWL_HWNDPARENT: return w->parent;
    case GWL_STYLE: return w->style | (w->visible ? WS_VISIBLE : 0);
    case GWL_EXSTYLE: return w->exstyle;
    case GWL_USERDATA: return w->userdata;
    case GWL_ID: return w->id;
  }
  if (idx >= 0 && uint32_t(idx) + 4 <= w->extra.size()) {
    uint32_t v;
    std::memcpy(&v, w->extra.data() + idx, 4);
    return v;
  }
  return 0;
}

HLE("user32.dll", SetWindowLongA, 3) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Window* w = Win(Arg(c, 0));
  int32_t idx = int32_t(Arg(c, 1));
  uint32_t v = Arg(c, 2), old = 0;
  if (!w) return 0;
  switch (idx) {
    case GWL_WNDPROC: old = w->wndproc; w->wndproc = v; return old;
    case GWL_HWNDPARENT: old = w->parent; w->parent = v; return old;
    case GWL_STYLE: old = w->style; w->style = v; return old;
    case GWL_EXSTYLE: old = w->exstyle; w->exstyle = v; return old;
    case GWL_USERDATA: old = w->userdata; w->userdata = v; return old;
    case GWL_ID: old = w->id; w->id = v; return old;
  }
  if (idx >= 0 && uint32_t(idx) + 4 <= w->extra.size()) {
    std::memcpy(&old, w->extra.data() + idx, 4);
    std::memcpy(w->extra.data() + idx, &v, 4);
  }
  return old;
}

// ---- focus and activation ---------------------------------------------------------

HLE("user32.dll", GetFocus, 0) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  return g_focus;
}
HLE("user32.dll", SetFocus, 1) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  uint32_t old = g_focus;
  g_focus = Arg(c, 0);
  return old;
}
HLE("user32.dll", GetActiveWindow, 0) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  return g_active;
}
HLE("user32.dll", SetActiveWindow, 1) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  uint32_t old = g_active;
  g_active = Arg(c, 0);
  return old;
}
HLE("user32.dll", GetForegroundWindow, 0) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  return g_active;
}
HLE("user32.dll", SetForegroundWindow, 1) {
  if (SDL_Window* s = SdlWindowFor(Arg(c, 0))) SDL_RaiseWindow(s);
  std::lock_guard<std::recursive_mutex> l(g_mu);
  g_active = g_focus = Arg(c, 0);
  return 1;
}
HLE("user32.dll", BringWindowToTop, 1) {
  if (SDL_Window* s = SdlWindowFor(Arg(c, 0))) SDL_RaiseWindow(s);
  return 1;
}

// ---- messages ----------------------------------------------------------------------

HLE("user32.dll", PeekMessageA, 5) {
  uint32_t msg_va = Arg(c, 0), hwnd = Arg(c, 1), lo = Arg(c, 2), hi = Arg(c, 3),
           flags = Arg(c, 4);
  PumpHostEvents();
  Msg m;
  if (!TakeMessage(hwnd, lo, hi, flags & 1 /* PM_REMOVE */, &m)) return 0;
  PutMsg(msg_va, m);
  return 1;
}

HLE("user32.dll", GetMessageA, 4) {
  uint32_t msg_va = Arg(c, 0), hwnd = Arg(c, 1), lo = Arg(c, 2), hi = Arg(c, 3);
  Msg m;
  for (;;) {
    PumpHostEvents();
    if (TakeMessage(hwnd, lo, hi, true, &m)) break;
    if (OnMainThread() && g_sdl_up) {
      SDL_WaitEventTimeout(nullptr, 10);
    } else {
      std::unique_lock<std::recursive_mutex> l(g_mu);
      g_queue_cv.wait_for(l, std::chrono::milliseconds(10));
    }
  }
  PutMsg(msg_va, m);
  return m.msg == WM_QUIT ? 0 : 1;
}

HLE("user32.dll", PostMessageA, 4) {
  uint32_t hwnd = Arg(c, 0);
  if (!hwnd) {
    PostTo(CurrentThread()->tid,
           Msg{0, Arg(c, 1), Arg(c, 2), Arg(c, 3), MsgTime(), g_mouse_x, g_mouse_y});
    return 1;
  }
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    if (!Win(hwnd)) return 0;
  }
  Post(hwnd, Arg(c, 1), Arg(c, 2), Arg(c, 3));
  return 1;
}

HLE("user32.dll", PostQuitMessage, 1) {
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Queue& q = g_queues[CurrentThread()->tid];
    q.quit = true;
    q.quit_code = Arg(c, 0);
  }
  g_queue_cv.notify_all();
  return 0;
}

HLE("user32.dll", SendMessageA, 4) {
  return Send(Arg(c, 0), Arg(c, 1), Arg(c, 2), Arg(c, 3));
}

HLE("user32.dll", DispatchMessageA, 1) {
  uint32_t m = Arg(c, 0);
  uint32_t hwnd = Rd32(m);
  if (!hwnd) return 0;
  return Send(hwnd, Rd32(m + 4), Rd32(m + 8), Rd32(m + 12));
}

HLE("user32.dll", TranslateMessage, 1) {
  // Generate WM_CHAR from WM_KEYDOWN using the US layout and shift/caps.
  uint32_t m = Arg(c, 0);
  uint32_t msg = Rd32(m + 4), vk = Rd32(m + 8);
  if (msg != WM_KEYDOWN && msg != WM_SYSKEYDOWN) return 0;
  bool shift = g_key_down[0x10], caps = g_key_toggled[0x14];
  bool ctrl = g_key_down[0x11];
  uint32_t ch = 0;
  if (vk >= 'A' && vk <= 'Z') {
    ch = (shift != caps) ? vk : vk + 32;
    if (ctrl) ch = vk - 'A' + 1;
  } else if (vk >= 0x60 && vk <= 0x69 && g_key_toggled[0x90]) {
    ch = '0' + (vk - 0x60);
  } else {
    static const struct { uint8_t vk; char n, s; } k[] = {
        {'0', '0', ')'}, {'1', '1', '!'}, {'2', '2', '@'}, {'3', '3', '#'},
        {'4', '4', '$'}, {'5', '5', '%'}, {'6', '6', '^'}, {'7', '7', '&'},
        {'8', '8', '*'}, {'9', '9', '('}, {0xBD, '-', '_'}, {0xBB, '=', '+'},
        {0xDB, '[', '{'}, {0xDD, ']', '}'}, {0xDC, '\\', '|'},
        {0xBA, ';', ':'}, {0xDE, '\'', '"'}, {0xC0, '`', '~'},
        {0xBC, ',', '<'}, {0xBE, '.', '>'}, {0xBF, '/', '?'},
        {0x20, ' ', ' '}, {0x0D, 13, 13}, {0x08, 8, 8}, {0x09, 9, 9},
        {0x1B, 27, 27}, {0x6A, '*', '*'}, {0x6B, '+', '+'},
        {0x6D, '-', '-'}, {0x6F, '/', '/'}, {0x6E, '.', '.'}};
    for (const auto& e : k)
      if (e.vk == vk) ch = uint8_t(shift ? e.s : e.n);
  }
  if (!ch) return 0;
  Post(Rd32(m), msg == WM_SYSKEYDOWN ? WM_SYSCHAR : WM_CHAR, ch, Rd32(m + 12));
  return 1;
}

HLE("user32.dll", TranslateAcceleratorA, 3) { return 0; }
HLE("user32.dll", IsDialogMessageA, 2) { return 0; }

HLE("user32.dll", CallWindowProcA, 5) {
  Ctx& cx = CurrentThread()->ctx;
  return CallGuest(cx, Arg(c, 0), {Arg(c, 1), Arg(c, 2), Arg(c, 3), Arg(c, 4)});
}

HLE("user32.dll", DefWindowProcA, 4) {
  uint32_t hwnd = Arg(c, 0), msg = Arg(c, 1), wp = Arg(c, 2);
  switch (msg) {
    case WM_NCCREATE: return 1;
    case WM_CLOSE: {
      Ctx& cx = CurrentThread()->ctx;
      uint32_t destroy = ProcAddress(ModuleHandle("user32.dll"), "DestroyWindow");
      CallGuest(cx, destroy, {hwnd});
      return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_NCHITTEST: return 1;  // HTCLIENT
    case WM_SYSCOMMAND:
      if ((wp & 0xFFF0) == 0xF060) Post(hwnd, WM_CLOSE, 0, 0);  // SC_CLOSE
      return 0;
    case WM_SYSKEYDOWN:
      if (wp == 0x73 /* VK_F4 */ && g_key_down[0x12]) Post(hwnd, WM_CLOSE, 0, 0);
      return 0;
    case WM_GETTEXTLENGTH: {
      std::lock_guard<std::recursive_mutex> l(g_mu);
      Window* w = Win(hwnd);
      return w ? uint32_t(w->title.size()) : 0;
    }
    default:
      return 0;
  }
}

HLE("user32.dll", MsgWaitForMultipleObjects, 5) {
  // Wait for objects or for any queued input/message (wake mask ignored).
  uint32_t n = Arg(c, 0), arr = Arg(c, 1), wait_all = Arg(c, 2),
           timeout = Arg(c, 3);
  uint32_t wfm = ProcAddress(ModuleHandle("kernel32.dll"), "WaitForMultipleObjects");
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
  Ctx& cx = CurrentThread()->ctx;
  for (;;) {
    PumpHostEvents();
    {
      std::lock_guard<std::recursive_mutex> l(g_mu);
      Queue& q = g_queues[CurrentThread()->tid];
      if (!q.msgs.empty() || q.quit) return n;  // WAIT_OBJECT_0 + n
    }
    if (n) {
      uint32_t r = CallGuest(cx, wfm, {n, arr, wait_all, 0});
      if (r != 0x102) return r;
    }
    if (timeout != 0xFFFFFFFF && std::chrono::steady_clock::now() >= deadline)
      return 0x102;  // WAIT_TIMEOUT
    if (OnMainThread() && g_sdl_up)
      SDL_WaitEventTimeout(nullptr, 1);
    else
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

// ---- hooks ---------------------------------------------------------------------------
// The only hook is WH_KEYBOARD_LL (13), installed to swallow the Windows
// keys. Our input never produces system hotkeys, so it is recorded and
// never called.

HLE("user32.dll", SetWindowsHookExA, 4) {
  static uint32_t next = 0x00F00010;
  DS1_TRACE("  SetWindowsHookExA(type %d, proc %08X): recorded, not called\n",
            int32_t(Arg(c, 0)), Arg(c, 1));
  return next += 0x10;
}
HLE("user32.dll", CallNextHookEx, 4) { return 0; }

// ---- cursor ------------------------------------------------------------------------

HLE("user32.dll", ShowCursor, 1) {
  g_cursor_count += Arg(c, 0) ? 1 : -1;
  if (g_sdl_up && !Background()) {
    if (g_cursor_count >= 0)
      SDL_ShowCursor();
    else
      SDL_HideCursor();
  }
  return uint32_t(g_cursor_count);
}

HLE("user32.dll", GetCursorPos, 1) {
  Wr32(Arg(c, 0), uint32_t(g_mouse_x));
  Wr32(Arg(c, 0) + 4, uint32_t(g_mouse_y));
  return 1;
}

HLE("user32.dll", SetCursorPos, 2) {
  int x = int32_t(Arg(c, 0)), y = int32_t(Arg(c, 1));
  g_mouse_x = x;
  g_mouse_y = y;
  uint32_t hwnd;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    hwnd = g_active;
  }
  // Only move the real pointer while our window has focus: the game
  // recentres the cursor every frame and must never grab it from other
  // applications.
  SDL_Window* s = SdlWindowFor(hwnd);
  if (s && !Background() && SDL_GetKeyboardFocus() == s) {
    int ox, oy;
    WindowOrigin(hwnd, &ox, &oy);
    SDL_WarpMouseInWindow(s, float(x - ox), float(y - oy));
  }
  return 1;
}

// ---- display modes -------------------------------------------------------------------

struct Mode {
  int w, h, bpp;
};

std::vector<Mode> Modes() {
  std::vector<Mode> m;
  const int sizes[][2] = {{640, 480}, {800, 600}, {1024, 768}, {1152, 864},
                          {1280, 960}, {1280, 1024}, {1600, 1200}};
  for (int bpp : {16, 32})
    for (const auto& s : sizes)
      if (s[0] <= std::max(g_display_w, 1024) && s[1] <= std::max(g_display_h, 768))
        m.push_back({s[0], s[1], bpp});
  return m;
}

void PutDevMode(uint32_t dm, const Mode& m) {
  std::memset(GPtr(dm), 0, 156);
  PutStr(dm, "DS1 Display");
  Wr16(dm + 32, 0x0401);  // dmSpecVersion
  Wr16(dm + 36, 156);     // dmSize
  Wr32(dm + 40, 0x001C0000 | 0x00400000);  // BITSPERPEL|PELSWIDTH|PELSHEIGHT|DISPLAYFREQUENCY
  Wr32(dm + 104, uint32_t(m.bpp));
  Wr32(dm + 108, uint32_t(m.w));
  Wr32(dm + 112, uint32_t(m.h));
  Wr32(dm + 120, 60);
}

HLE("user32.dll", EnumDisplaySettingsA, 3) {
  EnsureSdlVideo();
  uint32_t idx = Arg(c, 1), dm = Arg(c, 2);
  if (idx == 0xFFFFFFFF || idx == 0xFFFFFFFE) {  // ENUM_CURRENT/REGISTRY_SETTINGS
    PutDevMode(dm, {g_display_w, g_display_h, 32});
    return 1;
  }
  auto modes = Modes();
  if (idx >= modes.size()) return 0;
  PutDevMode(dm, modes[idx]);
  return 1;
}

// ---- painting and GDI device contexts (no GDI drawing reaches the screen;
// the game renders through DirectDraw) -------------------------------------------------

HLE("user32.dll", GetDC, 1) { return 0x00D00001; }
HLE("user32.dll", ReleaseDC, 2) { return 1; }
HLE("user32.dll", BeginPaint, 2) {
  uint32_t ps = Arg(c, 1);
  std::memset(GPtr(ps), 0, 64);
  Wr32(ps, 0x00D00001);
  return 0x00D00001;
}
HLE("user32.dll", EndPaint, 2) { return 1; }
HLE("user32.dll", FillRect, 3) { return 1; }
HLE("user32.dll", DrawTextA, 5) { return 16; }
HLE("user32.dll", DrawFrameControl, 4) { return 1; }

// ---- clipboard (not wired to the host yet) --------------------------------------------

HLE("user32.dll", OpenClipboard, 1) { return 0; }
HLE("user32.dll", CloseClipboard, 0) { return 1; }
HLE("user32.dll", EmptyClipboard, 0) { return 0; }
HLE("user32.dll", GetClipboardData, 1) { return 0; }
HLE("user32.dll", SetClipboardData, 2) { return 0; }

}  // namespace
}  // namespace hle
}  // namespace ds1
