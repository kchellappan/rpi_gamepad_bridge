#pragma once
// C++ client for the Raspberry Pi gamepad bridge.
//
// Two directions, one wire format:
//
//     ControlClient     your machine -> the Pi   drives the console
//     CaptureReceiver   the Pi -> your machine   every state as it happens
//
// Header-plus-source, no dependencies, so submoduling this repo adds nothing to your build
// but two files.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

#include "gpb/gamepad_state.hpp"

namespace gpb::client {

// What the bridge says its target actually has. Ask rather than assume: whether a target's
// triggers are analog or buttons is not something a client can know, and guessing wrong
// fails silently.
struct Capabilities {
  bool ok = false;
  std::string sink, target, trigger_mode, notes, raw_json;
  bool analog_triggers() const { return trigger_mode == "analog"; }
};

// Query a bridge over UDP. Blocks for up to timeout_ms.
Capabilities query_capabilities(const std::string& host, int port, int timeout_ms = 1000);

class ControlClient {
 public:
  struct Options {
    std::string host;            // UDP destination; leave empty to use unix_path
    int port = 9871;
    std::string unix_path;       // when running on the Pi itself
    std::string key;             // HMAC key; empty disables authentication
    double rate_hz = 125.0;
    bool stream = true;          // see the note on streaming below
  };

  explicit ControlClient(Options opts);
  ~ControlClient();

  ControlClient(const ControlClient&) = delete;
  ControlClient& operator=(const ControlClient&) = delete;

  bool connect(std::string& err);

  // Send the current state once.
  bool send();

  // Mutate then send. The state is also directly accessible via state().
  void press(uint32_t button_mask);
  void release(uint32_t button_mask);
  void tap(uint32_t button_mask, int hold_ms = 50);
  void set_axes(int16_t lx, int16_t ly, int16_t rx, int16_t ry);
  void set_triggers(uint8_t lt, uint8_t rt);

  GamepadState& state() { return state_; }
  uint64_t sent() const { return sent_; }

  void close();

 private:
  void stream_loop();

  Options opts_;
  int fd_ = -1;
  bool is_unix_ = false;
  unsigned char dest_[128] = {};
  unsigned dest_len_ = 0;

  GamepadState state_{};
  std::atomic<uint32_t> seq_{0};
  std::atomic<uint64_t> sent_{0};
  std::atomic<bool> running_{false};
  std::thread thread_;
};

class CaptureReceiver {
 public:
  CaptureReceiver(int port, std::string bind_addr = "0.0.0.0", std::string key = "");
  ~CaptureReceiver();

  bool open(std::string& err);
  // Blocks up to timeout_ms. Returns false on timeout or a rejected datagram; check
  // rejected() to distinguish.
  bool recv(GamepadState& out, int timeout_ms = 1000);
  uint64_t rejected() const { return rejected_; }
  void close();

 private:
  int port_;
  std::string bind_addr_, key_;
  int fd_ = -1;
  uint64_t rejected_ = 0;
};

}  // namespace gpb::client
