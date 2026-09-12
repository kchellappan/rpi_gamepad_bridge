#include "gpb/sources/udp_source.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "gpb/rt.hpp"
#include "gpb/wire.hpp"

namespace gpb {

UdpSource::UdpSource(const Config& cfg) {
  bind_addr_ = cfg.get("source.udp.bind", "0.0.0.0");
  port_ = cfg.get_int("source.udp.port", 9871);
  peer_ = cfg.get("source.udp.peer");
  key_ = cfg.get("source.udp.key");
}

UdpSource::~UdpSource() { shutdown(); }

bool UdpSource::initialize(std::string& err) {
  fd_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd_ < 0) {
    err = std::string("udp socket: ") + std::strerror(errno);
    return false;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port_));
  if (::inet_pton(AF_INET, bind_addr_.c_str(), &addr.sin_addr) != 1) {
    err = "source.udp.bind is not an IPv4 address: " + bind_addr_;
    return false;
  }
  if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    err = "bind " + bind_addr_ + ":" + std::to_string(port_) + ": " + std::strerror(errno);
    return false;
  }

  std::fprintf(stderr, "[udp] listening on %s:%d  auth=%s  peer=%s\n", bind_addr_.c_str(),
               port_, key_.empty() ? "off" : "hmac",
               peer_.empty() ? "any" : peer_.c_str());
  if (key_.empty()) {
    // Worth saying out loud: this port injects controller input into whatever console the
    // gadget is plugged into.
    std::fprintf(stderr, "[udp] WARNING: no key set, so anything that can reach this port "
                         "can drive the gamepad\n");
  }
  return true;
}

bool UdpSource::read(GamepadState& out) {
  uint8_t buf[kDatagramMax];
  bool got = false;

  while (true) {
    sockaddr_in from{};
    socklen_t fromlen = sizeof(from);
    const ssize_t n = ::recvfrom(fd_, buf, sizeof(buf), 0,
                                 reinterpret_cast<sockaddr*>(&from), &fromlen);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      if (errno == EINTR) continue;
      std::fprintf(stderr, "[udp] recvfrom: %s\n", std::strerror(errno));
      break;
    }

    if (!peer_.empty()) {
      char ip[INET_ADDRSTRLEN] = {0};
      ::inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
      if (peer_ != ip) {
        ++counters_.wrong_peer;
        continue;
      }
    }

    const size_t expected = sizeof(GamepadState) + (key_.empty() ? 0 : kTagBytes);
    if (static_cast<size_t>(n) != expected) {
      ++counters_.bad_frame;
      continue;
    }

    if (!key_.empty()) {
      uint8_t want[kTagBytes];
      hmac_tag(key_, buf, sizeof(GamepadState), want);
      if (!tag_equals(want, buf + sizeof(GamepadState), kTagBytes)) {
        ++counters_.bad_tag;
        continue;
      }
    }

    GamepadState in{};
    std::memcpy(&in, buf, sizeof(in));
    if (in.magic != kWireMagic || in.version != kWireVersion) {
      ++counters_.bad_frame;
      continue;
    }

    // Order of arrival is not order of sending. A datagram that lost a race has already been
    // superseded, and applying it would move the controller backwards.
    //
    // Comparing as a wrapped difference rather than "greater than" so a client that runs
    // long enough to wrap its counter does not stall the link for the second half of the
    // sequence space.
    if (have_seq_ && static_cast<int32_t>(in.seq - last_seq_) <= 0) {
      ++counters_.stale;
      continue;
    }
    last_seq_ = in.seq;
    have_seq_ = true;

    out = in;
    got = true;
    ++counters_.accepted;
  }

  if (!got) return false;
  // Stamp arrival locally: the sender's clock is a different machine's, and nothing here
  // should depend on the two being synchronised.
  out.t_mono_ns = now_mono_ns();
  out.t_real_ns = now_real_ns();
  return true;
}

void UdpSource::shutdown() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

}  // namespace gpb
