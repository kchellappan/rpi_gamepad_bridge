#include "gpb_client/client.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "gpb/wire.hpp"

namespace gpb::client {
namespace {

uint64_t mono_ns() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + ts.tv_nsec;
}

uint64_t real_ns() {
  timespec ts{};
  clock_gettime(CLOCK_REALTIME, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + ts.tv_nsec;
}

std::string json_string(const std::string& doc, const std::string& key) {
  const std::string needle = "\"" + key + "\":\"";
  const size_t at = doc.find(needle);
  if (at == std::string::npos) return "";
  const size_t start = at + needle.size();
  std::string out;
  for (size_t i = start; i < doc.size(); ++i) {
    if (doc[i] == '\\' && i + 1 < doc.size()) { out += doc[++i]; continue; }
    if (doc[i] == '"') break;
    out += doc[i];
  }
  return out;
}

}  // namespace

Capabilities query_capabilities(const std::string& host, int port, int timeout_ms) {
  Capabilities caps;
  const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return caps;

  sockaddr_in dest{};
  dest.sin_family = AF_INET;
  dest.sin_port = htons(static_cast<uint16_t>(port));
  if (::inet_pton(AF_INET, host.c_str(), &dest.sin_addr) != 1) {
    ::close(fd);
    return caps;
  }
  ::sendto(fd, kQueryCaps, kQueryCapsLen, 0, reinterpret_cast<sockaddr*>(&dest), sizeof(dest));

  pollfd p{fd, POLLIN, 0};
  if (::poll(&p, 1, timeout_ms) > 0) {
    char buf[2048];
    const ssize_t n = ::recv(fd, buf, sizeof(buf) - 1, 0);
    if (n > 0) {
      buf[n] = '\0';
      caps.raw_json.assign(buf, static_cast<size_t>(n));
      caps.sink = json_string(caps.raw_json, "sink");
      caps.target = json_string(caps.raw_json, "target");
      caps.trigger_mode = json_string(caps.raw_json, "trigger_mode");
      caps.notes = json_string(caps.raw_json, "notes");
      caps.ok = !caps.target.empty();
    }
  }
  ::close(fd);
  return caps;
}

ControlClient::ControlClient(Options opts) : opts_(std::move(opts)) {}
ControlClient::~ControlClient() { close(); }

bool ControlClient::connect(std::string& err) {
  if (!opts_.unix_path.empty()) {
    is_unix_ = true;
    fd_ = ::socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (fd_ < 0) { err = std::strerror(errno); return false; }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, opts_.unix_path.c_str(), sizeof(addr.sun_path) - 1);
    if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      err = "connect " + opts_.unix_path + ": " + std::strerror(errno);
      return false;
    }
  } else {
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) { err = std::strerror(errno); return false; }
    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(static_cast<uint16_t>(opts_.port));
    if (::inet_pton(AF_INET, opts_.host.c_str(), &dest.sin_addr) != 1) {
      err = "not an IPv4 address: " + opts_.host;
      return false;
    }
    std::memcpy(dest_, &dest, sizeof(dest));
    dest_len_ = sizeof(dest);
  }

  if (opts_.stream) {
    running_ = true;
    thread_ = std::thread(&ControlClient::stream_loop, this);
  }
  return true;
}

bool ControlClient::send() {
  if (fd_ < 0) return false;
  uint8_t buf[kDatagramMax];
  GamepadState s = state_;
  s.seq = seq_.fetch_add(1) + 1;
  s.t_mono_ns = mono_ns();
  s.t_real_ns = real_ns();
  std::memcpy(buf, &s, sizeof(s));
  size_t len = sizeof(s);
  if (!opts_.key.empty()) {
    hmac_tag(opts_.key, buf, sizeof(s), buf + sizeof(s));
    len += kTagBytes;
  }

  ssize_t n;
  if (is_unix_) {
    n = ::send(fd_, buf, len, MSG_NOSIGNAL);
  } else {
    sockaddr_in dest{};
    std::memcpy(&dest, dest_, sizeof(dest));
    n = ::sendto(fd_, buf, len, 0, reinterpret_cast<sockaddr*>(&dest), dest_len_);
  }
  if (n == static_cast<ssize_t>(len)) { ++sent_; return true; }
  return false;
}

// Stream continuously rather than only on change.
//
// UDP drops datagrams, and a client that sends only on change loses the EVENT, not a
// sample: a "button down" that goes missing never happens at all, and nothing downstream
// can tell. At 125 Hz any loss is corrected within 8 ms.
//
// This is the same failure the console taught this project directly -- with change-only
// reporting, sticks looked perfect while buttons were mostly ignored, because an axis value
// is absolute and survives a gap where a press does not.
void ControlClient::stream_loop() {
  const auto interval = std::chrono::duration<double>(1.0 / (opts_.rate_hz > 0 ? opts_.rate_hz : 125.0));
  while (running_) {
    send();
    std::this_thread::sleep_for(std::chrono::duration_cast<std::chrono::microseconds>(interval));
  }
}

void ControlClient::press(uint32_t mask) { state_.buttons |= mask; send(); }
void ControlClient::release(uint32_t mask) { state_.buttons &= ~mask; send(); }

// Hold long enough for a console to accept it. The gadget endpoint is polled once per
// millisecond, but a console samples across several polls before treating a press as real,
// so a tap of a few milliseconds is unreliable on actual hardware.
void ControlClient::tap(uint32_t mask, int hold_ms) {
  press(mask);
  std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
  release(mask);
}

void ControlClient::set_axes(int16_t lx, int16_t ly, int16_t rx, int16_t ry) {
  state_.lx = lx; state_.ly = ly; state_.rx = rx; state_.ry = ry;
  send();
}

void ControlClient::set_triggers(uint8_t lt, uint8_t rt) {
  state_.lt = lt; state_.rt = rt;
  send();
}

void ControlClient::close() {
  if (running_.exchange(false) && thread_.joinable()) thread_.join();
  if (fd_ >= 0) {
    // Release everything on the way out: the bridge holds the last state it received, so
    // exiting mid-press would leave that button held on the console indefinitely.
    state_ = GamepadState{};
    send();
    ::close(fd_);
    fd_ = -1;
  }
}

CaptureReceiver::CaptureReceiver(int port, std::string bind_addr, std::string key)
    : port_(port), bind_addr_(std::move(bind_addr)), key_(std::move(key)) {}
CaptureReceiver::~CaptureReceiver() { close(); }

bool CaptureReceiver::open(std::string& err) {
  fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd_ < 0) { err = std::strerror(errno); return false; }
  int one = 1;
  ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port_));
  if (::inet_pton(AF_INET, bind_addr_.c_str(), &addr.sin_addr) != 1) {
    err = "not an IPv4 address: " + bind_addr_;
    return false;
  }
  if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    err = "bind: " + std::string(std::strerror(errno));
    return false;
  }
  return true;
}

bool CaptureReceiver::recv(GamepadState& out, int timeout_ms) {
  if (fd_ < 0) return false;
  pollfd p{fd_, POLLIN, 0};
  if (::poll(&p, 1, timeout_ms) <= 0) return false;

  uint8_t buf[kDatagramMax];
  const ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
  const size_t expected = sizeof(GamepadState) + (key_.empty() ? 0 : kTagBytes);
  if (n < 0 || static_cast<size_t>(n) != expected) { ++rejected_; return false; }

  if (!key_.empty()) {
    uint8_t want[kTagBytes];
    hmac_tag(key_, buf, sizeof(GamepadState), want);
    if (!tag_equals(want, buf + sizeof(GamepadState), kTagBytes)) { ++rejected_; return false; }
  }
  GamepadState s{};
  std::memcpy(&s, buf, sizeof(s));
  if (s.magic != kWireMagic || s.version != kWireVersion) { ++rejected_; return false; }
  out = s;
  return true;
}

void CaptureReceiver::close() {
  if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
}

}  // namespace gpb::client
