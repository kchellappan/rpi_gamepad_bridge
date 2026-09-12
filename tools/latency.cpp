// gpb-latency -- measure the real input-to-output latency, end to end.
//
// The rig is a loopback: the OTG cable's data leg goes into one of the Pi's own USB-A ports
// instead of a console. dwc2 (device) and RP1 (host) are independent controllers, so the Pi
// enumerates its own gadget and can watch what it sends.
//
// What is measured:
//
//     /dev/hidg0 -> USB -> host -> evdev -> here
//
// An earlier version drove the bridge through its Unix socket instead, so the measurement
// would traverse the real serving path. That was the wrong trade. The bridge's contribution
// is a socket read, an encode and a write -- microseconds, against a total dominated by a
// 1 ms polling interval, and well below what this can resolve given that USB polling
// quantises every result anyway. It bought nothing measurable and cost a mode switch, a
// service restart, a restore-on-failure path and a socket-permissions problem.
//
// So: write the report directly, and stop the bridge for the duration since it holds the
// gadget open.
//
// Two things are NOT included. The bridge's own processing, as above. And the controller's
// latency, which cannot be measured this way at all -- nothing here can press a physical
// button. That needs a GPIO bridged across a button's contacts.

#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <poll.h>
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

// The HORIPAD report this gadget presents: 16 button bits, a hat, four axes, a vendor byte.
// Only one bit needs to change to make something observable on the host side, so the full
// encoder is not needed here -- byte 0 bit 1 is B.
#pragma pack(push, 1)
struct Report {
  uint8_t buttons_lo = 0;
  uint8_t buttons_hi = 0;
  uint8_t hat = 8;              // 8 = neutral
  uint8_t lx = 0x80, ly = 0x80, rx = 0x80, ry = 0x80;
  uint8_t vendor = 0;
};
#pragma pack(pop)
static_assert(sizeof(Report) == 8, "the gadget's report is 8 bytes");

constexpr uint8_t kB = 1 << 1;

// Set by SIGINT/SIGTERM so a run can be stopped from the panel without losing the samples
// already taken. sig_atomic_t because a handler may write it at any point.
volatile sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

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
  std::string hidg_path = "/dev/hidg0", dev_path;
  int settle_ms = 25, timeout_ms = 300;
  // A run continues until stopped or this elapses. Capped so a forgotten tab cannot hold
  // the bridge down indefinitely -- the bridge is stopped for the duration.
  int duration_ms = 60000;
  const int kMaxDurationMs = 60000;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--hidg" && i + 1 < argc) hidg_path = argv[++i];
    else if (a == "--device" && i + 1 < argc) dev_path = argv[++i];
    else if (a == "--duration-ms" && i + 1 < argc) duration_ms = std::atoi(argv[++i]);
    else if (a == "--settle-ms" && i + 1 < argc) settle_ms = std::atoi(argv[++i]);
  }
  duration_ms = std::max(1000, std::min(duration_ms, kMaxDurationMs));

  struct sigaction sa {};
  sa.sa_handler = on_signal;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
  if (dev_path.empty()) {
    std::printf("{\"ok\":false,\"error\":\"--device is required (the loopback gamepad node)\"}\n");
    return 2;
  }
  const int dev = ::open(dev_path.c_str(), O_RDONLY | O_NONBLOCK);
  if (dev < 0) {
    std::printf("{\"ok\":false,\"error\":\"cannot open %s\"}\n", dev_path.c_str());
    return 1;
  }
  int clk = CLOCK_MONOTONIC;
  ioctl(dev, EVIOCSCLOCKID, &clk);

  const int hidg = ::open(hidg_path.c_str(), O_RDWR);
  if (hidg < 0) {
    std::printf("{\"ok\":false,\"error\":\"cannot open %s (is the bridge stopped?)\"}\n",
                hidg_path.c_str());
    ::close(dev);
    return 1;
  }

  std::vector<double> ms;
  int lost = 0;
  const uint64_t deadline = now_ns() + static_cast<uint64_t>(duration_ms) * 1000000ull;

  for (int i = 0; !g_stop && now_ns() < deadline; ++i) {
    Report r;
    // Alternate, so every sample is a genuine change. Sending an identical report twice
    // would produce nothing for the host to notice.
    r.buttons_lo = (i % 2 == 0) ? kB : 0;

    drain(dev);
    const uint64_t t0 = now_ns();
    if (::write(hidg, &r, sizeof(r)) != static_cast<ssize_t>(sizeof(r))) {
      ++lost;
      usleep(static_cast<useconds_t>(settle_ms) * 1000);
      continue;
    }

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

    if (t1 == 0) {
      ++lost;
    } else {
      const double sample = static_cast<double>(t1 - t0) / 1e6;
      ms.push_back(sample);
      // One line per sample, flushed, so a reader can show progress while the run
      // continues. The summary comes last and is the only line starting with '{'.
      std::printf("%.3f\n", sample);
      std::fflush(stdout);
    }
    usleep(static_cast<useconds_t>(settle_ms) * 1000);
  }

  // Leave the pad released rather than however the last sample happened to end.
  Report neutral;
  (void)!::write(hidg, &neutral, sizeof(neutral));
  ::close(hidg);
  ::close(dev);

  if (ms.empty()) {
    std::printf("{\"ok\":false,\"error\":\"no samples completed; nothing observed the gadget\","
                "\"lost\":%d}\n", lost);
    return 1;
  }
  std::sort(ms.begin(), ms.end());
  double sum = 0;
  for (double v : ms) sum += v;

  std::printf("{\"ok\":true,\"stopped\":%s,\"n\":%zu,\"lost\":%d,\"min_ms\":%.3f,\"p50_ms\":%.3f,"
              "\"p95_ms\":%.3f,\"max_ms\":%.3f,\"mean_ms\":%.3f,\"samples\":[",
              g_stop ? "true" : "false", ms.size(), lost, ms.front(), pct(ms, 0.50),
              pct(ms, 0.95), ms.back(),
              sum / static_cast<double>(ms.size()));
  for (size_t i = 0; i < ms.size(); ++i) std::printf("%s%.3f", i ? "," : "", ms[i]);
  std::printf("]}\n");
  return 0;
}
