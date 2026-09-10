#pragma once
// Capture path for VLA training data.
//
// Writes the exact same GamepadState struct that the socket source accepts, which is the
// whole point: a captured session replays bit-for-bit, so the capture format doubles as
// its own regression test.
//
// Disk I/O must never touch the hot path, so record() only copies into a preallocated
// ring and a writer thread drains it. If the ring fills we drop and count, rather than
// blocking the bridge -- a dropped training sample is recoverable, a stalled control loop
// during live play is not.

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>
#include "rgb/gamepad_state.hpp"

namespace rgb {

class Recorder {
 public:
  ~Recorder();

  bool start(const std::string& path, size_t ring_slots, std::string& err);
  void record(const GamepadState& s);  // hot path: copy + release store, nothing else
  void stop();

  uint64_t written() const { return written_.load(std::memory_order_relaxed); }
  uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

 private:
  void writer_loop();

  std::vector<GamepadState> ring_;
  size_t mask_ = 0;
  std::atomic<uint64_t> head_{0};  // producer (bridge thread)
  std::atomic<uint64_t> tail_{0};  // consumer (writer thread)
  std::atomic<bool> running_{false};
  std::atomic<uint64_t> written_{0}, dropped_{0};
  std::thread thread_;
  std::FILE* file_ = nullptr;
};

}  // namespace rgb
