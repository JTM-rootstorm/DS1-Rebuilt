// advapi32 (registry, user name), version.dll (version resources) and the
// small ole32 surface the game uses.
//
// The registry is an in-memory tree persisted to
// $XDG_DATA_HOME/ds1/registry.txt. It is seeded with what the retail
// installer would have written (the GOG install writes nothing).

#include <sys/stat.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <vector>

#include "runtime/hle/hle.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/hle/win32.h"
#include "runtime/log.h"

namespace ds1::hle {
namespace {

using namespace win32;

constexpr uint32_t kHkeyClassesRoot = 0x80000000, kHkeyCurrentUser = 0x80000001,
                   kHkeyLocalMachine = 0x80000002, kHkeyUsers = 0x80000003;
constexpr uint32_t kRegSz = 1;

struct RegValue {
  uint32_t type;
  std::vector<uint8_t> data;
};

std::mutex g_reg_mu;
// Full key path (lower case, e.g. "hklm\software\...") -> values (lower
// case name -> value). Key existence = presence in the map.
std::map<std::string, std::map<std::string, RegValue>> g_reg;
std::map<uint32_t, std::string> g_open;  // HKEY -> key path
uint32_t g_next_hkey = 0x1000;
bool g_reg_loaded = false;

std::string RegFile() {
  const char* x = std::getenv("XDG_DATA_HOME");
  std::string base = x && *x ? x : std::string(std::getenv("HOME")) + "/.local/share";
  return base + "/ds1/registry.txt";
}

RegValue Str(const std::string& s) {
  RegValue v{kRegSz, {}};
  v.data.assign(s.begin(), s.end());
  v.data.push_back(0);
  return v;
}

void Seed() {
  std::string k = "hklm\\software\\microsoft\\microsoft games\\dungeonsiege\\1.0";
  // Parent keys exist too.
  g_reg["hklm\\software"];
  g_reg["hklm\\software\\microsoft"];
  g_reg["hklm\\software\\microsoft\\microsoft games"];
  g_reg["hklm\\software\\microsoft\\microsoft games\\dungeonsiege"];
  g_reg[k]["exe path"] = Str(kGuestGameDir);
  g_reg[k]["installationdirectory"] = Str(kGuestGameDir);
}

void Save() {
  std::string path = RegFile();
  std::string dir = path.substr(0, path.find_last_of('/'));
  for (size_t i = 1; i <= dir.size(); i++)
    if (i == dir.size() || dir[i] == '/') mkdir(dir.substr(0, i).c_str(), 0755);
  std::ofstream f(path);
  for (const auto& [key, vals] : g_reg) {
    f << "[" << key << "]\n";
    for (const auto& [name, v] : vals) {
      f << name << "\t" << v.type << "\t";
      for (uint8_t b : v.data) {
        char buf[3];
        std::snprintf(buf, sizeof buf, "%02x", b);
        f << buf;
      }
      f << "\n";
    }
  }
}

void LoadOnce() {
  if (g_reg_loaded) return;
  g_reg_loaded = true;
  std::ifstream f(RegFile());
  if (!f) {
    Seed();
    return;
  }
  std::string line, key;
  while (std::getline(f, line)) {
    if (line.empty()) continue;
    if (line[0] == '[') {
      key = line.substr(1, line.size() - 2);
      g_reg[key];
      continue;
    }
    std::istringstream in(line);
    std::string name, type, hex;
    std::getline(in, name, '\t');
    std::getline(in, type, '\t');
    std::getline(in, hex);
    RegValue v{uint32_t(std::stoul(type)), {}};
    for (size_t i = 0; i + 1 < hex.size(); i += 2)
      v.data.push_back(uint8_t(std::stoul(hex.substr(i, 2), nullptr, 16)));
    g_reg[key][name] = v;
  }
}

std::string RootName(uint32_t h) {
  switch (h) {
    case kHkeyClassesRoot: return "hkcr";
    case kHkeyCurrentUser: return "hkcu";
    case kHkeyLocalMachine: return "hklm";
    case kHkeyUsers: return "hku";
    default: {
      auto it = g_open.find(h);
      return it == g_open.end() ? std::string() : it->second;
    }
  }
}

std::string Join(const std::string& base, const std::string& sub) {
  std::string s = Lower(sub);
  while (!s.empty() && s.back() == '\\') s.pop_back();
  while (!s.empty() && s.front() == '\\') s.erase(0, 1);
  return s.empty() ? base : base + "\\" + s;
}

constexpr uint32_t kErrSuccess = 0, kErrFileNotFound = 2, kErrInvalidHandle = 6,
                   kErrMoreData = 234;

HLE("advapi32.dll", RegOpenKeyExA, 5) {
  std::lock_guard<std::mutex> l(g_reg_mu);
  LoadOnce();
  std::string base = RootName(Arg(c, 0));
  if (base.empty()) return kErrInvalidHandle;
  std::string path = Join(base, GStr(Arg(c, 1)));
  if (!g_reg.count(path)) return kErrFileNotFound;
  uint32_t h = g_next_hkey += 4;
  g_open[h] = path;
  Wr32(Arg(c, 4), h);
  return kErrSuccess;
}

HLE("advapi32.dll", RegCreateKeyExA, 9) {
  std::lock_guard<std::mutex> l(g_reg_mu);
  LoadOnce();
  std::string base = RootName(Arg(c, 0));
  if (base.empty()) return kErrInvalidHandle;
  std::string path = Join(base, GStr(Arg(c, 1)));
  bool existed = g_reg.count(path);
  // Create the key and all its parents.
  for (size_t i = 0; i <= path.size(); i++)
    if (i == path.size() || path[i] == '\\') g_reg[path.substr(0, i)];
  uint32_t h = g_next_hkey += 4;
  g_open[h] = path;
  Wr32(Arg(c, 7), h);
  if (Arg(c, 8)) Wr32(Arg(c, 8), existed ? 2 : 1);  // REG_OPENED / CREATED
  if (!existed) Save();
  return kErrSuccess;
}

HLE("advapi32.dll", RegCloseKey, 1) {
  std::lock_guard<std::mutex> l(g_reg_mu);
  g_open.erase(Arg(c, 0));
  return kErrSuccess;
}

HLE("advapi32.dll", RegQueryValueExA, 6) {
  std::lock_guard<std::mutex> l(g_reg_mu);
  LoadOnce();
  std::string key = RootName(Arg(c, 0));
  uint32_t type_out = Arg(c, 3), data = Arg(c, 4), size_ptr = Arg(c, 5);
  auto k = g_reg.find(key);
  if (k == g_reg.end()) return kErrInvalidHandle;
  auto v = k->second.find(Lower(GStr(Arg(c, 1))));
  if (v == k->second.end()) {
    DS1_TRACE("  RegQueryValueExA(%s, %s): not found\n", key.c_str(),
              GStr(Arg(c, 1)).c_str());
    return kErrFileNotFound;
  }
  if (type_out) Wr32(type_out, v->second.type);
  uint32_t n = uint32_t(v->second.data.size());
  if (!size_ptr) return data ? kErrInvalidHandle : kErrSuccess;
  uint32_t cap = Rd32(size_ptr);
  Wr32(size_ptr, n);
  if (!data) return kErrSuccess;
  if (cap < n) return kErrMoreData;
  std::memcpy(GPtr(data), v->second.data.data(), n);
  return kErrSuccess;
}

HLE("advapi32.dll", RegSetValueExA, 6) {
  std::lock_guard<std::mutex> l(g_reg_mu);
  LoadOnce();
  std::string key = RootName(Arg(c, 0));
  if (!g_reg.count(key)) return kErrInvalidHandle;
  RegValue v{Arg(c, 3), {}};
  uint8_t* p = GPtr(Arg(c, 4));
  v.data.assign(p, p + Arg(c, 5));
  g_reg[key][Lower(GStr(Arg(c, 1)))] = v;
  Save();
  return kErrSuccess;
}

HLE("advapi32.dll", GetUserNameA, 2) {
  std::string n = "Player";
  uint32_t buf = Arg(c, 0), psize = Arg(c, 1);
  if (Rd32(psize) < n.size() + 1) {
    Wr32(psize, uint32_t(n.size() + 1));
    SetLastError(kErrorInsufficientBuffer);
    return 0;
  }
  PutStr(buf, n);
  Wr32(psize, uint32_t(n.size() + 1));
  return 1;
}

// ---- version.dll ------------------------------------------------------------
// Only the game's own EXE has version info we can serve; it comes straight
// from the mapped .rsrc section.

// Finds the first RT_VERSION resource in the mapped image. Returns its
// guest VA and size.
bool FindVersionResource(uint32_t* va, uint32_t* size) {
  const PeImage& img = g_proc->img;
  uint32_t rsrc = 0;
  for (const auto& s : img.sections)
    if (s.name == ".rsrc") rsrc = s.va;
  if (!rsrc) return false;
  // Root directory: find type 16 (RT_VERSION), then the first name and
  // first language entries.
  uint32_t dir = rsrc;
  uint32_t want_id = 16;
  for (int level = 0; level < 3; level++) {
    uint16_t named = Rd16(dir + 12), ids = Rd16(dir + 14);
    uint32_t entry = dir + 16;
    uint32_t found = 0;
    for (uint32_t i = 0; i < uint32_t(named + ids); i++, entry += 8) {
      uint32_t id = Rd32(entry), off = Rd32(entry + 4);
      if (level == 0 && (id & 0x80000000 || id != want_id)) continue;
      found = off;
      break;
    }
    if (!found) return false;
    if (found & 0x80000000) {
      dir = rsrc + (found & 0x7FFFFFFF);
    } else {
      uint32_t data_entry = rsrc + found;
      *va = img.image_base + Rd32(data_entry);
      *size = Rd32(data_entry + 4);
      return true;
    }
  }
  return false;
}

bool IsOwnExe(const std::string& path) {
  std::string l = Lower(path);
  return l.size() >= 16 && l.substr(l.size() - 16) == "dungeonsiege.exe";
}

HLE("version.dll", GetFileVersionInfoSizeA, 2) {
  uint32_t va, size;
  if (Arg(c, 1)) Wr32(Arg(c, 1), 0);
  if (!IsOwnExe(GStr(Arg(c, 0))) || !FindVersionResource(&va, &size)) {
    SetLastError(kErrorFileNotFound);
    return 0;
  }
  // Room after the resource for ANSI copies handed out by VerQueryValueA.
  return size * 3;
}

HLE("version.dll", GetFileVersionInfoA, 4) {
  uint32_t va, size;
  uint32_t len = Arg(c, 2), buf = Arg(c, 3);
  if (!IsOwnExe(GStr(Arg(c, 0))) || !FindVersionResource(&va, &size)) {
    SetLastError(kErrorFileNotFound);
    return 0;
  }
  std::memcpy(GPtr(buf), GPtr(va), std::min(len, size));
  // Remember where the scratch area starts (stored after the resource).
  if (len >= size + 8) Wr32(buf + size, size + 8);
  return 1;
}

// Walks a VS_VERSIONINFO block tree (UTF-16 keys) along a backslash path.
bool VersionLookup(uint32_t block, const std::vector<std::string>& path,
                   size_t depth, uint32_t* value, uint32_t* vlen, bool* text) {
  uint16_t length = Rd16(block), value_len = Rd16(block + 2), type = Rd16(block + 4);
  std::u16string key = GWStr(block + 6);
  uint32_t p = block + 6 + uint32_t(key.size() + 1) * 2;
  p = (p + 3) & ~3u;
  if (depth == path.size()) {
    *value = p;
    *vlen = value_len;
    *text = type == 1;
    return true;
  }
  uint32_t child = (p + (type == 1 ? value_len * 2u : value_len) + 3) & ~3u;
  uint32_t end = block + length;
  while (child < end) {
    uint16_t clen = Rd16(child);
    if (!clen) break;
    if (Lower(Narrow(GWStr(child + 6))) == Lower(path[depth]))
      return VersionLookup(child, path, depth + 1, value, vlen, text);
    child = (child + clen + 3) & ~3u;
  }
  return false;
}

HLE("version.dll", VerQueryValueA, 4) {
  uint32_t buf = Arg(c, 0), out_ptr = Arg(c, 2), out_len = Arg(c, 3);
  std::string sub = GStr(Arg(c, 1));
  std::vector<std::string> path;
  std::string cur;
  for (char ch : sub) {
    if (ch == '\\') {
      if (!cur.empty()) path.push_back(cur);
      cur.clear();
    } else {
      cur += ch;
    }
  }
  if (!cur.empty()) path.push_back(cur);
  uint32_t value, vlen;
  bool text;
  if (!VersionLookup(buf, path, 0, &value, &vlen, &text)) return 0;
  if (text) {
    // Hand out an ANSI copy from the scratch area after the resource.
    uint32_t size = Rd16(buf);
    uint32_t scratch_off = Rd32(buf + size);
    std::string s = Narrow(GWStr(value));
    uint32_t dst = buf + scratch_off;
    PutStr(dst, s);
    Wr32(buf + size, scratch_off + uint32_t(s.size()) + 1);
    Wr32(out_ptr, dst);
    Wr32(out_len, uint32_t(s.size() + 1));
  } else {
    Wr32(out_ptr, value);
    Wr32(out_len, vlen);
  }
  return 1;
}

// ---- ole32 ------------------------------------------------------------------

HLE("ole32.dll", CoInitializeEx, 2) { return 0; }  // S_OK
HLE("ole32.dll", CoUninitialize, 0) { return 0; }

HLE("ole32.dll", CoCreateInstance, 5) {
  // Only DirectPlay8 (multiplayer) and the Zone update object are created
  // this way; neither exists here. REGDB_E_CLASSNOTREG.
  uint32_t clsid = Arg(c, 0);
  WarnF("CoCreateInstance({%08X-...}): class not available\n", Rd32(clsid));
  if (Arg(c, 4)) Wr32(Arg(c, 4), 0);
  return 0x80040154;
}

HLE("ole32.dll", CoCreateGuid, 1) {
  static std::mt19937_64 rng{std::random_device{}()};
  uint32_t g = Arg(c, 0);
  for (int i = 0; i < 4; i++) Wr32(g + 4 * i, uint32_t(rng()));
  Wr8(g + 7, uint8_t((g_proc->mem.read<uint8_t>(g + 7) & 0x0F) | 0x40));  // v4
  return 0;
}

HLE("ole32.dll", StringFromGUID2, 3) {
  uint32_t g = Arg(c, 0), out = Arg(c, 1), cap = Arg(c, 2);
  char buf[64];
  const uint8_t* b = GPtr(g + 8);
  std::snprintf(buf, sizeof buf,
                "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", Rd32(g),
                Rd16(g + 4), Rd16(g + 6), b[0], b[1], b[2], b[3], b[4], b[5],
                b[6], b[7]);
  size_t n = std::strlen(buf) + 1;
  if (cap < n) return 0;
  for (size_t i = 0; i < n; i++) Wr16(out + 2 * uint32_t(i), uint8_t(buf[i]));
  return uint32_t(n);
}

HLE("ole32.dll", IIDFromString, 2) {
  std::string s = Narrow(GWStr(Arg(c, 0)));
  unsigned int d1, d2, d3, b[8];
  if (std::sscanf(s.c_str(), "{%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x}", &d1,
                  &d2, &d3, &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &b[6],
                  &b[7]) != 11)
    return 0x80070057;  // E_INVALIDARG
  uint32_t g = Arg(c, 1);
  Wr32(g, d1);
  Wr16(g + 4, uint16_t(d2));
  Wr16(g + 6, uint16_t(d3));
  for (int i = 0; i < 8; i++) Wr8(g + 8 + i, uint8_t(b[i]));
  return 0;
}

}  // namespace
}  // namespace ds1::hle
