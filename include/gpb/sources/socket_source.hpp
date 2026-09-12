#pragma once
// Programmatic control: a client application drives the output directly.
//
// Unix SOCK_SEQPACKET rather than TCP loopback -- message framing is free, there is no
// Nagle/delayed-ACK interaction to defeat, and it skips the IP stack entirely. If a
// remote driver is ever wanted, that is a separate UdpSource, not a reason to compromise
// the local path.
//
// The payload is a raw GamepadState, identical to what Recorder writes. One recv()
// straight into the struct, no parsing, and captured sessions replay byte-for-byte.
// (uinput would have let clients use standard APIs, but it would have forced the inject
// format to be evdev's event stream while capture stayed GamepadState -- and then the two
// directions no longer agree, which is exactly what the VLA use case cannot tolerate.)

#include <string>
#include "gpb/config.hpp"
#include "gpb/input_source.hpp"

namespace gpb {

class SocketSource final : public InputSource {
 public:
  explicit SocketSource(const Config& cfg);
  ~SocketSource() override;

  bool initialize(std::string& err) override;
  int fd() const override { return client_ >= 0 ? client_ : listen_; }
  bool read(GamepadState& out) override;
  void shutdown() override;
  const char* name() const override { return "socket"; }

  // The client speaks canonical action space (it is usually replaying a capture, or a
  // policy emitting into the same space it was trained on), so the mapping layer must
  // not run over it a second time.
  bool emits_canonical() const override { return true; }

 private:
  std::string path_;
  unsigned mode_ = 0660;
  std::string group_;
  int listen_ = -1;
  int client_ = -1;
  uint32_t seq_ = 0;
};

}  // namespace gpb
