#include "rgb/sources/evdev_source.hpp"

#include <fcntl.h>
#include <cstring>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include "rgb/rt.hpp"

namespace rgb {
namespace {

struct NamedCode {
  const char* name;
  uint16_t code;
};

// Only the codes a gamepad can plausibly emit. Keeping the table small keeps the config
// vocabulary honest.
constexpr std::array<NamedCode, 16> kAbsCodes{{
    {"ABS_X", ABS_X},           {"ABS_Y", ABS_Y},         {"ABS_Z", ABS_Z},
    {"ABS_RX", ABS_RX},         {"ABS_RY", ABS_RY},       {"ABS_RZ", ABS_RZ},
    {"ABS_THROTTLE", ABS_THROTTLE}, {"ABS_RUDDER", ABS_RUDDER},
    {"ABS_WHEEL", ABS_WHEEL},   {"ABS_GAS", ABS_GAS},     {"ABS_BRAKE", ABS_BRAKE},
    {"ABS_HAT0X", ABS_HAT0X},   {"ABS_HAT0Y", ABS_HAT0Y}, {"ABS_HAT1X", ABS_HAT1X},
    {"ABS_HAT1Y", ABS_HAT1Y},   {"ABS_TILT_X", ABS_TILT_X},
}};

constexpr std::array<NamedCode, 30> kKeyCodes{{
    {"BTN_SOUTH", BTN_SOUTH},   {"BTN_A", BTN_A},         {"BTN_EAST", BTN_EAST},
    {"BTN_B", BTN_B},           {"BTN_NORTH", BTN_NORTH}, {"BTN_X", BTN_X},
    {"BTN_WEST", BTN_WEST},     {"BTN_Y", BTN_Y},         {"BTN_C", BTN_C},
    {"BTN_Z", BTN_Z},           {"BTN_TL", BTN_TL},       {"BTN_TR", BTN_TR},
    {"BTN_TL2", BTN_TL2},       {"BTN_TR2", BTN_TR2},     {"BTN_SELECT", BTN_SELECT},
    {"BTN_START", BTN_START},   {"BTN_MODE", BTN_MODE},   {"BTN_THUMBL", BTN_THUMBL},
    {"BTN_THUMBR", BTN_THUMBR}, {"BTN_DPAD_UP", BTN_DPAD_UP},
    {"BTN_DPAD_DOWN", BTN_DPAD_DOWN}, {"BTN_DPAD_LEFT", BTN_DPAD_LEFT},
    {"BTN_DPAD_RIGHT", BTN_DPAD_RIGHT},
    {"BTN_TRIGGER_HAPPY1", BTN_TRIGGER_HAPPY1}, {"BTN_TRIGGER_HAPPY2", BTN_TRIGGER_HAPPY2},
    {"BTN_TRIGGER_HAPPY3", BTN_TRIGGER_HAPPY3}, {"BTN_TRIGGER_HAPPY4", BTN_TRIGGER_HAPPY4},
    {"KEY_BACK", KEY_BACK},     {"KEY_HOMEPAGE", KEY_HOMEPAGE},
    {"BTN_TRIGGER", BTN_TRIGGER},
}};

struct NamedMask {
  const char* name;
  uint32_t mask;
};
constexpr std::array<NamedMask, 18> kButtonTargets{{
    {"south", btn::kSouth},   {"east", btn::kEast},     {"west", btn::kWest},
    {"north", btn::kNorth},   {"l1", btn::kL1},         {"r1", btn::kR1},
    {"l2", btn::kL2},         {"r2", btn::kR2},         {"select", btn::kSelect},
    {"start", btn::kStart},   {"l3", btn::kL3},         {"r3", btn::kR3},
    {"guide", btn::kGuide},   {"misc1", btn::kMisc1},   {"dup", btn::kDUp},
    {"ddown", btn::kDDown},   {"dleft", btn::kDLeft},   {"dright", btn::kDRight},
}};

AxisTarget axis_target_from_name(const std::string& n) {
  if (n == "lx") return AxisTarget::kLX;
  if (n == "ly") return AxisTarget::kLY;
  if (n == "rx") return AxisTarget::kRX;
  if (n == "ry") return AxisTarget::kRY;
  if (n == "lt") return AxisTarget::kLT;
  if (n == "rt") return AxisTarget::kRT;
  if (n == "hatx") return AxisTarget::kHatX;
  if (n == "haty") return AxisTarget::kHatY;
  return AxisTarget::kNone;
}

uint32_t button_mask_from_name(const std::string& n) {
  for (const auto& e : kButtonTargets)
    if (n == e.name) return e.mask;
  return 0;
}

}  // namespace

uint16_t evdev_code_from_name(const std::string& name, bool& ok) {
  ok = true;
  for (const auto& e : kAbsCodes)
    if (name == e.name) return e.code;
  for (const auto& e : kKeyCodes)
    if (name == e.name) return e.code;
  ok = false;
  return 0;
}

std::string evdev_abs_name(uint16_t code) {
  for (const auto& e : kAbsCodes)
    if (code == e.code) return e.name;
  return "ABS_" + std::to_string(code);
}

std::string evdev_key_name(uint16_t code) {
  for (const auto& e : kKeyCodes)
    if (code == e.code) return e.name;
  return "KEY_" + std::to_string(code);
}

EvdevSource::EvdevSource(const Config& cfg) {
  path_ = cfg.get("source.evdev.device");
  grab_ = cfg.get_bool("source.evdev.grab", true);
  bind_from_config(cfg);
}

EvdevSource::~EvdevSource() { shutdown(); }

void EvdevSource::bind_from_config(const Config& cfg) {
  // Config keys look like:  axis.ABS_X = lx        button.BTN_SOUTH = south
  // Bindings are keyed by evdev code so the hot path is a map lookup on a small integer.
  for (const auto& [key, val] : cfg.all()) {
    const std::string prefix_axis = "source.evdev.axis.";
    const std::string prefix_btn = "source.evdev.button.";
    if (key.rfind(prefix_axis, 0) == 0) {
      bool ok = false;
      uint16_t code = evdev_code_from_name(key.substr(prefix_axis.size()), ok);
      if (!ok) continue;
      std::string target = val;
      bool invert = false;
      if (!target.empty() && target[0] == '-') {
        invert = true;
        target = target.substr(1);
      }
      AxisBinding b;
      b.target = axis_target_from_name(target);
      b.invert = invert;
      if (b.target != AxisTarget::kNone) axes_[code] = b;
    } else if (key.rfind(prefix_btn, 0) == 0) {
      bool ok = false;
      uint16_t code = evdev_code_from_name(key.substr(prefix_btn.size()), ok);
      if (!ok) continue;
      uint32_t mask = button_mask_from_name(val);
      if (mask) buttons_[code] = mask;
    }
  }
}

void EvdevSource::autodetect_ranges() {
  // Ask the kernel for each axis's real range rather than assuming 0..255. Controllers
  // disagree wildly here (Stadia's sticks are 8-bit, an Xbox pad's are 16-bit signed),
  // and this is exactly the sort of per-device fact the config should not have to carry.
  for (auto& [code, b] : axes_) {
    input_absinfo info{};
    if (ioctl(fd_, EVIOCGABS(code), &info) == 0) {
      b.min = info.minimum;
      b.max = info.maximum;
      b.flat = info.flat;
    }
  }
}

bool EvdevSource::initialize(std::string& err) {
  if (path_.empty()) {
    err = "source.evdev.device is not set (run rgb-discover to find it)";
    return false;
  }
  fd_ = ::open(path_.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd_ < 0) {
    err = "cannot open " + path_ + ": " + std::strerror(errno);
    return false;
  }

  // Make event timestamps monotonic. Without this the kernel stamps them with realtime,
  // which can step (NTP, suspend) and makes latency math quietly wrong.
  int clk = CLOCK_MONOTONIC;
  if (ioctl(fd_, EVIOCSCLOCKID, &clk) != 0)
    std::fprintf(stderr, "[evdev] EVIOCSCLOCKID failed; timestamps will be realtime\n");

  autodetect_ranges();

  if (grab_) {
    // Exclusive access, so the Pi's own input stack does not also react to the pad while
    // we are forwarding it to a console.
    if (ioctl(fd_, EVIOCGRAB, 1) != 0)
      std::fprintf(stderr, "[evdev] EVIOCGRAB failed; the Pi will also see this input\n");
  }

  char devname[256] = {0};
  if (ioctl(fd_, EVIOCGNAME(sizeof(devname) - 1), devname) >= 0)
    std::fprintf(stderr, "[evdev] opened \"%s\" (%zu axes, %zu buttons bound)\n", devname,
                 axes_.size(), buttons_.size());

  if (axes_.empty() && buttons_.empty()) {
    err = "no axis or button bindings configured for " + path_;
    return false;
  }
  return true;
}

int16_t EvdevSource::scale_axis(const AxisBinding& b, int32_t raw) const {
  const int32_t span = b.max - b.min;
  if (span <= 0) return 0;
  // Map [min,max] -> [-32767, 32767].
  const double t = static_cast<double>(raw - b.min) / static_cast<double>(span);
  double v = (t * 2.0 - 1.0) * 32767.0;
  if (b.invert) v = -v;
  return static_cast<int16_t>(std::clamp(v, -32767.0, 32767.0));
}

bool EvdevSource::read(GamepadState& out) {
  input_event evs[64];
  bool complete = false;

  while (true) {
    const ssize_t n = ::read(fd_, evs, sizeof(evs));
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      if (errno == EINTR) continue;
      std::fprintf(stderr, "[evdev] read error: %s\n", std::strerror(errno));
      break;
    }
    if (n == 0) break;

    const size_t count = static_cast<size_t>(n) / sizeof(input_event);
    for (size_t i = 0; i < count; ++i) {
      const input_event& e = evs[i];
      switch (e.type) {
        case EV_ABS: {
          auto it = axes_.find(e.code);
          if (it == axes_.end()) break;
          const AxisBinding& b = it->second;
          switch (b.target) {
            case AxisTarget::kLX: acc_.lx = scale_axis(b, e.value); break;
            case AxisTarget::kLY: acc_.ly = scale_axis(b, e.value); break;
            case AxisTarget::kRX: acc_.rx = scale_axis(b, e.value); break;
            case AxisTarget::kRY: acc_.ry = scale_axis(b, e.value); break;
            case AxisTarget::kLT: {
              const int32_t span = std::max(b.max - b.min, 1);
              acc_.lt = static_cast<uint8_t>(
                  std::clamp((e.value - b.min) * 255 / span, 0, 255));
              break;
            }
            case AxisTarget::kRT: {
              const int32_t span = std::max(b.max - b.min, 1);
              acc_.rt = static_cast<uint8_t>(
                  std::clamp((e.value - b.min) * 255 / span, 0, 255));
              break;
            }
            case AxisTarget::kHatX: hat_x_ = e.value; break;
            case AxisTarget::kHatY: hat_y_ = e.value; break;
            default: break;
          }
          dirty_ = true;
          break;
        }
        case EV_KEY: {
          auto it = buttons_.find(e.code);
          if (it == buttons_.end()) break;
          // value 2 is autorepeat; treat anything non-zero as held.
          acc_.set(it->second, e.value != 0);
          dirty_ = true;
          break;
        }
        case EV_SYN:
          if (e.code == SYN_REPORT && dirty_) {
            // The batch is closed: this is the first moment the accumulated state is
            // coherent across all axes. Publishing before now is the diagonal-snap bug.
            acc_.t_mono_ns = static_cast<uint64_t>(e.time.tv_sec) * 1000000000ull +
                             static_cast<uint64_t>(e.time.tv_usec) * 1000ull;
            complete = true;
            dirty_ = false;
          } else if (e.code == SYN_DROPPED) {
            // The kernel's buffer overflowed and we lost events, so our accumulator may
            // disagree with the device. Re-sync ranges and keep going.
            std::fprintf(stderr, "[evdev] SYN_DROPPED: state may have desynced\n");
          }
          break;
        default:
          break;
      }
    }
  }

  if (!complete) return false;

  // Hat axes are -1/0/1; fold them into the d-pad bits so sinks see one representation.
  acc_.set(btn::kDLeft, hat_x_ < 0);
  acc_.set(btn::kDRight, hat_x_ > 0);
  acc_.set(btn::kDUp, hat_y_ < 0);
  acc_.set(btn::kDDown, hat_y_ > 0);

  acc_.seq = ++seq_;
  acc_.t_real_ns = now_real_ns();
  out = acc_;
  return true;
}

void EvdevSource::shutdown() {
  if (fd_ >= 0) {
    if (grab_) ioctl(fd_, EVIOCGRAB, 0);
    ::close(fd_);
    fd_ = -1;
  }
}

}  // namespace rgb
