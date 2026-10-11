#pragma once
// Generator for tests/lift_x87_test.cpp; see x87test.cpp.

#include <string>

#include "program.h"

namespace lifter {
class Decoder;
int WriteX87Test(const Program& p, const Decoder& dec, const std::string& out,
                 const char* prologue);
}  // namespace lifter
