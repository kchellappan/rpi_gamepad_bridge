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
#include "gpb/recorder.hpp"
#include "gpb/transform.hpp"

namespace gpb {

struct BridgeOptions {
  bool rumble = false;          // reverse channel, off by default
  bool record = false;
  std::string record_path;
  size_t record_ring_slots = 8192;
  int rt_priority = 0;          // 0 disables SCHED_FIFO
  int cpu_affinity = -1;        // -1 disables pinning
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
  };
  const Stats& stats() const { return stats_; }

 private:
  bool submit_current();

  std::unique_ptr<InputSource> src_;
  std::unique_ptr<OutputSink> sink_;
  std::vector<std::unique_ptr<Transform>> transforms_;
  BridgeOptions opts_;
  Recorder recorder_;

  GamepadState current_{};
  GamepadState last_sent_{};
  Stats stats_{};
  volatile bool running_ = false;
  int epfd_ = -1;
  int timerfd_ = -1;
};

}  // namespace gpb
