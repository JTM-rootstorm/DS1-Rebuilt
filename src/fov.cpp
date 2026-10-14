// Hor+ field of view on screens wider than 4:3. Replacements of lifted
// functions named with `replace` in lifter.manifest; FINDINGS.md
// "Widescreen".
//
// The camera's field of view (SiegeCamera +0x124, radians, 60 degrees)
// is horizontal: the projection (Rapi 0x6539DE) has x scale cot(fov / 2)
// and y scale that times the viewport's aspect, so a wider screen sees
// less vertically (Hor-). Three functions read it, and each must see the
// same value or the view, its culling and the mouse disagree:
//   0x66192A  camera update: culling frustum (0x6799C1) and projection
//             (0x653D06) from fov and the viewport size;
//   0x672701  drag-selection box to a camera-space volume;
//   0x6729D3  screen point to a camera-space ray (mouse picking).
// Around each, the member holds the widened fov' = 2 atan(tan(fov / 2) *
// A / (4/3)) for a viewport aspect A > 4/3, so the vertical field of view
// is what a 4:3 screen gets; the stored value (saved games, scripts) is
// left as it was. At 4:3 and narrower nothing changes.

#include <bit>
#include <cmath>
#include <cstdint>

#include "runtime/lifted.h"
#include "runtime/runtime.h"

namespace recomp::gen {

namespace {

// The siege engine singleton: camera at +0x3C, Rapi at +0x44.
constexpr uint32_t kSiegeEngine = 0x7A05FC;
// SiegeCamera: normalised viewport width/height, field of view.
constexpr uint32_t kCamViewportW = 0x11C, kCamViewportH = 0x120, kCamFov = 0x124;
// Rapi: screen width/height in pixels.
constexpr uint32_t kRapiWidth = 0x66C, kRapiHeight = 0x670;

float F32(uint32_t va) { return std::bit_cast<float>(Rd32(va)); }

// Holds the widened field of view in `camera` for its lifetime.
class WideFov {
 public:
  explicit WideFov(uint32_t camera) : camera_(camera) {
    const uint32_t rapi = Rd32(Rd32(kSiegeEngine) + 0x44);
    if (!camera || !rapi) return;
    // The viewport size as SiegeCamera::GetViewportWidth/Height round it.
    const double w = std::nearbyint(float(int32_t(Rd32(rapi + kRapiWidth))) *
                                    F32(camera + kCamViewportW));
    const double h = std::nearbyint(float(int32_t(Rd32(rapi + kRapiHeight))) *
                                    F32(camera + kCamViewportH));
    if (!(w > 0 && h > 0) || w * 3 <= h * 4) return;
    saved_ = Rd32(camera + kCamFov);
    const double fov = std::bit_cast<float>(saved_);
    const double wide = 2 * std::atan(std::tan(fov / 2) * (w / h) * 0.75);
    Wr32(camera + kCamFov, std::bit_cast<uint32_t>(float(wide)));
    active_ = true;
  }
  ~WideFov() {
    if (active_) Wr32(camera_ + kCamFov, saved_);
  }
  WideFov(const WideFov&) = delete;
  WideFov& operator=(const WideFov&) = delete;

 private:
  uint32_t camera_;
  uint32_t saved_ = 0;
  bool active_ = false;
};

uint32_t EngineCamera() {
  const uint32_t engine = Rd32(kSiegeEngine);
  return engine ? Rd32(engine + 0x3C) : 0;
}

}  // namespace

void f_0066192A_lifted(Ctx& c);
void f_00672701_lifted(Ctx& c);
void f_006729D3_lifted(Ctx& c);

// SiegeCamera update (__fastcall, the camera in ECX).
void f_0066192A(Ctx& c) {
  WideFov wide(c.ecx);
  f_0066192A_lifted(c);
}

void f_00672701(Ctx& c) {
  WideFov wide(EngineCamera());
  f_00672701_lifted(c);
}

void f_006729D3(Ctx& c) {
  WideFov wide(EngineCamera());
  f_006729D3_lifted(c);
}

}  // namespace recomp::gen
