#include "rgb/sinks/ns_hid_sink.hpp"

#include <fcntl.h>
#include <cstring>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>

namespace rgb {
namespace {

// Pokken byte 0: bit0 Y, bit1 B, bit2 A, bit3 X, bit4 L, bit5 R, bit6 ZL, bit7 ZR
constexpr uint8_t kY = 1 << 0, kB = 1 << 1, kA = 1 << 2, kX = 1 << 3;
constexpr uint8_t kL = 1 << 4, kR = 1 << 5, kZL = 1 << 6, kZR = 1 << 7;
// Pokken byte 1
constexpr uint8_t kMinus = 1 << 0, kPlus = 1 << 1, kLClick = 1 << 2, kRClick = 1 << 3;
constexpr uint8_t kHome = 1 << 4, kCapture = 1 << 5;

constexpr uint8_t kHatNeutral = 8;

uint8_t to_u8(int16_t v) {
  return static_cast<uint8_t>(std::clamp((static_cast<int>(v) + 32768) >> 8, 0, 255));
}

uint8_t encode_hat(bool up, bool down, bool left, bool right) {
  // Opposite pairs cancel; the Switch has no representation for "both".
  if (up && down) up = down = false;
  if (left && right) left = right = false;
  if (up && right) return 1;
  if (down && right) return 3;
  if (down && left) return 5;
  if (up && left) return 7;
  if (up) return 0;
  if (right) return 2;
  if (down) return 4;
  if (left) return 6;
  return kHatNeutral;
}

}  // namespace

NsHidSink::NsHidSink(const Config& cfg) {
  path_ = cfg.get("sink.ns_hid.device", "/dev/hidg0");
  face_by_position_ = cfg.get_bool("sink.ns_hid.face_by_position", true);
  const int hz = cfg.get_int("sink.ns_hid.heartbeat_hz", 0);
  if (hz > 0) cadence_ = std::chrono::nanoseconds(1000000000LL / hz);
}

NsHidSink::~NsHidSink() { shutdown(); }

bool NsHidSink::initialize(std::string& err) {
  // Non-blocking is what makes coalescing possible: a blocking write would park us
  // inside the gadget driver until the host polls, turning every stale report into
  // head-of-line latency for the fresh one behind it.
  fd_ = ::open(path_.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (fd_ < 0) {
    err = "cannot open HID gadget " + path_ + ": " + std::strerror(errno) +
          " (is the gadget up? run scripts/gadget_up.sh)";
    return false;
  }
  // Park the pad in a neutral state so the console sees a sane controller at enumeration
  // rather than whatever the struct happened to contain.
  PokkenReport neutral{};
  neutral.hat = kHatNeutral;
  neutral.lx = neutral.ly = neutral.rx = neutral.ry = 0x80;
  write_report(neutral);
  std::fprintf(stderr, "[ns_hid] gadget open at %s\n", path_.c_str());
  return true;
}

PokkenReport NsHidSink::encode(const GamepadState& s, bool face_by_position) {
  PokkenReport r{};

  if (face_by_position) {
    // Preserve the PHYSICAL position of the pressed button. A thumb reaching for the
    // bottom face button gets the Switch's bottom face button (B), regardless of what
    // letter the source pad printed there.
    if (s.pressed(btn::kSouth)) r.buttons_lo |= kB;
    if (s.pressed(btn::kEast))  r.buttons_lo |= kA;
    if (s.pressed(btn::kWest))  r.buttons_lo |= kY;
    if (s.pressed(btn::kNorth)) r.buttons_lo |= kX;
  } else {
    // Preserve the LABEL instead: an Xbox pad's "A" becomes the Switch's "A", which sits
    // in a different place. Some players want this; it is why the choice is config.
    if (s.pressed(btn::kSouth)) r.buttons_lo |= kA;
    if (s.pressed(btn::kEast))  r.buttons_lo |= kB;
    if (s.pressed(btn::kWest))  r.buttons_lo |= kX;
    if (s.pressed(btn::kNorth)) r.buttons_lo |= kY;
  }

  if (s.pressed(btn::kL1)) r.buttons_lo |= kL;
  if (s.pressed(btn::kR1)) r.buttons_lo |= kR;
  if (s.pressed(btn::kL2)) r.buttons_lo |= kZL;
  if (s.pressed(btn::kR2)) r.buttons_lo |= kZR;

  if (s.pressed(btn::kSelect)) r.buttons_hi |= kMinus;
  if (s.pressed(btn::kStart))  r.buttons_hi |= kPlus;
  if (s.pressed(btn::kL3))     r.buttons_hi |= kLClick;
  if (s.pressed(btn::kR3))     r.buttons_hi |= kRClick;
  if (s.pressed(btn::kGuide))  r.buttons_hi |= kHome;
  if (s.pressed(btn::kMisc1))  r.buttons_hi |= kCapture;

  r.hat = encode_hat(s.pressed(btn::kDUp), s.pressed(btn::kDDown), s.pressed(btn::kDLeft),
                     s.pressed(btn::kDRight));

  // No inversion here: GamepadState is down-positive and so is the HID report.
  r.lx = to_u8(s.lx);
  r.ly = to_u8(s.ly);
  r.rx = to_u8(s.rx);
  r.ry = to_u8(s.ry);
  r.vendor = 0;
  return r;
}

bool NsHidSink::write_report(const PokkenReport& r) {
  const ssize_t n = ::write(fd_, &r, sizeof(r));
  if (n == static_cast<ssize_t>(sizeof(r))) return true;
  if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return false;
  if (n < 0 && errno == ESHUTDOWN) return false;  // host not listening yet
  if (n < 0) std::fprintf(stderr, "[ns_hid] write: %s\n", std::strerror(errno));
  return false;
}

bool NsHidSink::submit(const GamepadState& s) {
  const PokkenReport r = encode(s, face_by_position_);
  if (write_report(r)) {
    pending_ = false;
    return true;
  }
  // Could not place it on the wire. Overwrite whatever was queued rather than appending:
  // the newest state is the only one that matters, and a backlog of stale reports would
  // turn a bounded latency into an unbounded one.
  pending_report_ = r;
  pending_ = true;
  return false;
}

bool NsHidSink::flush() {
  if (!pending_) return true;
  if (write_report(pending_report_)) {
    pending_ = false;
    return true;
  }
  return false;
}

void NsHidSink::shutdown() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

}  // namespace rgb
