#pragma once
// Capture, published to another machine as it happens.
//
// Recording to a file on the Pi is enough when the Pi is the only participant. It is not
// enough when a second machine is capturing HDMI video for the same session: correlating a
// file on the Pi against frames on the rig means reconciling two clocks after the fact.
// Publishing each state to the rig as it occurs lets the consumer timestamp both streams
// against one clock instead.
//
// Fire and forget, same as the control direction. A lost capture datagram is a lost training
// sample; blocking the bridge to retransmit it would be a far worse trade.

#include <cstdint>
#include <string>

#include "gpb/gamepad_state.hpp"

namespace gpb {

class Publisher {
 public:
  ~Publisher();

  // host:port. An empty host disables publishing entirely.
  bool start(const std::string& host, int port, const std::string& key, std::string& err);
  void publish(const GamepadState& s);   // hot path: one sendto, never blocks
  void stop();

  uint64_t sent() const { return sent_; }
  uint64_t failed() const { return failed_; }

 private:
  int fd_ = -1;
  std::string key_;
  uint64_t sent_ = 0, failed_ = 0;
  // sockaddr_in, kept opaque so this header does not drag in the socket headers.
  unsigned char addr_[16] = {};
};

}  // namespace gpb
