// mss32 (Miles Sound System). M3 bring-up: the library initialises and
// reports that no digital audio device exists, which the game treats as
// "sound disabled" (a normal configuration on Windows). The real driver
// (OpenAL Soft) is M4; the state kept here (preferences, file callbacks)
// is what it will build on.

#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include "runtime/hle/hle.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/log.h"

namespace ds1::hle {
namespace {

std::mutex g_mu;
std::map<uint32_t, uint32_t> g_prefs;
std::string g_last_error;
uint32_t g_error_buf = 0;
struct FileCallbacks {
  uint32_t open = 0, close = 0, seek = 0, read = 0;
} g_file_cb;

void SetError(const std::string& e) {
  std::lock_guard<std::mutex> l(g_mu);
  g_last_error = e;
}

HLE_NAMED("mss32.dll", "_AIL_startup@0", AIL_startup, 0) { return 1; }
HLE_NAMED("mss32.dll", "_AIL_shutdown@0", AIL_shutdown, 0) { return 0; }

HLE_NAMED("mss32.dll", "_AIL_set_redist_directory@4", AIL_set_redist_directory, 1) {
  static uint32_t prev = 0;
  if (!prev) {
    prev = HeapAllocProcess(260, true);
    PutStr(prev, ".");
  }
  return prev;
}

HLE_NAMED("mss32.dll", "_AIL_set_file_callbacks@16", AIL_set_file_callbacks, 4) {
  g_file_cb = {Arg(c, 0), Arg(c, 1), Arg(c, 2), Arg(c, 3)};
  return 0;
}

HLE_NAMED("mss32.dll", "_AIL_set_preference@8", AIL_set_preference, 2) {
  std::lock_guard<std::mutex> l(g_mu);
  uint32_t old = g_prefs[Arg(c, 0)];
  g_prefs[Arg(c, 0)] = Arg(c, 1);
  return old;
}

HLE_NAMED("mss32.dll", "_AIL_get_preference@4", AIL_get_preference, 1) {
  std::lock_guard<std::mutex> l(g_mu);
  return g_prefs[Arg(c, 0)];
}

HLE_NAMED("mss32.dll", "_AIL_last_error@0", AIL_last_error, 0) {
  std::lock_guard<std::mutex> l(g_mu);
  if (!g_error_buf) g_error_buf = HeapAllocProcess(256, true);
  PutStr(g_error_buf, g_last_error.substr(0, 255));
  return g_error_buf;
}

// AIL_open_digital_driver(frequency, bits, channels, flags) -> HDIGDRIVER.
HLE_NAMED("mss32.dll", "_AIL_open_digital_driver@16", AIL_open_digital_driver, 4) {
  WarnF("mss32: AIL_open_digital_driver(%u Hz, %u bit, %u ch): no audio "
        "driver yet (M4), reporting no device\n", Arg(c, 0), Arg(c, 1), Arg(c, 2));
  SetError("No digital audio device (DS1 runtime: audio not implemented yet)");
  return 0;
}

// AIL_enumerate_3D_providers(HPROENUM* next, HPROVIDER* dest, char** name):
// no 3D providers without a driver.
HLE_NAMED("mss32.dll", "_AIL_enumerate_3D_providers@12", AIL_enumerate_3D_providers, 3) {
  return 0;
}

}  // namespace
}  // namespace ds1::hle
