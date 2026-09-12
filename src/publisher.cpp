#include "gpb/publisher.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "gpb/wire.hpp"

namespace gpb {

static_assert(sizeof(sockaddr_in) <= 16, "addr_ must hold a sockaddr_in");

Publisher::~Publisher() { stop(); }

bool Publisher::start(const std::string& host, int port, const std::string& key,
                      std::string& err) {
  if (host.empty()) return true;   // disabled
  key_ = key;

  fd_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd_ < 0) {
    err = std::string("publisher socket: ") + std::strerror(errno);
    return false;
  }
  sockaddr_in dest{};
  dest.sin_family = AF_INET;
  dest.sin_port = htons(static_cast<uint16_t>(port));
  if (::inet_pton(AF_INET, host.c_str(), &dest.sin_addr) != 1) {
    err = "bridge.publish_host is not an IPv4 address: " + host;
    return false;
  }
  std::memcpy(addr_, &dest, sizeof(dest));
  std::fprintf(stderr, "[publish] capture -> %s:%d  auth=%s\n", host.c_str(), port,
               key_.empty() ? "off" : "hmac");
  return true;
}

void Publisher::publish(const GamepadState& s) {
  if (fd_ < 0) return;
  uint8_t buf[kDatagramMax];
  std::memcpy(buf, &s, sizeof(s));
  size_t len = sizeof(s);
  if (!key_.empty()) {
    hmac_tag(key_, buf, sizeof(s), buf + sizeof(s));
    len += kTagBytes;
  }
  sockaddr_in dest{};
  std::memcpy(&dest, addr_, sizeof(dest));
  // MSG_DONTWAIT as well as the non-blocking socket: this runs on the bridge's only thread,
  // and a capture consumer that has gone away must never be able to stall live input.
  if (::sendto(fd_, buf, len, MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&dest),
               sizeof(dest)) == static_cast<ssize_t>(len)) {
    ++sent_;
  } else {
    ++failed_;
  }
}

void Publisher::stop() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

}  // namespace gpb
