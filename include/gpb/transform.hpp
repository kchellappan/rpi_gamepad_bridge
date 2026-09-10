#pragma once
// The mapping / profile layer, which sits between source and sink.
//
// It lives here rather than inside either one because the moment deadzones or remapping
// leak into a source, that source stops being generic -- you end up with an EvdevSource
// that knows Switch button names.
//
// This is also the designated hook for custom injection logic. That means third-party
// code runs in the hot path, so the contract is explicit: apply() must be non-blocking,
// allocation-free, and is budgeted at ~100us. Exceed it and you are eating directly into
// the latency budget the rest of the design works to protect.

#include <memory>
#include <string>
#include <vector>
#include "gpb/gamepad_state.hpp"

namespace gpb {

class Transform {
 public:
  virtual ~Transform() = default;
  virtual void apply(GamepadState&) = 0;
  virtual const char* name() const = 0;
};

// Per-stick shaping. The common case, kept as plain data so it stays cheap.
struct StickProfile {
  int16_t deadzone = 0;      // radial, applied to the pair
  int16_t saturation = 32767;
  float expo = 1.0f;         // 1.0 = linear; >1 softens around center
  bool invert_x = false;
  bool invert_y = false;
};

class ProfileTransform final : public Transform {
 public:
  ProfileTransform(StickProfile left, StickProfile right, uint8_t trigger_deadzone)
      : left_(left), right_(right), trigger_deadzone_(trigger_deadzone) {}

  void apply(GamepadState& s) override;
  const char* name() const override { return "profile"; }

 private:
  StickProfile left_, right_;
  uint8_t trigger_deadzone_;
};

}  // namespace gpb
