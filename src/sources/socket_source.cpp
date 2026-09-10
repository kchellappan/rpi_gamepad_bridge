#include "gpb/sources/socket_source.hpp"

#include <fcntl.h>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include "gpb/rt.hpp"

namespace gpb {

SocketSource::SocketSource(const Config& cfg) {
  path_ = cfg.get("source.socket.path", "/run/gpbridge.sock");
}

SocketSource::~SocketSource() { shutdown(); }

bool SocketSource::initialize(std::string& err) {
  ::unlink(path_.c_str());  // stale socket from a previous run

  listen_ = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (listen_ < 0) {
    err = std::string("socket(AF_UNIX, SOCK_SEQPACKET): ") + std::strerror(errno);
    return false;
  }
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path_.size() >= sizeof(addr.sun_path)) {
    err = "socket path too long: " + path_;
    return false;
  }
  std::strncpy(addr.sun_path, path_.c_str(), sizeof(addr.sun_path) - 1);

  if (::bind(listen_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    err = "bind " + path_ + ": " + std::strerror(errno);
    return false;
  }
  if (::listen(listen_, 1) != 0) {
    err = "listen " + path_ + ": " + std::strerror(errno);
    return false;
  }
  std::fprintf(stderr, "[socket] listening on %s (payload: %zu-byte GamepadState)\n",
               path_.c_str(), sizeof(GamepadState));
  return true;
}

bool SocketSource::read(GamepadState& out) {
  // One client at a time: this drives a physical console, and two writers racing for the
  // same gamepad is not a situation with a sensible resolution.
  if (client_ < 0) {
    const int c = ::accept4(listen_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (c < 0) return false;
    client_ = c;
    std::fprintf(stderr, "[socket] client connected\n");
    return false;
  }

  GamepadState in{};
  bool got = false;
  while (true) {
    const ssize_t n = ::recv(client_, &in, sizeof(in), 0);
    if (n == 0) {
      std::fprintf(stderr, "[socket] client disconnected\n");
      ::close(client_);
      client_ = -1;
      break;
    }
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      if (errno == EINTR) continue;
      std::fprintf(stderr, "[socket] recv: %s\n", std::strerror(errno));
      break;
    }
    if (static_cast<size_t>(n) != sizeof(GamepadState)) {
      std::fprintf(stderr, "[socket] bad frame: %zd bytes, expected %zu\n", n,
                   sizeof(GamepadState));
      continue;
    }
    if (in.magic != kWireMagic || in.version != kWireVersion) {
      std::fprintf(stderr, "[socket] wire mismatch (magic %08x version %u)\n", in.magic,
                   in.version);
      continue;
    }
    // Drain the whole backlog and keep only the newest: a control stream has no use for
    // stale intent.
    out = in;
    got = true;
  }

  if (!got) return false;
  out.seq = ++seq_;
  out.t_mono_ns = now_mono_ns();
  out.t_real_ns = now_real_ns();
  return true;
}

void SocketSource::shutdown() {
  if (client_ >= 0) { ::close(client_); client_ = -1; }
  if (listen_ >= 0) { ::close(listen_); listen_ = -1; ::unlink(path_.c_str()); }
}

}  // namespace gpb
