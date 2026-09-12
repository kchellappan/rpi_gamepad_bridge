#include "gpb/bridge.hpp"

#include <cstring>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <sys/types.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <string>
#include "gpb/rt.hpp"

namespace gpb {

Bridge::Bridge(std::unique_ptr<InputSource> src, std::unique_ptr<OutputSink> sink,
               std::vector<std::unique_ptr<Transform>> transforms, BridgeOptions opts)
    : src_(std::move(src)),
      sink_(std::move(sink)),
      transforms_(std::move(transforms)),
      opts_(std::move(opts)) {}

Bridge::~Bridge() {
  if (!opts_.status_path.empty()) ::remove(opts_.status_path.c_str());
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
  src_->set_capabilities(sink_->capabilities().to_json());
  if (!src_->initialize(err)) return false;

  if (opts_.record && !recorder_.start(opts_.record_path, opts_.record_ring_slots, err))
    return false;
  if (!publisher_.start(opts_.publish_host, opts_.publish_port, opts_.publish_key, err))
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
  if (sent) {
    last_sent_ = current_;
    stats_.last_write_ok_ns = now_mono_ns();
    stats_.write_failures = 0;
  } else {
    ++stats_.write_failures;
  }
  return sent;
}

// Publish health for the control panel.
//
// Written atomically and cheaply: a few hundred bytes once a second, off the path that
// matters. The point is that "the host is attached" and "the host is accepting reports" are
// different claims, and only the second one means anything to a user. The link can sit at
// state=configured with its IN endpoint disabled, failing every write, which is precisely
// the failure this exists to surface.
void Bridge::publish_status(bool force) {
  if (opts_.status_path.empty()) return;
  const uint64_t now = now_mono_ns();
  if (!force && now - status_written_ns_ < 1000000000ull) return;
  status_written_ns_ = now;

  const uint64_t since_ok_ms =
      stats_.last_write_ok_ns ? (now - stats_.last_write_ok_ns) / 1000000ull : 0;

  char buf[1800];
  const int n = std::snprintf(
      buf, sizeof(buf),
      "{\"pid\":%d,\"uptime_ms\":%llu,\"source_connected\":%s,\"source\":\"%s\","
      "\"sink\":\"%s\",\"updates\":%llu,\"submits\":%llu,\"coalesced\":%llu,"
      "\"heartbeats\":%llu,\"disconnects\":%llu,\"reconnects\":%llu,"
      "\"write_failures\":%llu,\"ever_wrote\":%s,\"since_write_ok_ms\":%llu,"
      "\"capabilities\":%s,\"source_counters\":%s}\n",
      static_cast<int>(getpid()), (unsigned long long)((now - started_ns_) / 1000000ull),
      src_ && src_->connected() ? "true" : "false", src_ ? src_->name() : "",
      sink_ ? sink_->name() : "", (unsigned long long)stats_.updates,
      (unsigned long long)stats_.submits, (unsigned long long)stats_.coalesced,
      (unsigned long long)stats_.heartbeats, (unsigned long long)stats_.disconnects,
      (unsigned long long)stats_.reconnects, (unsigned long long)stats_.write_failures,
      stats_.last_write_ok_ns ? "true" : "false", (unsigned long long)since_ok_ms,
      caps_json_.c_str(), src_ ? src_->counters_json().c_str() : "{}");
  if (n <= 0) return;

  const std::string tmp = opts_.status_path + ".tmp";
  FILE* f = std::fopen(tmp.c_str(), "w");
  if (!f) return;
  std::fwrite(buf, 1, static_cast<size_t>(n), f);
  std::fclose(f);
  std::rename(tmp.c_str(), opts_.status_path.c_str());
}

int Bridge::run() {
  lock_memory();
  pin_to_cpu(opts_.cpu_affinity);
  set_realtime_priority(opts_.rt_priority);

  int registered_src_fd = -1;
  running_ = true;
  started_ns_ = now_mono_ns();
  if (sink_) caps_json_ = sink_->capabilities().to_json();
  publish_status(true);
  epoll_event events[8];
  // Seed from reality rather than assuming: the source may legitimately have started
  // without its device, in which case this is not a "disconnect" to announce.
  bool was_connected = src_->connected();
  uint64_t next_retry_ns = 0;
  const uint64_t kRetryIntervalNs = 500ull * 1000 * 1000;

  while (running_) {
    // Device loss. The Stadia controller re-enumerates unprompted, so this is a normal
    // operating condition rather than an error path, and must not spin or leak state.
    if (!src_->connected()) {
      if (was_connected) {
        was_connected = false;
        ++stats_.disconnects;
        // Fail safe. Whatever was held when the device vanished must be released, or the
        // console keeps seeing that button pressed forever -- a stuck input is far worse
        // than a dropped one.
        current_ = GamepadState{};
        submit_current();
        std::fprintf(stderr, "[bridge] source disconnected; output released to neutral\n");
      }
      if (registered_src_fd >= 0) {
        ::epoll_ctl(epfd_, EPOLL_CTL_DEL, registered_src_fd, nullptr);
        registered_src_fd = -1;
      }
      const uint64_t now = now_mono_ns();
      if (now >= next_retry_ns) {
        std::string rerr;
        if (src_->reconnect(rerr)) {
          ++stats_.reconnects;
          was_connected = true;
          std::fprintf(stderr, "[bridge] source reconnected\n");
        } else {
          next_retry_ns = now + kRetryIntervalNs;
        }
      }
    }

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

    // Poll faster while disconnected so a reconnect is picked up promptly, but stay lazy
    // in the normal case where every wakeup is driven by an actual event.
    const int timeout_ms = src_->connected() ? 1000 : 100;
    publish_status(false);
    const int n = ::epoll_wait(epfd_, events, 8, timeout_ms);
    if (n < 0) {
      if (errno == EINTR) continue;
      std::fprintf(stderr, "[bridge] epoll_wait: %s\n", std::strerror(errno));
      return 1;
    }

    for (int i = 0; i < n; ++i) {
      const int fd = events[i].data.fd;

      if (fd == registered_src_fd) {
        if (events[i].events & (EPOLLERR | EPOLLHUP)) {
          // Surfaces on the next loop as !connected(); read() confirms the errno.
          src_->read(current_);
          continue;
        }
        if (!src_->read(current_)) continue;   // partial batch; nothing coherent yet
        ++stats_.updates;

        if (!src_->emits_canonical())
          for (auto& t : transforms_) t->apply(current_);

        if (opts_.record) recorder_.record(current_);
        publisher_.publish(current_);
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
               "%llu heartbeats, %llu disconnects, %llu reconnects\n",
               (unsigned long long)stats_.updates, (unsigned long long)stats_.submits,
               (unsigned long long)stats_.coalesced,
               (unsigned long long)stats_.heartbeats,
               (unsigned long long)stats_.disconnects,
               (unsigned long long)stats_.reconnects);
  if (opts_.record)
    std::fprintf(stderr, "[bridge] capture: %llu written, %llu dropped\n",
                 (unsigned long long)recorder_.written(),
                 (unsigned long long)recorder_.dropped());
  return 0;
}

}  // namespace gpb
