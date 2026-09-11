// gpb-discover -- find a controller and learn its evdev mapping on the device itself.
//
// Exists because guessing at a controller's axis and button codes is exactly the kind of
// per-device fact that should be measured rather than assumed. Controllers disagree about
// which ABS_* code carries the right stick, whether the triggers are ABS_GAS/ABS_BRAKE or
// ABS_Z/ABS_RZ, and where "Assistant"/"Capture" style extra buttons land. Rather than ship
// a guess, ship the thing that produces the answer.
//
//   gpb-discover list             enumerate input devices
//   gpb-discover caps  <dev>      dump one device's axes and buttons
//   gpb-discover wizard <dev>     interactive: emits a pasteable config stanza

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <cstring>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "gpb/sources/evdev_source.hpp"

namespace {

bool bit_set(const unsigned long* arr, int bit) {
  return (arr[bit / (8 * sizeof(long))] >> (bit % (8 * sizeof(long)))) & 1ul;
}

std::string device_name(int fd) {
  char buf[256] = {0};
  if (ioctl(fd, EVIOCGNAME(sizeof(buf) - 1), buf) < 0) return "(unknown)";
  return buf;
}

std::vector<std::string> list_event_devices() {
  std::vector<std::string> out;
  DIR* d = opendir("/dev/input");
  if (!d) return out;
  while (dirent* e = readdir(d)) {
    if (std::strncmp(e->d_name, "event", 5) == 0) out.push_back(std::string("/dev/input/") + e->d_name);
  }
  closedir(d);
  std::sort(out.begin(), out.end());
  return out;
}

int cmd_list() {
  for (const auto& path : list_event_devices()) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) continue;
    unsigned long keys[KEY_MAX / (8 * sizeof(long)) + 1] = {0};
    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys);
    // A gamepad is anything that claims a south face button; that filter is crude but it
    // reliably separates pads from keyboards, mice and the Pi's own power button.
    const bool gamepad = bit_set(keys, BTN_SOUTH) || bit_set(keys, BTN_TRIGGER);
    std::printf("%-22s %-44s%s\n", path.c_str(), device_name(fd).c_str(),
                gamepad ? "  <-- gamepad" : "");
    close(fd);
  }
  std::printf(
      "\nPrefer the stable path under /dev/input/by-id/ in your config: event numbers are\n"
      "assigned in probe order and will move when you replug.\n");
  return 0;
}

int cmd_caps(const char* path) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    std::fprintf(stderr, "open %s: %s\n", path, std::strerror(errno));
    return 1;
  }
  std::printf("device: %s\n\n", device_name(fd).c_str());

  unsigned long absbits[ABS_MAX / (8 * sizeof(long)) + 1] = {0};
  ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits);
  std::printf("axes:\n");
  for (int c = 0; c <= ABS_MAX; ++c) {
    if (!bit_set(absbits, c)) continue;
    input_absinfo info{};
    ioctl(fd, EVIOCGABS(c), &info);
    std::printf("  %-14s min=%-7d max=%-7d flat=%-5d value=%d\n",
                gpb::evdev_abs_name(static_cast<uint16_t>(c)).c_str(), info.minimum,
                info.maximum, info.flat, info.value);
  }

  unsigned long keybits[KEY_MAX / (8 * sizeof(long)) + 1] = {0};
  ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits);
  std::printf("\nbuttons:\n");
  for (int c = 0; c <= KEY_MAX; ++c)
    if (bit_set(keybits, c))
      std::printf("  %s\n", gpb::evdev_key_name(static_cast<uint16_t>(c)).c_str());
  close(fd);
  return 0;
}

// ---------------------------------------------------------------- wizard

struct AxisPrompt {
  const char* target;
  const char* instruction;
};
struct ButtonPrompt {
  const char* target;
  const char* instruction;
};

constexpr int kStepTimeoutMs = 8000;

struct AbsInfo {
  int32_t neutral = 0;   // resting value
  int32_t span = 1;
  int32_t lo = 0, hi = 0;
  // False when `neutral` is the midpoint fallback rather than a reading from the kernel.
  // Recorded for the startup diagnostic; rest checks trust it either way, because the
  // axes that need the fallback are exactly the ones whose midpoint is their centre.
  bool confirmed = false;
};

// Read whatever is already queued and throw it away. Requires a non-blocking fd: on a
// blocking one this loop never terminates, because read() waits for the next event rather
// than reporting that the queue is empty.
void drain(int fd) {
  input_event e;
  while (::read(fd, &e, sizeof(e)) == sizeof(e)) {
  }
}

bool wait_readable(int fd, int timeout_ms) {
  pollfd p{fd, POLLIN, 0};
  const int n = ::poll(&p, 1, timeout_ms);
  return n > 0 && (p.revents & POLLIN);
}

// Sample each axis's resting position -- and decide whether the sample can be believed.
//
// EVIOCGABS returns whatever the kernel last recorded for the axis, which for a device
// that has not reported since its node was opened is simply zero. On the Stadia controller
// that is exactly what happens: the sticks declare min=1, max=255 and report value=0. A
// value below the axis's own minimum cannot be a real reading, and that is the tell.
//
// Believing it is quietly destructive. The sticks actually rest near 128, so a neutral of
// 0 means they never come back "to rest", every subsequent prompt burns its full
// rest-wait timeout, and the user's input lands during the wait and is drained. Where the
// reading is impossible we fall back to the midpoint and mark the axis unconfirmed, so
// nothing blocks on a guess until we have seen the axis actually move.
std::map<uint16_t, AbsInfo> sample_neutral(int fd) {
  std::map<uint16_t, AbsInfo> out;
  unsigned long absbits[ABS_MAX / (8 * sizeof(long)) + 1] = {0};
  ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits);
  for (int c = 0; c <= ABS_MAX; ++c) {
    if (!bit_set(absbits, c)) continue;
    input_absinfo info{};
    if (ioctl(fd, EVIOCGABS(c), &info) != 0) continue;
    AbsInfo a;
    a.lo = info.minimum;
    a.hi = info.maximum;
    a.span = std::max(info.maximum - info.minimum, 1);
    if (info.value >= info.minimum && info.value <= info.maximum) {
      a.neutral = info.value;
      a.confirmed = true;
    } else {
      a.neutral = info.minimum + a.span / 2;
      a.confirmed = false;
    }
    out[static_cast<uint16_t>(c)] = a;
  }
  return out;
}


std::string rgb_abs_label(uint16_t code) { return gpb::evdev_abs_name(code); }

bool any_key_down(int fd) {
  unsigned long keys[KEY_MAX / (8 * sizeof(long)) + 1] = {0};
  if (ioctl(fd, EVIOCGKEY(sizeof(keys)), keys) < 0) return false;
  for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i)
    if (keys[i]) return true;
  return false;
}

// Wait until the pad is genuinely back at rest -- every axis near its neutral and no
// button held.
//
// The distinction between "at rest" and "quiet" is the bug this replaces. A stick held at
// full deflection generates no events at all, so an idle-detector sees silence and
// declares the pad settled. The next prompt then starts with the stick already deflected,
// and the release back to center is a full-span excursion that satisfies it instantly.
// One physical movement could answer three consecutive prompts.
// `observed` names axes we have actually received events from. An axis that has never
// reported reads 0 from EVIOCGABS regardless of where it physically sits, so demanding it
// be "at rest" would block forever on a value that means nothing. We can only hold an axis
// to its neutral once it has proven it reports at all.
bool wait_for_rest(int fd, const std::map<uint16_t, AbsInfo>& neutral,
                   const std::set<uint16_t>& observed, int timeout_ms, std::string& blocker) {
  int waited = 0;
  const int kSlice = 100;
  blocker.clear();
  while (waited < timeout_ms) {
    drain(fd);
    bool at_rest = true;
    blocker.clear();
    if (any_key_down(fd)) {
      at_rest = false;
      blocker = "a button is held";
    }
    if (at_rest) {
      for (const auto& [code, a] : neutral) {
        if (!a.confirmed && !observed.count(code)) continue;
        input_absinfo info{};
        if (ioctl(fd, EVIOCGABS(code), &info) != 0) continue;
        if (std::abs(info.value - a.neutral) > a.span / 8) {
          at_rest = false;
          blocker = rgb_abs_label(code) + "=" + std::to_string(info.value) + " vs " +
                    std::to_string(a.neutral);
          break;
        }
      }
    }
    if (at_rest) return true;
    usleep(kSlice * 1000);
    waited += kSlice;
  }
  return false;
}

// One capture implementation, used by both the terminal wizard and the JSON capture the
// web wizard drives. A second copy would drift from this one, and everything subtle lives
// here: resting baselines, the observed-axis rule, and excluding codes already bound.
//
// `accept_button` and `accept_axis` select what counts. A control like ZL may legitimately
// be satisfied by either -- a digital shoulder button or an analog trigger -- so the caller
// can accept both and record whichever actually arrived.
struct Capture {
  bool ok = false;
  bool is_axis = false;
  std::string code;
  bool invert = false;
};

Capture capture_control(int fd, const std::map<uint16_t, AbsInfo>& neutral,
                        std::set<uint16_t>& observed, const std::set<uint16_t>& exclude_axes,
                        const std::set<uint16_t>& exclude_keys, bool accept_button,
                        bool accept_axis, int timeout_ms) {
  Capture out;
  std::map<uint16_t, int32_t> extreme;
  for (const auto& [code, a] : neutral) extreme[code] = a.neutral;

  input_event e;
  int waited = 0;
  const int kSlice = 100;

  while (waited < timeout_ms) {
    if (!wait_readable(fd, kSlice)) {
      waited += kSlice;
      continue;
    }
    while (::read(fd, &e, sizeof(e)) == static_cast<ssize_t>(sizeof(e))) {
      if (accept_button && e.type == EV_KEY && e.value == 1 && !exclude_keys.count(e.code)) {
        out.ok = true;
        out.is_axis = false;
        out.code = gpb::evdev_key_name(e.code);
        return out;
      }
      // A hat is reported as an axis but behaves like a button, so it satisfies either.
      if (e.type == EV_ABS && (e.code == ABS_HAT0X || e.code == ABS_HAT0Y) && e.value != 0) {
        if (!accept_button && !accept_axis) continue;
        out.ok = true;
        out.is_axis = true;
        out.code = gpb::evdev_abs_name(e.code);
        out.invert = e.value < 0;
        return out;
      }
      if (!accept_axis || e.type != EV_ABS) continue;

      auto it = neutral.find(e.code);
      if (it == neutral.end()) continue;
      observed.insert(e.code);   // it reports; from now on we can hold it to its neutral
      if (exclude_axes.count(e.code)) continue;

      const int32_t base = it->second.neutral;
      if (std::abs(e.value - base) > std::abs(extreme[e.code] - base)) extreme[e.code] = e.value;
      const int32_t delta = extreme[e.code] - base;
      if (std::abs(delta) > it->second.span / 3) {
        out.ok = true;
        out.is_axis = true;
        out.code = gpb::evdev_abs_name(e.code);
        out.invert = delta < 0;
        return out;
      }
    }
  }
  return out;
}

bool capture_axis(int fd, const std::map<uint16_t, AbsInfo>& neutral,
                  const std::set<uint16_t>& exclude, std::set<uint16_t>& observed,
                  std::string& code_name, bool& invert, int timeout_ms) {
  Capture c = capture_control(fd, neutral, observed, exclude, {}, false, true, timeout_ms);
  code_name = c.code;
  invert = c.invert;
  return c.ok;
}

bool capture_button(int fd, const std::set<uint16_t>& exclude_keys, std::string& code_name,
                    bool& is_axis, bool& axis_invert, int timeout_ms) {
  static const std::map<uint16_t, AbsInfo> kNoAxes;
  std::set<uint16_t> ignored;
  Capture c = capture_control(fd, kNoAxes, ignored, {}, exclude_keys, true, false, timeout_ms);
  code_name = c.code;
  is_axis = c.is_axis;
  axis_invert = c.invert;
  return c.ok;
}

int cmd_wizard(const char* path) {
  // Non-blocking is mandatory here, not a preference: drain() and the capture loops detect
  // "nothing more to read" by read() failing, which on a blocking fd simply never happens.
  int fd = open(path, O_RDONLY | O_NONBLOCK);
  if (fd < 0) {
    std::fprintf(stderr, "open %s: %s\n", path, std::strerror(errno));
    return 1;
  }
  std::printf("device: %s\n", device_name(fd).c_str());

  std::printf("\nLet go of the controller -- sampling its resting position ... ");
  std::fflush(stdout);
  usleep(1200000);
  drain(fd);
  auto neutral = sample_neutral(fd);
  size_t guessed = 0;
  for (const auto& [_, a] : neutral)
    if (!a.confirmed) ++guessed;
  std::printf("%zu axes", neutral.size());
  if (guessed) std::printf(" (%zu report no usable resting value; midpoint assumed)", guessed);
  std::printf("\n");

  std::printf(
      "\nFollow the prompts, releasing fully between steps. Each step times out\n"
      "after %d seconds and is skipped, so you can pass on any control your pad\n"
      "does not have. Ctrl-C aborts.\n\n",
      kStepTimeoutMs / 1000);

  const AxisPrompt axis_prompts[] = {
      {"lx", "Push the LEFT stick fully RIGHT"},
      {"ly", "Push the LEFT stick fully DOWN"},
      {"rx", "Push the RIGHT stick fully RIGHT"},
      {"ry", "Push the RIGHT stick fully DOWN"},
      {"lt", "Squeeze the LEFT trigger fully"},
      {"rt", "Squeeze the RIGHT trigger fully"},
  };
  const ButtonPrompt button_prompts[] = {
      {"south", "Press the BOTTOM face button"},
      {"east", "Press the RIGHT face button"},
      {"west", "Press the LEFT face button"},
      {"north", "Press the TOP face button"},
      {"l1", "Press the LEFT shoulder (bumper)"},
      {"r1", "Press the RIGHT shoulder (bumper)"},
      {"l3", "Click the LEFT stick"},
      {"r3", "Click the RIGHT stick"},
      {"select", "Press SELECT / BACK / the left-hand small button"},
      {"start", "Press START / MENU / the right-hand small button"},
      {"guide", "Press the GUIDE / HOME button"},
      {"misc1", "Press CAPTURE / SHARE / ASSISTANT (or wait to skip)"},
      {"dup", "Press D-PAD UP"},
      {"ddown", "Press D-PAD DOWN"},
      {"dleft", "Press D-PAD LEFT"},
      {"dright", "Press D-PAD RIGHT"},
  };

  std::vector<std::string> axis_lines, button_lines;
  std::set<uint16_t> used_axes, used_keys, observed_axes;

  for (const auto& p : axis_prompts) {
    std::printf("  [%-6s] %-44s ... ", p.target, p.instruction);
    std::fflush(stdout);
    std::string blocker;
    if (!wait_for_rest(fd, neutral, observed_axes, 5000, blocker))
      std::printf("(not at rest: %s) ", blocker.empty() ? "unknown" : blocker.c_str());
    drain(fd);

    std::string code;
    bool invert = false;
    if (capture_axis(fd, neutral, used_axes, observed_axes, code, invert, kStepTimeoutMs)) {
      // "Fully right" and "fully down" are both the POSITIVE direction in our convention
      // (+X right, +Y down), so a negative excursion means the device disagrees with us.
      std::printf("%s%s\n", invert ? "-" : "", code.c_str());
      axis_lines.push_back("axis." + code + " = " + (invert ? "-" : "") + p.target);
      bool ok = false;
      used_axes.insert(gpb::evdev_code_from_name(code, ok));
    } else {
      std::printf("(timed out, skipped)\n");
    }
  }

  std::printf("\n");
  for (const auto& p : button_prompts) {
    std::printf("  [%-6s] %-44s ... ", p.target, p.instruction);
    std::fflush(stdout);
    std::string blocker;
    wait_for_rest(fd, neutral, observed_axes, 5000, blocker);
    drain(fd);

    std::string code;
    bool is_axis = false, axis_invert = false;
    if (capture_button(fd, used_keys, code, is_axis, axis_invert, kStepTimeoutMs)) {
      std::printf("%s\n", code.c_str());
      if (is_axis) {
        const std::string t = (code == "ABS_HAT0X") ? "hatx" : "haty";
        const std::string line = "axis." + code + " = " + t;
        if (std::find(axis_lines.begin(), axis_lines.end(), line) == axis_lines.end())
          axis_lines.push_back(line);
      } else {
        button_lines.push_back("button." + code + " = " + p.target);
        bool ok = false;
        used_keys.insert(gpb::evdev_code_from_name(code, ok));
      }
    } else {
      std::printf("(timed out, skipped)\n");
    }
  }
  close(fd);

  std::printf("\n\n--- paste into your config under [source.evdev] ---\n\n");
  std::printf("[source.evdev]\ndevice = %s\ngrab = true\n\n", path);
  for (const auto& l : axis_lines) std::printf("%s\n", l.c_str());
  std::printf("\n");
  for (const auto& l : button_lines) std::printf("%s\n", l.c_str());
  std::printf("\n--- end ---\n");
  return 0;
}

// ---------------------------------------------------------------- capture (machine readable)

// Emits one JSON object describing a single captured control. Used by the web wizard, which
// drives the capture step by step from the browser and needs a parseable answer rather than
// a human-readable prompt.
int cmd_capture(const char* path, const std::string& kind, int timeout_ms,
                const std::string& exclude_csv) {
  const bool accept_button = (kind == "button" || kind == "any");
  const bool accept_axis = (kind == "axis" || kind == "any");
  if (!accept_button && !accept_axis) {
    std::printf("{\"ok\":false,\"error\":\"kind must be button, axis or any\"}\n");
    return 2;
  }

  int fd = open(path, O_RDONLY | O_NONBLOCK);
  if (fd < 0) {
    std::printf("{\"ok\":false,\"error\":\"cannot open device\"}\n");
    return 1;
  }

  std::set<uint16_t> exclude_axes, exclude_keys;
  {
    std::string item;
    std::string csv = exclude_csv;
    size_t pos;
    while (!csv.empty()) {
      pos = csv.find(',');
      item = csv.substr(0, pos);
      if (!item.empty()) {
        bool ok = false;
        const uint16_t code = gpb::evdev_code_from_name(item, ok);
        if (ok) {
          // ABS_* names resolve into the axis set, everything else into the key set.
          if (item.rfind("ABS_", 0) == 0) exclude_axes.insert(code);
          else exclude_keys.insert(code);
        }
      }
      if (pos == std::string::npos) break;
      csv = csv.substr(pos + 1);
    }
  }

  const auto neutral = sample_neutral(fd);
  std::set<uint16_t> observed;
  for (const auto& [code, a] : neutral)
    if (a.confirmed) observed.insert(code);

  // Settle first, so a control still held from the previous step cannot answer this one.
  std::string blocker;
  wait_for_rest(fd, neutral, observed, 4000, blocker);
  drain(fd);

  Capture c = capture_control(fd, neutral, observed, exclude_axes, exclude_keys,
                              accept_button, accept_axis, timeout_ms);
  close(fd);

  if (!c.ok) {
    std::printf("{\"ok\":false,\"error\":\"timeout\"}\n");
    return 0;   // a timeout is a normal outcome the caller handles, not a failure to run
  }
  std::printf("{\"ok\":true,\"kind\":\"%s\",\"code\":\"%s\",\"invert\":%s}\n",
              c.is_axis ? "axis" : "button", c.code.c_str(), c.invert ? "true" : "false");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage:\n  %s list\n  %s caps <device>\n  %s wizard <device>\n"
                 "  %s capture <device> [--kind button|axis|any] [--timeout-ms N] "
                 "[--exclude CODE,CODE]\n",
                 argv[0], argv[0], argv[0], argv[0]);
    return 2;
  }
  const std::string cmd = argv[1];
  if (cmd == "list") return cmd_list();
  if (cmd == "caps" && argc >= 3) return cmd_caps(argv[2]);
  if (cmd == "wizard" && argc >= 3) return cmd_wizard(argv[2]);
  if (cmd == "capture" && argc >= 3) {
    std::string kind = "any", exclude;
    int timeout_ms = 8000;
    for (int i = 3; i < argc; ++i) {
      const std::string a = argv[i];
      if (a == "--kind" && i + 1 < argc) kind = argv[++i];
      else if (a == "--timeout-ms" && i + 1 < argc) timeout_ms = std::atoi(argv[++i]);
      else if (a == "--exclude" && i + 1 < argc) exclude = argv[++i];
    }
    return cmd_capture(argv[2], kind, timeout_ms, exclude);
  }
  std::fprintf(stderr, "unknown command\n");
  return 2;
}
