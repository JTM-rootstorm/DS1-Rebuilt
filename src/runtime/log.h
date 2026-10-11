#pragma once
// Minimal logging. Trace output (every HLE call) is enabled with
// DS1_TRACE=1; warnings and fatal errors always print.

#include <cstdio>

namespace ds1 {

extern bool g_trace;

[[noreturn]] void FatalF(const char* fmt, ...)
    __attribute__((format(printf, 1, 2)));
void WarnF(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void TraceF(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace ds1

#define DS1_TRACE(...)                    \
  do {                                    \
    if (::ds1::g_trace) ::ds1::TraceF(__VA_ARGS__); \
  } while (0)
