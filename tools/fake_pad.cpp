// gpb-fakepad -- a synthetic gamepad, created through uinput.
//
// Exists so the evdev path can be exercised without a human and without hardware: the
// wizard, the bridge, and the mapping config can all be driven end to end in CI or on a
// dev box. It was written after a blocking-read bug meant the wizard hung on its very
// first prompt, which no amount of reading the code had caught but a scripted pad would
// have found immediately.
//
//   gpb-fakepad --hold             create the device and idle (drive it yourself)
//   gpb-fakepad --wizard-script    emit the exact sequence gpb-discover wizard prompts for
//
// Needs write access to /dev/uinput, so in practice: sudo.

#include <dirent.h>
#include <fcntl.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <string.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr int kAbsCodes[] = {ABS_X, ABS_Y, ABS_Z, ABS_RZ, ABS_GAS, ABS_BRAKE};
constexpr int kHatCodes[] = {ABS_HAT0X, ABS_HAT0Y};
constexpr int kBtnCodes[] = {BTN_SOUTH, BTN_EAST,   BTN_NORTH,  BTN_WEST,
                             BTN_TL,    BTN_TR,     BTN_THUMBL, BTN_THUMBR,
                             BTN_SELECT, BTN_START, BTN_MODE,   BTN_TRIGGER_HAPPY1};

// Find the /dev/input/eventN node the kernel just gave us, by name. The caller needs it
// to point a reader at this pad specifically -- there may well be a real controller
// plugged in alongside.
std::string find_own_node() {
  DIR* d = opendir("/dev/input");
  if (!d) return "";
  std::string found;
  while (dirent* e = readdir(d)) {
    if (std::strncmp(e->d_name, "event", 5) != 0) continue;
    const std::string path = std::string("/dev/input/") + e->d_name;
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) continue;
    char buf[256] = {0};
    if (ioctl(fd, EVIOCGNAME(sizeof(buf) - 1), buf) >= 0 &&
        std::strcmp(buf, "gpb-fakepad") == 0) {
      found = path;
    }
    ::close(fd);
    if (!found.empty()) break;
  }
  closedir(d);
  return found;
}

void emit(int fd, uint16_t type, uint16_t code, int32_t value) {
  input_event e{};
  e.type = type;
  e.code = code;
  e.value = value;
  if (::write(fd, &e, sizeof(e)) != sizeof(e))
    std::fprintf(stderr, "uinput write failed: %s\n", std::strerror(errno));
}

void sync(int fd) { emit(fd, EV_SYN, SYN_REPORT, 0); }

int create_device() {
  int fd = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK);
  if (fd < 0) {
    std::fprintf(stderr, "open /dev/uinput: %s (try sudo)\n", std::strerror(errno));
    return -1;
  }
  ioctl(fd, UI_SET_EVBIT, EV_KEY);
  ioctl(fd, UI_SET_EVBIT, EV_ABS);
  ioctl(fd, UI_SET_EVBIT, EV_SYN);
  for (int c : kBtnCodes) ioctl(fd, UI_SET_KEYBIT, c);
  for (int c : kAbsCodes) ioctl(fd, UI_SET_ABSBIT, c);
  for (int c : kHatCodes) ioctl(fd, UI_SET_ABSBIT, c);

  uinput_user_dev dev{};
  std::snprintf(dev.name, UINPUT_MAX_NAME_SIZE, "gpb-fakepad");
  dev.id.bustype = BUS_USB;
  dev.id.vendor = 0x1209;   // pid.codes, the free VID for open hardware
  dev.id.product = 0x0001;
  dev.id.version = 1;
  // Mirror the Stadia controller's real ranges, including its 1..255 sticks, so anything
  // tested against this pad meets the same edge cases the real device presents.
  for (int c : kAbsCodes) {
    dev.absmin[c] = (c == ABS_GAS || c == ABS_BRAKE) ? 0 : 1;
    dev.absmax[c] = 255;
    dev.absflat[c] = 15;
  }
  for (int c : kHatCodes) {
    dev.absmin[c] = -1;
    dev.absmax[c] = 1;
  }
  if (::write(fd, &dev, sizeof(dev)) != sizeof(dev)) {
    std::fprintf(stderr, "uinput setup write: %s\n", std::strerror(errno));
    return -1;
  }
  if (ioctl(fd, UI_DEV_CREATE) < 0) {
    std::fprintf(stderr, "UI_DEV_CREATE: %s\n", std::strerror(errno));
    return -1;
  }
  return fd;
}

// How long a deflection is held before releasing. "Sloppy" holds long enough that the
// release lands inside the NEXT prompt's window, which is how a human actually moves a
// stick and is precisely the case that broke the wizard: one movement answering several
// prompts in a row.
int g_hold_us = 250000;
int g_gap_us = 1500000;

void axis_pulse(int fd, uint16_t code, int32_t to, int32_t rest) {
  emit(fd, EV_ABS, code, to);
  sync(fd);
  usleep(g_hold_us);
  emit(fd, EV_ABS, code, rest);
  sync(fd);
}

void button_pulse(int fd, uint16_t code) {
  emit(fd, EV_KEY, code, 1);
  sync(fd);
  usleep(150000);
  emit(fd, EV_KEY, code, 0);
  sync(fd);
}

int run_wizard_script(int fd, int lead_in_ms) {
  // Order must match gpb-discover wizard's prompts exactly.
  //
  // This script is open-loop: it cannot see the wizard, so the gap must exceed the
  // wizard's worst-case per-step overhead (waiting for the pad to return to rest). A human
  // waits for the prompt; a script does not, and if it runs ahead the wizard captures
  // whatever the script has moved on to -- a harness artifact that looks like a product
  // bug.
  const int gap_us = g_gap_us;
  // Give the caller time to locate the node and attach a reader before the first event.
  // Anything emitted before the wizard is listening is simply lost.
  std::printf("driving the wizard's prompt sequence in %dms...\n", lead_in_ms);
  std::fflush(stdout);
  usleep(static_cast<useconds_t>(lead_in_ms) * 1000);
  struct { const char* label; uint16_t code; int32_t to; int32_t rest; } axes[] = {
      {"lx -> right", ABS_X, 255, 128},
      {"ly -> down",  ABS_Y, 255, 128},
      {"rx -> right", ABS_Z, 255, 128},
      {"ry -> down",  ABS_RZ, 255, 128},
      {"lt",          ABS_BRAKE, 255, 0},
      {"rt",          ABS_GAS, 255, 0},
  };
  for (const auto& a : axes) {
    std::printf("  %s\n", a.label);
    std::fflush(stdout);
    axis_pulse(fd, a.code, a.to, a.rest);
    usleep(gap_us);
  }

  const uint16_t btns[] = {BTN_SOUTH, BTN_EAST, BTN_WEST, BTN_NORTH,
                           BTN_TL,    BTN_TR,   BTN_THUMBL, BTN_THUMBR,
                           BTN_SELECT, BTN_START, BTN_MODE, BTN_TRIGGER_HAPPY1};
  const char* names[] = {"south", "east", "west", "north", "l1", "r1",
                         "l3", "r3", "select", "start", "guide", "misc1"};
  for (size_t i = 0; i < sizeof(btns) / sizeof(btns[0]); ++i) {
    std::printf("  %s\n", names[i]);
    std::fflush(stdout);
    button_pulse(fd, btns[i]);
    usleep(gap_us);
  }

  struct { const char* label; uint16_t code; int32_t v; } hats[] = {
      {"dup", ABS_HAT0Y, -1}, {"ddown", ABS_HAT0Y, 1},
      {"dleft", ABS_HAT0X, -1}, {"dright", ABS_HAT0X, 1},
  };
  for (const auto& h : hats) {
    std::printf("  %s\n", h.label);
    std::fflush(stdout);
    axis_pulse(fd, h.code, h.v, 0);
    usleep(gap_us);
  }
  std::printf("script complete\n");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string mode = argc > 1 ? argv[1] : "";
  if (mode != "--hold" && mode != "--wizard-script" && mode != "--wizard-script-sloppy") {
    std::fprintf(stderr,
                 "usage: %s [--hold | --wizard-script [lead_in_ms] | "
                 "--wizard-script-sloppy [lead_in_ms]]\n",
                 argv[0]);
    return 2;
  }
  if (mode == "--wizard-script-sloppy") {
    g_hold_us = 1400000;   // release lands inside the NEXT prompt's window
    g_gap_us = 4500000;    // ...but still leave the wizard time to reach that prompt
  }
  const int lead_in_ms = argc > 2 ? std::atoi(argv[2]) : 6000;

  int fd = create_device();
  if (fd < 0) return 1;
  // udev needs a moment to publish the node before anyone can open it.
  usleep(400000);

  // Park every axis at its true resting position. uinput starts them at 0, but the sticks
  // rest at 128, so a reader sampling the neutral before any movement would record 0 and
  // then never see the pad return to "rest" again. A real controller reports its actual
  // center; the fixture has to as well, or it manufactures failures that the hardware
  // would never produce.
  for (int c : kAbsCodes) emit(fd, EV_ABS, c, (c == ABS_GAS || c == ABS_BRAKE) ? 0 : 128);
  for (int c : kHatCodes) emit(fd, EV_ABS, c, 0);
  sync(fd);
  usleep(200000);
  const std::string node = find_own_node();
  std::printf("created virtual pad \"gpb-fakepad\" at %s\n",
              node.empty() ? "(node not found)" : node.c_str());
  std::fflush(stdout);

  int rc = 0;
  if (mode != "--hold") rc = run_wizard_script(fd, lead_in_ms);
  else { std::printf("holding; Ctrl-C to remove\n"); std::fflush(stdout); pause(); }

  ioctl(fd, UI_DEV_DESTROY);
  ::close(fd);
  return rc;
}
