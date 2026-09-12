#pragma once
// Nintendo Switch output, by presenting as a HORI Pokken Tournament Pro Pad.
//
// Why this device specifically: it is a plain USB HID gamepad with no authentication
// handshake, which is what makes the Switch reachable at all. PS4/PS5 and Xbox consoles
// challenge the pad for a signature from a licensed auth IC; no descriptor trickery gets
// past that. See README for the full target matrix.
//
// Writes 8-byte reports to the HID gadget character device created by scripts/gadget_up.sh.

#include <chrono>
#include <string>
#include "gpb/config.hpp"
#include "gpb/output_sink.hpp"

namespace gpb {

#pragma pack(push, 1)
struct PokkenReport {
  uint8_t buttons_lo;  // bit0 Y, 1 B, 2 A, 3 X, 4 L, 5 R, 6 ZL, 7 ZR
  uint8_t buttons_hi;  // bit0 Minus, 1 Plus, 2 LClick, 3 RClick, 4 Home, 5 Capture
  uint8_t hat;         // 0=N,1=NE,2=E,3=SE,4=S,5=SW,6=W,7=NW,8=neutral
  uint8_t lx, ly, rx, ry;  // 0..255, 128 center
  uint8_t vendor;      // constant padding byte in the descriptor
};
#pragma pack(pop)
static_assert(sizeof(PokkenReport) == 8, "Pokken report must be 8 bytes");

class NsHidSink final : public OutputSink {
 public:
  explicit NsHidSink(const Config& cfg);
  ~NsHidSink() override;

  bool initialize(std::string& err) override;
  bool submit(const GamepadState&) override;

  bool has_pending() const override { return pending_; }
  int writable_fd() const override { return fd_; }
  bool flush() override;

  std::chrono::nanoseconds cadence() const override { return cadence_; }
  Capabilities capabilities() const override;
  void shutdown() override;
  const char* name() const override { return "ns_hid"; }

  // Exposed for tests and for the discover/verify tooling.
  static PokkenReport encode(const GamepadState&, bool face_by_position);

 private:
  bool write_report(const PokkenReport&);

  std::string path_ = "/dev/hidg0";
  int fd_ = -1;
  bool pending_ = false;
  int last_errno_ = 0;   // errno from the most recent write, captured before it can be clobbered
  bool face_by_position_ = true;
  PokkenReport pending_report_{};
  std::chrono::nanoseconds cadence_{0};
};

}  // namespace gpb
