// gpb-latency -- measure the real input-to-output latency, end to end.
//
// The rig is a loopback: the OTG cable's data leg goes into one of the Pi's own USB-A ports
// instead of a console. dwc2 (device) and RP1 (host) are independent controllers, so the Pi
// enumerates its own gadget and can watch what it sends.
//
// What is measured is the whole chain a real input takes, minus the controller itself:
//
//     socket -> bridge -> encode -> /dev/hidg0 -> USB -> host -> evdev -> here
//
// Driving through the bridge rather than writing to /dev/hidg0 directly is deliberate. It
// costs a mode switch to set up, and it is the difference between measuring the USB path and
// measuring the thing the user actually asked about.
//
// The controller's own contribution is NOT included and cannot be: nothing here can press a
// physical button. Measuring that needs a GPIO bridged across a button's contacts.

#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kMagic = 0x52474231;
constexpr uint16_t kVersion = 1;
constexpr uint32_t kSouth = 1u << 0;

#pragma pack(push, 1)
struct WireState {
  uint32_t magic = kMagic;
  uint16_t version = kVersion;
  uint16_t size = 48;
  uint32_t seq = 0;
  uint32_t buttons = 0;
  uint64_t t_mono_ns = 0;
  uint64_t t_real_ns = 0;
  int16_t lx = 0, ly = 0, rx = 0, ry = 0;
  uint8_t lt = 0, rt = 0;
  uint8_t pad[6] = {0, 0, 0, 0, 0, 0};
};
#pragma pack(pop)
static_assert(sizeof(WireState) == 48, "must match GamepadState on the wire");

uint64_t now_ns() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + ts.tv_nsec;
}

void drain(int fd) {
  input_event e;
  while (::read(fd, &e, sizeof(e)) == sizeof(e)) {
  }
}

double pct(std::vector<double>& v, double p) {
  if (v.empty()) return 0.0;
  const size_t i = std::min(v.size() - 1,
                            static_cast<size_t>(p * (static_cast<double>(v.size()) - 1)));
  return v[i];
}

}  // namespace

int main(int argc, char** argv) {
  std::string sock_path = "/run/gpbridge.sock", dev_path;
  int samples = 60, settle_ms = 25, timeout_ms = 300;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--socket" && i + 1 < argc) sock_path = argv[++i];
    else if (a == "--device" && i + 1 < argc) dev_path = argv[++i];
    else if (a == "--samples" && i + 1 < argc) samples = std::atoi(argv[++i]);
    else if (a == "--settle-ms" && i + 1 < argc) settle_ms = std::atoi(argv[++i]);
  }
  if (dev_path.empty()) {
    std::printf("{\"ok\":false,\"error\":\"--device is required (the loopback gamepad node)\"}\n");
    return 2;
  }
  samples = std::max(1, std::min(samples, 500));

  const int dev = ::open(dev_path.c_str(), O_RDONLY | O_NONBLOCK);
  if (dev < 0) {
    std::printf("{\"ok\":false,\"error\":\"cannot open %s\"}\n", dev_path.c_str());
    return 1;
  }
  // Event timestamps are not used for the measurement, but a monotonic clock on this fd
  // keeps anything that does read them consistent with our own.
  int clk = CLOCK_MONOTONIC;
  ioctl(dev, EVIOCSCLOCKID, &clk);

  const int sock = ::socket(AF_UNIX, SOCK_SEQPACKET, 0);
  if (sock < 0) {
    std::printf("{\"ok\":false,\"error\":\"cannot create socket\"}\n");
    return 1;
  }
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);
  if (::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    std::printf("{\"ok\":false,\"error\":\"cannot connect to %s (is the bridge in socket "
                "mode?)\"}\n", sock_path.c_str());
    return 1;
  }
  usleep(300000);   // let the bridge register the client before the first sample

  std::vector<double> ms;
  ms.reserve(static_cast<size_t>(samples));
  int lost = 0;

  for (int i = 0; i < samples; ++i) {
    WireState s;
    s.seq = static_cast<uint32_t>(i + 1);
    // Alternate so every sample is a genuine change. Sending the same state twice would be
    // coalesced away and measure nothing.
    s.buttons = (i % 2 == 0) ? kSouth : 0;

    drain(dev);
    const uint64_t t0 = now_ns();
    if (::send(sock, &s, sizeof(s), 0) != static_cast<ssize_t>(sizeof(s))) {
      ++lost;
      continue;
    }

    // Wait for the host side to observe the button change we just injected.
    uint64_t t1 = 0;
    int waited = 0;
    while (waited < timeout_ms) {
      pollfd p{dev, POLLIN, 0};
      const int n = ::poll(&p, 1, timeout_ms - waited);
      if (n <= 0) break;
      const uint64_t seen = now_ns();
      input_event e;
      bool matched = false;
      while (::read(dev, &e, sizeof(e)) == static_cast<ssize_t>(sizeof(e))) {
        if (e.type == EV_KEY) matched = true;
      }
      if (matched) { t1 = seen; break; }
      waited = static_cast<int>((now_ns() - t0) / 1000000ull);
    }

    if (t1 == 0) ++lost;
    else ms.push_back(static_cast<double>(t1 - t0) / 1e6);
    usleep(static_cast<useconds_t>(settle_ms) * 1000);
  }

  ::close(sock);
  ::close(dev);

  if (ms.empty()) {
    std::printf("{\"ok\":false,\"error\":\"no samples completed; nothing observed the gadget\","
                "\"lost\":%d}\n", lost);
    return 1;
  }
  std::sort(ms.begin(), ms.end());
  double sum = 0;
  for (double v : ms) sum += v;

  std::printf("{\"ok\":true,\"n\":%zu,\"lost\":%d,\"min_ms\":%.3f,\"p50_ms\":%.3f,"
              "\"p95_ms\":%.3f,\"max_ms\":%.3f,\"mean_ms\":%.3f,\"samples\":[",
              ms.size(), lost, ms.front(), pct(ms, 0.50), pct(ms, 0.95), ms.back(),
              sum / static_cast<double>(ms.size()));
  for (size_t i = 0; i < ms.size(); ++i) std::printf("%s%.3f", i ? "," : "", ms[i]);
  std::printf("]}\n");
  return 0;
}
