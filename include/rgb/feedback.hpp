#pragma once
// Reverse channel: sink -> source. Rumble is the motivating case (the console tells the
// controller to vibrate). Disabled by default; see [bridge] rumble in the config.
//
// This exists now, unimplemented, because retrofitting bidirectionality into two base
// classes later is invasive and doing it up front costs nothing.

#include <cstdint>

namespace rgb {

enum class FeedbackKind : uint8_t {
  kNone = 0,
  kRumble,
  kPlayerLed,
};

struct FeedbackEvent {
  FeedbackKind kind = FeedbackKind::kNone;
  uint8_t low_freq  = 0;   // kRumble: heavy motor, 0..255
  uint8_t high_freq = 0;   // kRumble: light motor, 0..255
  uint8_t player    = 0;   // kPlayerLed: slot index
  uint32_t duration_ms = 0;
};

}  // namespace rgb
