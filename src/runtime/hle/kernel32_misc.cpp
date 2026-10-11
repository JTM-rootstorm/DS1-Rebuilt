// kernel32: resources, time conversion/formatting, strings, and the odds
// and ends (console title, FormatMessage, CreateProcess).

#include <cstring>
#include <ctime>
#include <string>

#include "runtime/hle/hle.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/hle/win32.h"
#include "runtime/log.h"

namespace ds1::hle {
namespace {

using namespace win32;

// ---- resources ------------------------------------------------------------------
// Only the EXE's own .rsrc exists. Resource "handles" (HRSRC) are guest
// addresses of IMAGE_RESOURCE_DATA_ENTRY records; LoadResource returns the
// data pointer and LockResource is the identity, as on Win32.

uint32_t RsrcBase() {
  for (const auto& s : g_proc->img.sections)
    if (s.name == ".rsrc") return s.va;
  return 0;
}

bool IsOwnModule(uint32_t h) { return h == 0 || h == g_proc->img.image_base; }

// Name-or-id argument as a comparable key: "#123" for integer ids.
std::string ResKey(uint32_t v) {
  if (v < 0x10000) return "#" + std::to_string(v);
  std::string s = GStr(v);
  if (!s.empty() && s[0] == '#') return "#" + std::to_string(std::stoul(s.substr(1)));
  for (auto& ch : s) ch = char(std::toupper(uint8_t(ch)));
  return s;
}

// Key of a directory entry (names are UTF-16, upper-cased like Windows).
std::string EntryKey(uint32_t rsrc, uint32_t entry) {
  uint32_t id = Rd32(entry);
  if (!(id & 0x80000000)) return "#" + std::to_string(id & 0xFFFF);
  uint32_t str = rsrc + (id & 0x7FFFFFFF);
  uint16_t len = Rd16(str);
  std::string s;
  for (uint32_t i = 0; i < len; i++)
    s.push_back(char(std::toupper(uint8_t(Rd16(str + 2 + 2 * i)))));
  return s;
}

// Finds the subdirectory/data offset for `key` in directory `dir`; with an
// empty key, the first entry. Returns 0 if absent.
uint32_t FindEntry(uint32_t rsrc, uint32_t dir, const std::string& key) {
  uint32_t n = uint32_t(Rd16(dir + 12)) + Rd16(dir + 14);
  for (uint32_t i = 0; i < n; i++) {
    uint32_t e = dir + 16 + 8 * i;
    if (key.empty() || EntryKey(rsrc, e) == key) return Rd32(e + 4);
  }
  return 0;
}

uint32_t FindResourceImpl(uint32_t type, uint32_t name, uint32_t lang) {
  uint32_t rsrc = RsrcBase();
  if (!rsrc) return 0;
  uint32_t t = FindEntry(rsrc, rsrc, ResKey(type));
  if (!(t & 0x80000000)) return 0;
  uint32_t n = FindEntry(rsrc, rsrc + (t & 0x7FFFFFFF), ResKey(name));
  if (!(n & 0x80000000)) return 0;
  uint32_t ldir = rsrc + (n & 0x7FFFFFFF);
  uint32_t l = lang ? FindEntry(rsrc, ldir, "#" + std::to_string(lang)) : 0;
  if (!l) l = FindEntry(rsrc, ldir, "");
  if (!l || (l & 0x80000000)) return 0;
  return rsrc + l;
}

HLE("kernel32.dll", FindResourceA, 3) {
  if (!IsOwnModule(Arg(c, 0))) return 0;
  uint32_t r = FindResourceImpl(Arg(c, 2), Arg(c, 1), 0);
  if (!r) SetLastError(1814);  // ERROR_RESOURCE_NAME_NOT_FOUND
  return r;
}

HLE("kernel32.dll", FindResourceExA, 4) {
  if (!IsOwnModule(Arg(c, 0))) return 0;
  uint32_t r = FindResourceImpl(Arg(c, 1), Arg(c, 2), Arg(c, 3) & 0xFFFF);
  if (!r) SetLastError(1814);
  return r;
}

HLE("kernel32.dll", LoadResource, 2) {
  uint32_t hrsrc = Arg(c, 1);
  return hrsrc ? g_proc->img.image_base + Rd32(hrsrc) : 0;
}

HLE("kernel32.dll", LockResource, 1) { return Arg(c, 0); }

// Enumeration callbacks get either an integer id or a guest string.
uint32_t KeyArg(const std::string& key, uint32_t* tmp) {
  if (key[0] == '#') return uint32_t(std::stoul(key.substr(1)));
  *tmp = HeapAllocProcess(uint32_t(key.size() + 1), false);
  PutStr(*tmp, key);
  return *tmp;
}

HLE("kernel32.dll", EnumResourceTypesA, 3) {
  uint32_t rsrc = RsrcBase(), fn = Arg(c, 1), param = Arg(c, 2);
  if (!rsrc || !IsOwnModule(Arg(c, 0))) return 0;
  uint32_t n = uint32_t(Rd16(rsrc + 12)) + Rd16(rsrc + 14);
  for (uint32_t i = 0; i < n; i++) {
    uint32_t tmp = 0;
    uint32_t key = KeyArg(EntryKey(rsrc, rsrc + 16 + 8 * i), &tmp);
    bool go = CallGuest(c, fn, {Arg(c, 0), key, param});
    if (tmp) FindHeap(g_proc->process_heap)->Free(tmp);
    if (!go) break;
  }
  return 1;
}

HLE("kernel32.dll", EnumResourceNamesA, 4) {
  uint32_t rsrc = RsrcBase(), type = Arg(c, 1), fn = Arg(c, 2), param = Arg(c, 3);
  if (!rsrc || !IsOwnModule(Arg(c, 0))) return 0;
  uint32_t t = FindEntry(rsrc, rsrc, ResKey(type));
  if (!(t & 0x80000000)) return 0;
  uint32_t dir = rsrc + (t & 0x7FFFFFFF);
  uint32_t n = uint32_t(Rd16(dir + 12)) + Rd16(dir + 14);
  for (uint32_t i = 0; i < n; i++) {
    uint32_t tmp = 0;
    uint32_t key = KeyArg(EntryKey(rsrc, dir + 16 + 8 * i), &tmp);
    bool go = CallGuest(c, fn, {Arg(c, 0), type, key, param});
    if (tmp) FindHeap(g_proc->process_heap)->Free(tmp);
    if (!go) break;
  }
  return 1;
}

HLE("kernel32.dll", EnumResourceLanguagesA, 5) {
  uint32_t rsrc = RsrcBase(), type = Arg(c, 1), name = Arg(c, 2), fn = Arg(c, 3),
           param = Arg(c, 4);
  if (!rsrc || !IsOwnModule(Arg(c, 0))) return 0;
  uint32_t t = FindEntry(rsrc, rsrc, ResKey(type));
  if (!(t & 0x80000000)) return 0;
  uint32_t nd = FindEntry(rsrc, rsrc + (t & 0x7FFFFFFF), ResKey(name));
  if (!(nd & 0x80000000)) return 0;
  uint32_t dir = rsrc + (nd & 0x7FFFFFFF);
  uint32_t n = uint32_t(Rd16(dir + 12)) + Rd16(dir + 14);
  for (uint32_t i = 0; i < n; i++) {
    uint32_t lang = Rd32(dir + 16 + 8 * i) & 0xFFFF;
    if (!CallGuest(c, fn, {Arg(c, 0), type, name, lang, param})) break;
  }
  return 1;
}

// ---- time --------------------------------------------------------------------------

uint64_t RdFt(uint32_t va) { return uint64_t(Rd32(va)) | (uint64_t(Rd32(va + 4)) << 32); }
void WrFt(uint32_t va, uint64_t v) {
  Wr32(va, uint32_t(v));
  Wr32(va + 4, uint32_t(v >> 32));
}
constexpr uint64_t kEpochDiff = 116444736000000000ull;  // 1601 -> 1970

HLE("kernel32.dll", CompareFileTime, 2) {
  uint64_t a = RdFt(Arg(c, 0)), b = RdFt(Arg(c, 1));
  return a < b ? 0xFFFFFFFF : a > b ? 1 : 0;
}

HLE("kernel32.dll", FileTimeToLocalFileTime, 2) {
  time_t now = time(nullptr);
  std::tm lt{};
  localtime_r(&now, &lt);
  int64_t off = int64_t(lt.tm_gmtoff) * 10000000;
  WrFt(Arg(c, 1), uint64_t(int64_t(RdFt(Arg(c, 0))) + off));
  return 1;
}

HLE("kernel32.dll", FileTimeToSystemTime, 2) {
  uint64_t ft = RdFt(Arg(c, 0));
  if (ft < kEpochDiff) return 0;
  time_t secs = time_t((ft - kEpochDiff) / 10000000);
  std::tm t{};
  gmtime_r(&secs, &t);
  uint32_t st = Arg(c, 1);
  Wr16(st + 0, uint16_t(t.tm_year + 1900));
  Wr16(st + 2, uint16_t(t.tm_mon + 1));
  Wr16(st + 4, uint16_t(t.tm_wday));
  Wr16(st + 6, uint16_t(t.tm_mday));
  Wr16(st + 8, uint16_t(t.tm_hour));
  Wr16(st + 10, uint16_t(t.tm_min));
  Wr16(st + 12, uint16_t(t.tm_sec));
  Wr16(st + 14, uint16_t((ft / 10000) % 1000));
  return 1;
}

HLE("kernel32.dll", SystemTimeToFileTime, 2) {
  uint32_t st = Arg(c, 0);
  std::tm t{};
  t.tm_year = Rd16(st) - 1900;
  t.tm_mon = Rd16(st + 2) - 1;
  t.tm_mday = Rd16(st + 6);
  t.tm_hour = Rd16(st + 8);
  t.tm_min = Rd16(st + 10);
  t.tm_sec = Rd16(st + 12);
  time_t secs = timegm(&t);
  WrFt(Arg(c, 1), uint64_t(secs) * 10000000 + uint64_t(Rd16(st + 14)) * 10000 + kEpochDiff);
  return 1;
}

std::tm TmFromSystemTime(uint32_t st) {
  std::tm t{};
  if (!st) {
    time_t now = time(nullptr);
    localtime_r(&now, &t);
    return t;
  }
  t.tm_year = Rd16(st) - 1900;
  t.tm_mon = Rd16(st + 2) - 1;
  t.tm_wday = Rd16(st + 4);
  t.tm_mday = Rd16(st + 6);
  t.tm_hour = Rd16(st + 8);
  t.tm_min = Rd16(st + 10);
  t.tm_sec = Rd16(st + 12);
  return t;
}

uint32_t PutFormatted(uint32_t buf, uint32_t cap, const std::string& s) {
  if (!cap) return uint32_t(s.size() + 1);
  if (s.size() + 1 > cap) {
    SetLastError(kErrorInsufficientBuffer);
    return 0;
  }
  PutStr(buf, s);
  return uint32_t(s.size() + 1);
}

// en-US defaults: M/d/yyyy and h:mm:ss tt. Explicit pictures are not
// translated (the game passes NULL for the format).
HLE("kernel32.dll", GetDateFormatA, 6) {
  std::tm t = TmFromSystemTime(Arg(c, 2));
  char out[64];
  if (Arg(c, 1) & 2)  // DATE_LONGDATE
    std::strftime(out, sizeof out, "%A, %B %d, %Y", &t);
  else
    std::snprintf(out, sizeof out, "%d/%d/%d", t.tm_mon + 1, t.tm_mday, t.tm_year + 1900);
  if (Arg(c, 3)) WarnF("GetDateFormatA: custom picture '%s' ignored\n", GStr(Arg(c, 3)).c_str());
  return PutFormatted(Arg(c, 4), Arg(c, 5), out);
}

HLE("kernel32.dll", GetTimeFormatA, 6) {
  std::tm t = TmFromSystemTime(Arg(c, 2));
  char out[64];
  int h = t.tm_hour % 12;
  if (Arg(c, 1) & 2)  // TIME_NOSECONDS
    std::snprintf(out, sizeof out, "%d:%02d %s", h ? h : 12, t.tm_min, t.tm_hour < 12 ? "AM" : "PM");
  else
    std::snprintf(out, sizeof out, "%d:%02d:%02d %s", h ? h : 12, t.tm_min, t.tm_sec,
                  t.tm_hour < 12 ? "AM" : "PM");
  if (Arg(c, 3)) WarnF("GetTimeFormatA: custom picture '%s' ignored\n", GStr(Arg(c, 3)).c_str());
  return PutFormatted(Arg(c, 4), Arg(c, 5), out);
}

// ---- strings -------------------------------------------------------------------------

HLE("kernel32.dll", lstrcpyA, 2) {
  PutStr(Arg(c, 0), GStr(Arg(c, 1)));
  return Arg(c, 0);
}

HLE("kernel32.dll", IsDBCSLeadByte, 1) { return 0; }    // CP1252: no DBCS
HLE("kernel32.dll", IsDBCSLeadByteEx, 2) { return 0; }

HLE("kernel32.dll", VerLanguageNameA, 3) {
  std::string n = (Arg(c, 0) & 0xFFFF) == 0x0409 ? "English (United States)"
                                                 : "Language Neutral";
  uint32_t buf = Arg(c, 1), cap = Arg(c, 2);
  if (!cap) return uint32_t(n.size());
  n = n.substr(0, cap - 1);
  PutStr(buf, n);
  return uint32_t(n.size());
}

HLE("kernel32.dll", FormatMessageA, 7) {
  uint32_t flags = Arg(c, 0), id = Arg(c, 2), buf = Arg(c, 4), cap = Arg(c, 5);
  if (!(flags & 0x1000)) {  // only FORMAT_MESSAGE_FROM_SYSTEM
    WarnF("FormatMessageA(flags %X): only FROM_SYSTEM is supported\n", flags);
    return 0;
  }
  std::string s = "Error " + std::to_string(id) + ".\r\n";
  if (flags & 0x100) {  // FORMAT_MESSAGE_ALLOCATE_BUFFER
    uint32_t p = HeapAllocProcess(uint32_t(s.size() + 1), false);
    PutStr(p, s);
    Wr32(buf, p);
    return uint32_t(s.size());
  }
  if (s.size() + 1 > cap) return 0;
  PutStr(buf, s);
  return uint32_t(s.size());
}

// ---- console, processes, debugging ------------------------------------------------------

HLE("kernel32.dll", GetConsoleTitleA, 2) {
  SetLastError(kErrorInvalidHandle);  // no console attached
  return 0;
}
HLE("kernel32.dll", SetConsoleTitleA, 1) { return 1; }

HLE("kernel32.dll", CreateProcessA, 10) {
  WarnF("CreateProcessA(%s, %s): not supported\n", GStr(Arg(c, 0)).c_str(),
        GStr(Arg(c, 1)).c_str());
  SetLastError(kErrorFileNotFound);
  return 0;
}

HLE("kernel32.dll", FlushInstructionCache, 3) { return 1; }

HLE("kernel32.dll", DebugBreak, 0) {
  FatalF("DebugBreak() called from %08X", Rd32(c.esp));
}

}  // namespace
}  // namespace ds1::hle
