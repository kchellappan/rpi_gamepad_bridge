// Minimal example: ask the bridge what it is driving, then drive it.
//
//   drive <host> [port]
#include <cstdio>
#include <string>
#include <thread>

#include "gpb_client/client.hpp"

int main(int argc, char** argv) {
  const std::string host = argc > 1 ? argv[1] : "127.0.0.1";
  const int port = argc > 2 ? std::atoi(argv[2]) : 9871;

  // Ask first. Whether this target's triggers are analog or buttons is not something a
  // client can know, and guessing wrong produces no error -- just nothing happening.
  const auto caps = gpb::client::query_capabilities(host, port);
  if (caps.ok) {
    std::printf("driving %s (%s triggers)\n", caps.target.c_str(), caps.trigger_mode.c_str());
  } else {
    std::printf("no answer from %s:%d; proceeding without capabilities\n", host.c_str(), port);
  }

  gpb::client::ControlClient::Options opts;
  opts.host = host;
  opts.port = port;
  gpb::client::ControlClient pad(opts);

  std::string err;
  if (!pad.connect(err)) {
    std::fprintf(stderr, "connect: %s\n", err.c_str());
    return 1;
  }

  pad.tap(gpb::btn::kEast);                       // press the target's east face button
  pad.set_axes(32767, 0, 0, 0);                   // left stick hard right
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  pad.set_axes(0, 0, 0, 0);

  // Send whichever trigger representation this target prefers. With the analog form the
  // bridge thresholds it anyway, so either works -- but asking costs nothing.
  if (caps.analog_triggers()) pad.set_triggers(255, 0);
  else pad.press(gpb::btn::kL2);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  std::printf("sent %llu datagrams\n", (unsigned long long)pad.sent());
  return 0;
}
