#include "gpb/transform.hpp"

#include <algorithm>
#include <cmath>

namespace gpb {
namespace {

// Radial deadzone on the pair, not per-axis. Per-axis deadzones are the classic cause of
// a stick that refuses to move diagonally near center: each axis is independently
// suppressed, so the first few degrees of a diagonal push read as pure-axis motion.
void shape_stick(const StickProfile& p, int16_t& x, int16_t& y) {
  if (p.invert_x) x = static_cast<int16_t>(-std::clamp<int>(x, -32767, 32767));
  if (p.invert_y) y = static_cast<int16_t>(-std::clamp<int>(y, -32767, 32767));

  const float fx = x, fy = y;
  const float mag = std::sqrt(fx * fx + fy * fy);
  if (mag <= 0.0f) return;

  const float dz = static_cast<float>(p.deadzone);
  const float sat = static_cast<float>(std::max<int16_t>(p.saturation, 1));
  if (mag <= dz) {
    x = y = 0;
    return;
  }

  // Rescale so the magnitude ramps from 0 at the deadzone edge to full at saturation --
  // preserves direction exactly, which per-axis clamping does not.
  float norm = std::min((mag - dz) / std::max(sat - dz, 1.0f), 1.0f);
  if (p.expo != 1.0f) norm = std::pow(norm, p.expo);

  const float scale = norm * 32767.0f / mag;
  x = static_cast<int16_t>(std::clamp(fx * scale, -32767.0f, 32767.0f));
  y = static_cast<int16_t>(std::clamp(fy * scale, -32767.0f, 32767.0f));
}

}  // namespace

void ProfileTransform::apply(GamepadState& s) {
  shape_stick(left_, s.lx, s.ly);
  shape_stick(right_, s.rx, s.ry);
  if (s.lt < trigger_deadzone_) s.lt = 0;
  if (s.rt < trigger_deadzone_) s.rt = 0;

  // Keep the digital trigger bits consistent with the analog values, so a sink that only
  // has digital shoulders (the Switch's ZL/ZR) behaves sanely without special-casing.
  s.set(btn::kL2, s.lt > 40);
  s.set(btn::kR2, s.rt > 40);
}

}  // namespace gpb
