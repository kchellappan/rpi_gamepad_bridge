#pragma once
// Abstraction over "a thing a console will accept input from".

#include <chrono>
#include <string>
#include "gpb/feedback.hpp"
#include "gpb/gamepad_state.hpp"

namespace gpb {

class OutputSink {
 public:
  virtual ~OutputSink() = default;

  // Slow path, deliberately separate from submit(). Enumeration, descriptor setup, and
  // -- for targets we cannot reach today -- a multi-round-trip authentication handshake.
  //
  // PS4/PS5 and Xbox consoles challenge the pad and require a response signed by a key
  // held in a licensed auth IC; the known workaround is proxying a genuine controller as
  // an auth donor. That is a conversation, not a single write, and it needs somewhere to
  // happen before any input report can flow. This phase is that somewhere.
  virtual bool initialize(std::string& err) = 0;

  // Hot path. Returns true if the state reached the wire; false means it is pending and
  // flush() must be called when writable_fd() signals EPOLLOUT.
  virtual bool submit(const GamepadState&) = 0;

  // Coalescing support. We must never build a queue of stale reports: if the host has not
  // drained the last one, the correct behaviour is to overwrite our pending copy with the
  // newest state, not to line up behind it. A queue converts a latency problem into an
  // unbounded latency problem.
  virtual bool has_pending() const { return false; }
  virtual int writable_fd() const { return -1; }
  virtual bool flush() { return true; }

  // Some hosts want a report every N even when nothing changed. Zero means event-driven
  // only.
  virtual std::chrono::nanoseconds cadence() const { return std::chrono::nanoseconds{0}; }

  // Reverse channel: descriptor that becomes readable when the host sends us something
  // (e.g. a rumble command). -1 if the sink has no back-channel.
  virtual int feedback_fd() const { return -1; }
  virtual bool read_feedback(FeedbackEvent&) { return false; }

  virtual void shutdown() {}
  virtual const char* name() const = 0;
};

}  // namespace gpb
