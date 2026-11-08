// World frustums wide enough for the Hor+ view on screens wider than 4:3.
// Replacement of a lifted function named with `replace` in
// lifter.manifest; FINDINGS.md "Widescreen".
//
// Terrain nodes and objects are loaded (and simulated) only inside world
// frustums: axis-aligned boxes around their owners, half `width` metres
// across in x and z and half `height` metres in y. Their size is set in one
// place, 0x6804BA (SiegeFrustum, __thiscall (width, height)), from the
// map's world_frustum_radius / world_frustum_height (45 / 100000 by
// default) when a frustum is created, and from the region's mood
// (frustum_width / frustum_height) as moods blend. The box was sized for a
// 4:3 view; with the widened field of view (src/fov.cpp) the screen's
// sides reach past it, and the camera sees the edge of the loaded world
// (the clear colour where nodes have not loaded yet). Here the width is
// scaled by A / (4/3) for a screen aspect A > 4/3: the horizontal reach
// grows with tan(fov' / 2) in the same proportion. The player's frustum is
// made while the front end still runs at 800x600, so A is the wider of the
// desktop's (the screen a fullscreen game shows on) and the current
// screen's; a 4:3 mode on a wide desktop loads more than it shows.

#include <algorithm>
#include <bit>
#include <cstdint>

#include "runtime/display.h"
#include "x86/lifted.h"
#include "x86/runtime.h"

namespace recomp::gen {

namespace {

// The siege engine singleton: Rapi at +0x44, its screen width/height in
// pixels at +0x66C/+0x670.
constexpr uint32_t kSiegeEngine = 0x7A05FC;
constexpr uint32_t kRapiWidth = 0x66C, kRapiHeight = 0x670;

double Aspect(int w, int h) { return w > 0 && h > 0 ? double(w) / h : 0; }

}  // namespace

void f_006804BA_lifted(Ctx& c);

// SiegeFrustum::SetDimensions(float width, float height), __thiscall.
void f_006804BA(Ctx& c) {
  int w, h;
  DesktopSize(&w, &h);
  double a = Aspect(w, h);
  const uint32_t engine = Rd32(kSiegeEngine);
  if (const uint32_t rapi = engine ? Rd32(engine + 0x44) : 0)
    a = std::max(a, Aspect(int32_t(Rd32(rapi + kRapiWidth)), int32_t(Rd32(rapi + kRapiHeight))));
  const uint32_t width = Rd32(c.esp + 4);
  if (a * 3 > 4)
    Wr32(c.esp + 4, std::bit_cast<uint32_t>(float(std::bit_cast<float>(width) * a * 0.75)));
  f_006804BA_lifted(c);
}

}  // namespace recomp::gen

