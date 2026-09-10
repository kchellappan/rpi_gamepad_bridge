// rgb-discover -- find a controller and learn its evdev mapping on the device itself.
//
// Exists because guessing at a controller's axis and button codes is exactly the kind of
// per-device fact that should be measured rather than assumed. Controllers disagree about
// which ABS_* code carries the right stick, whether the triggers are ABS_GAS/ABS_BRAKE or
// ABS_Z/ABS_RZ, and where "Assistant"/"Capture" style extra buttons land. Rather than ship
// a guess, ship the thing that produces the answer.
//
//   rgb-discover list             enumerate input devices
//   rgb-discover caps  <dev>      dump one device's axes and buttons
//   rgb-discover wizard <dev>     interactive: emits a pasteable config stanza

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <cstring>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "rgb/sources/evdev_source.hpp"

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
                rgb::evdev_abs_name(static_cast<uint16_t>(c)).c_str(), info.minimum,
                info.maximum, info.flat, info.value);
  }

  unsigned long keybits[KEY_MAX / (8 * sizeof(long)) + 1] = {0};
  ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits);
  std::printf("\nbuttons:\n");
  for (int c = 0; c <= KEY_MAX; ++c)
    if (bit_set(keybits, c))
      std::printf("  %s\n", rgb::evdev_key_name(static_cast<uint16_t>(c)).c_str());
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

void drain(int fd) {
  input_event e;
  while (::read(fd, &e, sizeof(e)) == sizeof(e)) {
  }
}

// Waits for whichever ABS code moves furthest from its resting value, so the user just
// pushes the stick and we work out both the code and the direction.
bool capture_axis(int fd, std::string& code_name, bool& invert) {
  std::map<uint16_t, int32_t> baseline, extreme;
  input_event e;
  const int kSettleEvents = 400;
  int seen = 0;

  while (seen < kSettleEvents) {
    const ssize_t n = ::read(fd, &e, sizeof(e));
    if (n != sizeof(e)) continue;
    if (e.type != EV_ABS) continue;
    if (!baseline.count(e.code)) {
      baseline[e.code] = e.value;
      extreme[e.code] = e.value;
    }
    if (std::abs(e.value - baseline[e.code]) > std::abs(extreme[e.code] - baseline[e.code]))
      extreme[e.code] = e.value;
    ++seen;

    // Stop as soon as one axis has clearly committed.
    for (const auto& [c, v] : extreme) {
      input_absinfo info{};
      ioctl(fd, EVIOCGABS(c), &info);
      const int32_t span = std::max(info.maximum - info.minimum, 1);
      if (std::abs(v - baseline[c]) > span / 3) {
        code_name = rgb::evdev_abs_name(c);
        invert = (v - baseline[c]) < 0;
        return true;
      }
    }
  }
  return false;
}

bool capture_button(int fd, std::string& code_name, bool& is_axis, bool& axis_invert) {
  input_event e;
  while (true) {
    const ssize_t n = ::read(fd, &e, sizeof(e));
    if (n != sizeof(e)) continue;
    if (e.type == EV_KEY && e.value == 1) {
      code_name = rgb::evdev_key_name(e.code);
      is_axis = false;
      return true;
    }
    // Many pads report the d-pad as a hat axis rather than four buttons.
    if (e.type == EV_ABS && (e.code == ABS_HAT0X || e.code == ABS_HAT0Y) && e.value != 0) {
      code_name = rgb::evdev_abs_name(e.code);
      is_axis = true;
      axis_invert = e.value < 0;
      return true;
    }
  }
}

int cmd_wizard(const char* path) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    std::fprintf(stderr, "open %s: %s\n", path, std::strerror(errno));
    return 1;
  }
  std::printf("device: %s\n", device_name(fd).c_str());
  std::printf(
      "\nFollow the prompts. Return the sticks to center between steps.\n"
      "Press Ctrl-C to abort.\n\n");

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
      {"misc1", "Press CAPTURE / SHARE / ASSISTANT (or any key to skip)"},
      {"dup", "Press D-PAD UP"},
      {"ddown", "Press D-PAD DOWN"},
      {"dleft", "Press D-PAD LEFT"},
      {"dright", "Press D-PAD RIGHT"},
  };

  std::vector<std::string> axis_lines, button_lines;

  for (const auto& p : axis_prompts) {
    std::printf("  [%-6s] %-44s ... ", p.target, p.instruction);
    std::fflush(stdout);
    drain(fd);
    std::string code;
    bool invert = false;
    if (capture_axis(fd, code, invert)) {
      // "Fully right" and "fully down" are both the POSITIVE direction in our convention
      // (+X right, +Y down), so a negative excursion means the device is inverted
      // relative to us.
      std::printf("%s%s\n", invert ? "-" : "", code.c_str());
      axis_lines.push_back("axis." + code + " = " + (invert ? "-" : "") + p.target);
    } else {
      std::printf("(no movement detected, skipped)\n");
    }
    usleep(300000);
  }

  std::printf("\n");
  for (const auto& p : button_prompts) {
    std::printf("  [%-6s] %-44s ... ", p.target, p.instruction);
    std::fflush(stdout);
    drain(fd);
    std::string code;
    bool is_axis = false, axis_invert = false;
    if (capture_button(fd, code, is_axis, axis_invert)) {
      std::printf("%s\n", code.c_str());
      if (is_axis) {
        const std::string t = (code == "ABS_HAT0X") ? "hatx" : "haty";
        const std::string line = "axis." + code + " = " + t;
        if (std::find(axis_lines.begin(), axis_lines.end(), line) == axis_lines.end())
          axis_lines.push_back(line);
      } else {
        button_lines.push_back("button." + code + " = " + p.target);
      }
    }
    usleep(300000);
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

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage:\n  %s list\n  %s caps <device>\n  %s wizard <device>\n",
                 argv[0], argv[0], argv[0]);
    return 2;
  }
  const std::string cmd = argv[1];
  if (cmd == "list") return cmd_list();
  if (cmd == "caps" && argc >= 3) return cmd_caps(argv[2]);
  if (cmd == "wizard" && argc >= 3) return cmd_wizard(argv[2]);
  std::fprintf(stderr, "unknown command\n");
  return 2;
}
