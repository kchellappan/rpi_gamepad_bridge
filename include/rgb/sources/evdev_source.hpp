#pragma once
// The workhorse source. Covers every physical pad in this project -- Stadia, PDP Xbox,
// Switch Pro, DualSense -- because the kernel has already normalized each of them into
// axes and buttons. Supporting a new pad is a config table, not a new class.
//
// Reads struct input_event straight off the character device rather than linking
// libevdev: it is one dependency fewer to cross-compile, and read() on the raw fd is the
// shortest path from kernel to state.

#include <linux/input.h>
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include "rgb/config.hpp"
#include "rgb/input_source.hpp"

namespace rgb {

// Which normalized field an evdev code feeds.
enum class AxisTarget : uint8_t { kNone, kLX, kLY, kRX, kRY, kLT, kRT, kHatX, kHatY };

struct AxisBinding {
  AxisTarget target = AxisTarget::kNone;
  int32_t min = 0, max = 255, flat = 0;
  bool invert = false;
};

class EvdevSource final : public InputSource {
 public:
  explicit EvdevSource(const Config& cfg);
  ~EvdevSource() override;

  bool initialize(std::string& err) override;
  int fd() const override { return fd_; }
  bool read(GamepadState& out) override;
  void shutdown() override;
  const char* name() const override { return "evdev"; }

 private:
  void bind_from_config(const Config& cfg);
  void autodetect_ranges();
  int16_t scale_axis(const AxisBinding& b, int32_t raw) const;

  std::string path_;
  bool grab_ = false;
  int fd_ = -1;

  std::map<uint16_t, AxisBinding> axes_;   // ABS_* code -> binding
  std::map<uint16_t, uint32_t> buttons_;   // BTN_/KEY_ code -> rgb::btn mask

  GamepadState acc_{};   // accumulator; only published on SYN_REPORT
  int32_t hat_x_ = 0, hat_y_ = 0;
  uint32_t seq_ = 0;
  bool dirty_ = false;
};

// evdev code-name <-> number, so the discover tool emits config that is directly
// pasteable and the config file stays readable.
uint16_t evdev_code_from_name(const std::string& name, bool& ok);
std::string evdev_abs_name(uint16_t code);
std::string evdev_key_name(uint16_t code);

}  // namespace rgb
