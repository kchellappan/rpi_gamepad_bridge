#include "rgb/recorder.hpp"

#include <chrono>
#include <cstring>

namespace rgb {

Recorder::~Recorder() { stop(); }

bool Recorder::start(const std::string& path, size_t ring_slots, std::string& err) {
  // Round up to a power of two so the hot path indexes with a mask instead of a modulo.
  size_t n = 1;
  while (n < ring_slots) n <<= 1;
  ring_.assign(n, GamepadState{});
  mask_ = n - 1;

  file_ = std::fopen(path.c_str(), "wb");
  if (!file_) {
    err = "cannot open capture file: " + path;
    return false;
  }
  running_ = true;
  thread_ = std::thread(&Recorder::writer_loop, this);
  return true;
}

void Recorder::record(const GamepadState& s) {
  const uint64_t head = head_.load(std::memory_order_relaxed);
  const uint64_t tail = tail_.load(std::memory_order_acquire);
  if (head - tail >= ring_.size()) {
    // Full. Drop and count rather than block: a lost training sample is recoverable, a
    // control loop stalled on disk I/O during live play is not.
    dropped_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  ring_[head & mask_] = s;
  head_.store(head + 1, std::memory_order_release);
}

void Recorder::writer_loop() {
  while (true) {
    const uint64_t head = head_.load(std::memory_order_acquire);
    uint64_t tail = tail_.load(std::memory_order_relaxed);
    if (tail == head) {
      if (!running_.load(std::memory_order_acquire)) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      continue;
    }
    while (tail != head) {
      const GamepadState& s = ring_[tail & mask_];
      if (std::fwrite(&s, sizeof(s), 1, file_) == 1) {
        written_.fetch_add(1, std::memory_order_relaxed);
      }
      ++tail;
    }
    tail_.store(tail, std::memory_order_release);
  }
  std::fflush(file_);
}

void Recorder::stop() {
  if (!running_.exchange(false)) return;
  if (thread_.joinable()) thread_.join();
  if (file_) {
    std::fclose(file_);
    file_ = nullptr;
  }
}

}  // namespace rgb
