#include "gpb/bridge.hpp"

#include <cstring>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include "gpb/rt.hpp"

namespace gpb {

Bridge::Bridge(std::unique_ptr<InputSource> src, std::unique_ptr<OutputSink> sink,
               std::vector<std::unique_ptr<Transform>> transforms, BridgeOptions opts)
    : src_(std::move(src)),
      sink_(std::move(sink)),
      transforms_(std::move(transforms)),
      opts_(std::move(opts)) {}

Bridge::~Bridge() {
  if (timerfd_ >= 0) ::close(timerfd_);
  if (epfd_ >= 0) ::close(epfd_);
  if (src_) src_->shutdown();
  if (sink_) sink_->shutdown();
  recorder_.stop();
}

bool Bridge::initialize(std::string& err) {
  // Slow phase for both halves, before any hot-path work. This is the seam an
  // authenticating sink would use for its handshake.
  if (!sink_->initialize(err)) return false;
  if (!src_->initialize(err)) return false;

  if (opts_.record && !recorder_.start(opts_.record_path, opts_.record_ring_slots, err))
    return false;

  epfd_ = ::epoll_create1(EPOLL_CLOEXEC);
  if (epfd_ < 0) {
    err = std::string("epoll_create1: ") + std::strerror(errno);
    return false;
  }

  const auto cadence = sink_->cadence();
  if (cadence.count() > 0) {
    timerfd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timerfd_ < 0) {
      err = std::string("timerfd_create: ") + std::strerror(errno);
      return false;
    }
    itimerspec its{};
    its.it_interval.tv_sec = cadence.count() / 1000000000LL;
    its.it_interval.tv_nsec = cadence.count() % 1000000000LL;
    its.it_value = its.it_interval;
    ::timerfd_settime(timerfd_, 0, &its, nullptr);
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = timerfd_;
    ::epoll_ctl(epfd_, EPOLL_CTL_ADD, timerfd_, &ev);
  }

  // The sink's descriptor is registered with no events; we only arm EPOLLOUT while a
  // report is actually pending, otherwise a always-writable gadget spins the loop.
  if (sink_->writable_fd() >= 0) {
    epoll_event ev{};
    ev.events = 0;
    ev.data.fd = sink_->writable_fd();
    ::epoll_ctl(epfd_, EPOLL_CTL_ADD, sink_->writable_fd(), &ev);
  }

  current_.magic = kWireMagic;
  current_.version = kWireVersion;
  current_.size = sizeof(GamepadState);
  return true;
}

bool Bridge::submit_current() {
  const bool was_pending = sink_->has_pending();
  const bool sent = sink_->submit(current_);
  ++stats_.submits;
  if (!sent && was_pending) ++stats_.coalesced;

  // Arm or disarm EPOLLOUT to match the pending state.
  if (sink_->writable_fd() >= 0) {
    epoll_event ev{};
    ev.events = sink_->has_pending() ? static_cast<uint32_t>(EPOLLOUT) : 0u;
    ev.data.fd = sink_->writable_fd();
    ::epoll_ctl(epfd_, EPOLL_CTL_MOD, sink_->writable_fd(), &ev);
  }
  if (sent) last_sent_ = current_;
  return sent;
}

int Bridge::run() {
  lock_memory();
  pin_to_cpu(opts_.cpu_affinity);
  set_realtime_priority(opts_.rt_priority);

  int registered_src_fd = -1;
  running_ = true;
  epoll_event events[8];

  while (running_) {
    // A source's descriptor can change under us -- SocketSource switches from its
    // listening socket to the accepted client -- so reconcile before every wait.
    const int sfd = src_->fd();
    if (sfd != registered_src_fd) {
      if (registered_src_fd >= 0) ::epoll_ctl(epfd_, EPOLL_CTL_DEL, registered_src_fd, nullptr);
      if (sfd >= 0) {
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = sfd;
        if (::epoll_ctl(epfd_, EPOLL_CTL_ADD, sfd, &ev) != 0) {
          std::fprintf(stderr, "[bridge] epoll_ctl(src): %s\n", std::strerror(errno));
          return 1;
        }
      }
      registered_src_fd = sfd;
    }

    const int n = ::epoll_wait(epfd_, events, 8, 1000);
    if (n < 0) {
      if (errno == EINTR) continue;
      std::fprintf(stderr, "[bridge] epoll_wait: %s\n", std::strerror(errno));
      return 1;
    }

    for (int i = 0; i < n; ++i) {
      const int fd = events[i].data.fd;

      if (fd == registered_src_fd) {
        if (!src_->read(current_)) continue;   // partial batch; nothing coherent yet
        ++stats_.updates;

        if (!src_->emits_canonical())
          for (auto& t : transforms_) t->apply(current_);

        if (opts_.record) recorder_.record(current_);
        submit_current();

      } else if (fd == timerfd_) {
        uint64_t ticks = 0;
        if (::read(timerfd_, &ticks, sizeof(ticks)) != sizeof(ticks)) continue;
        // Heartbeat: re-send the last known state for hosts that expect traffic even
        // when nothing changed. Never invent state here, just repeat it.
        ++stats_.heartbeats;
        submit_current();

      } else if (fd == sink_->writable_fd()) {
        if (sink_->flush()) {
          epoll_event ev{};
          ev.events = 0;
          ev.data.fd = fd;
          ::epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev);
        }
      }
    }

    // Reverse channel, off unless explicitly enabled.
    if (opts_.rumble && sink_->feedback_fd() >= 0) {
      FeedbackEvent fb{};
      while (sink_->read_feedback(fb)) {
        src_->on_feedback(fb);
        ++stats_.feedback;
      }
    }
  }

  std::fprintf(stderr,
               "[bridge] stopped: %llu updates, %llu submits, %llu coalesced, "
               "%llu heartbeats\n",
               (unsigned long long)stats_.updates, (unsigned long long)stats_.submits,
               (unsigned long long)stats_.coalesced,
               (unsigned long long)stats_.heartbeats);
  if (opts_.record)
    std::fprintf(stderr, "[bridge] capture: %llu written, %llu dropped\n",
                 (unsigned long long)recorder_.written(),
                 (unsigned long long)recorder_.dropped());
  return 0;
}

}  // namespace gpb
