#pragma once
// Abstraction over "a thing that produces gamepad state".
//
// Note what this is NOT for: it is not an abstraction over gamepads. The kernel's evdev
// layer already normalizes arbitrary HID gamepads into axes and buttons, so every
// physical pad we care about is served by the single EvdevSource, configured by a table.
// This interface earns its keep at the edges -- a CAN joystick, a socket client, a raw
// HID device we deliberately unbound from its driver -- i.e. things evdev cannot express.

#include <string>
#include "rgb/feedback.hpp"
#include "rgb/gamepad_state.hpp"

namespace rgb {

class InputSource {
 public:
  virtual ~InputSource() = default;

  // Slow path. Open devices, connect, negotiate. Blocking I/O is legal here and only
  // here. Returns false and sets err on failure.
  virtual bool initialize(std::string& err) = 0;

  // Pollable descriptor for the event loop, or -1 for a source that never becomes
  // readable on its own.
  virtual int fd() const = 0;

  // Hot path. Called when fd() is readable. Writes into `out` and returns true ONLY on a
  // complete, coherent update.
  //
  // The "complete" part is load-bearing: evdev delivers a batch of events terminated by
  // EV_SYN/SYN_REPORT, and emitting mid-batch ships states where X has moved but Y has
  // not. On a diagonal stick push that renders as travel along one axis followed by a
  // snap to the diagonal -- it looks exactly like a jitter bug, and it is not one.
  virtual bool read(GamepadState& out) = 0;

  // True when this source already emits post-transform, canonical action-space state --
  // i.e. it is replaying what Recorder captured. The Bridge then skips the mapping layer,
  // because deadzones and expo curves are NOT idempotent: applying them a second time
  // silently rescales every stick value, so a replayed session would not reproduce the
  // run it was captured from. That would quietly invalidate the whole VLA loop, since
  // capture and inject are supposed to agree bit-for-bit.
  virtual bool emits_canonical() const { return false; }

  // Reverse channel. Default no-op so that sources with no motors ignore it for free.
  virtual void on_feedback(const FeedbackEvent&) {}

  virtual void shutdown() {}
  virtual const char* name() const = 0;
};

}  // namespace rgb
