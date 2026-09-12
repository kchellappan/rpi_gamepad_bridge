#pragma once
// Control input over the network.
//
// Exists because the machine running inference cannot be the USB gadget: a workstation's
// USB ports are host-only, with no device-side controller, so no software makes it present
// itself as a controller. The Pi stays in the path and takes its instructions over Ethernet.
//
// UDP rather than TCP, deliberately. TCP guarantees ordering and retransmission, which are
// both wrong here -- a retransmitted stale gamepad state is worse than a dropped one. It is
// the same reason the sink coalesces instead of queueing: a queue turns a latency problem
// into an unbounded one, and TCP is a queue with a retry policy.

#include <cstdint>
#include <string>

#include "gpb/config.hpp"
#include "gpb/input_source.hpp"

namespace gpb {

class UdpSource final : public InputSource {
 public:
  explicit UdpSource(const Config& cfg);
  ~UdpSource() override;

  bool initialize(std::string& err) override;
  int fd() const override { return fd_; }
  bool read(GamepadState& out) override;
  void set_capabilities(const std::string& json) override { caps_json_ = json; }
  std::string counters_json() const override;
  void shutdown() override;
  const char* name() const override { return "udp"; }

  // The client speaks canonical action space, exactly as the Unix socket client does, so the
  // mapping layer must not run over it a second time.
  bool emits_canonical() const override { return true; }

  struct Counters {
    uint64_t accepted = 0;
    uint64_t stale = 0;        // arrived out of order; superseded before it landed
    uint64_t bad_tag = 0;      // failed authentication
    uint64_t bad_frame = 0;    // wrong size, magic or version
    uint64_t wrong_peer = 0;
    uint64_t queries = 0;
    uint64_t sessions = 0;
  };
  const Counters& counters() const { return counters_; }

 private:
  std::string bind_addr_;
  int port_ = 0;
  std::string peer_;          // empty accepts any source address
  std::string key_;           // empty disables authentication
  std::string caps_json_;
  int fd_ = -1;
  uint32_t last_seq_ = 0;
  bool have_seq_ = false;

  // Sequence numbers are only comparable within one client session. A client that restarts
  // begins again at 1, which is "older" than everything the previous run sent -- so without
  // detecting the new session, a restarted client is locked out permanently and silently.
  uint32_t last_from_ip_ = 0;
  uint16_t last_from_port_ = 0;
  uint64_t last_accept_ns_ = 0;
  uint64_t session_gap_ns_ = 0;
  Counters counters_{};
};

}  // namespace gpb
