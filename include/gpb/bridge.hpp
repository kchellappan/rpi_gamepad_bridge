#pragma once
// Owns one InputSource and one OutputSink by composition and binds them together.
//
// The Bridge is a STATE HOLDER, not a pipe. Input arrives when it changes; a sink may
// need to emit on a fixed cadence regardless. Those are two independent triggers against
// one piece of shared state, which is why this cannot just be a read()/write() loop.

#include <chrono>
#include <memory>
#include <string>
#include <vector>
#include "gpb/input_source.hpp"
#include "gpb/output_sink.hpp"
#include "gpb/publisher.hpp"
#include "gpb/recorder.hpp"
#include "gpb/transform.hpp"

namespace gpb {

struct BridgeOptions {
  bool rumble = false;          // reverse channel, off by default
  bool record = false;
  std::string record_path;

  // Publish each state to another machine as it happens, for a consumer aligning it against
  // video it is capturing itself. Empty host disables it.
  std::string publish_host;
  int publish_port = 9872;
  std::string publish_key;
  size_t record_ring_slots = 8192;
  int rt_priority = 0;          // 0 disables SCHED_FIFO
  int cpu_affinity = -1;        // -1 disables pinning

  // Where to publish health for the control panel. Empty disables it.
  //
  // The bridge is the only component that knows whether its writes actually reach the host.
  // /sys/class/udc reports what the link claims, and it can claim "configured" while the
  // interrupt endpoint is disabled and every write fails -- which is exactly the state that
  // looked healthy in the UI while nothing worked at all.
  std::string status_path;
};

class Bridge {
 public:
  Bridge(std::unique_ptr<InputSource> src, std::unique_ptr<OutputSink> sink,
         std::vector<std::unique_ptr<Transform>> transforms, BridgeOptions opts);
  ~Bridge();

  bool initialize(std::string& err);
  int run();                    // blocks until stop() or fatal error
  void stop() { running_ = false; }

  struct Stats {
    uint64_t updates = 0;       // complete source updates consumed
    uint64_t submits = 0;       // states handed to the sink
    uint64_t coalesced = 0;     // updates that overwrote an undrained pending report
    uint64_t heartbeats = 0;
    uint64_t feedback = 0;
    uint64_t disconnects = 0;
    uint64_t reconnects = 0;
    uint64_t last_write_ok_ns = 0;   // CLOCK_MONOTONIC of the last report that reached the wire
    uint64_t write_failures = 0;     // consecutive failures; reset by any success
  };
  const Stats& stats() const { return stats_; }

 private:
  bool submit_current();
  void publish_status(bool force);

  std::unique_ptr<InputSource> src_;
  std::unique_ptr<OutputSink> sink_;
  std::vector<std::unique_ptr<Transform>> transforms_;
  BridgeOptions opts_;
  Recorder recorder_;
  Publisher publisher_;

  GamepadState current_{};
  GamepadState last_sent_{};
  Stats stats_{};
  volatile bool running_ = false;
  int epfd_ = -1;
  int timerfd_ = -1;
  uint64_t started_ns_ = 0;
  uint64_t status_written_ns_ = 0;
  std::string caps_json_ = "{}";
};

}  // namespace gpb
