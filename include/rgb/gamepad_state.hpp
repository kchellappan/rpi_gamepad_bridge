#pragma once
// Normalized gamepad state: the keystone type of this project.
//
// Every InputSource produces one of these; every OutputSink consumes one. Neither side
// knows the other exists. This struct is ALSO the on-the-wire format for the socket
// source and the on-disk format for capture, so that a recorded session replays
// bit-exactly. Treat its layout as a published ABI: append fields, never reorder them,
// and bump kWireVersion when you do.

#include <cstdint>

namespace rgb {

// Button bits. Deliberately named by PHYSICAL POSITION, following evdev's BTN_SOUTH /
// BTN_EAST / BTN_NORTH / BTN_WEST convention, rather than by any vendor's face labels.
// A sink translates position -> label; that translation is the sink's business.
namespace btn {
inline constexpr uint32_t kSouth  = 1u << 0;   // Xbox A   / PS cross    / Switch B
inline constexpr uint32_t kEast   = 1u << 1;   // Xbox B   / PS circle   / Switch A
inline constexpr uint32_t kWest   = 1u << 2;   // Xbox X   / PS square   / Switch Y
inline constexpr uint32_t kNorth  = 1u << 3;   // Xbox Y   / PS triangle / Switch X
inline constexpr uint32_t kL1     = 1u << 4;
inline constexpr uint32_t kR1     = 1u << 5;
inline constexpr uint32_t kL2     = 1u << 6;   // digital shadow of the analog trigger
inline constexpr uint32_t kR2     = 1u << 7;
inline constexpr uint32_t kSelect = 1u << 8;   // Switch Minus
inline constexpr uint32_t kStart  = 1u << 9;   // Switch Plus
inline constexpr uint32_t kL3     = 1u << 10;
inline constexpr uint32_t kR3     = 1u << 11;
inline constexpr uint32_t kGuide  = 1u << 12;  // Switch Home
inline constexpr uint32_t kMisc1  = 1u << 13;  // share / capture / Assistant
inline constexpr uint32_t kDUp    = 1u << 14;
inline constexpr uint32_t kDDown  = 1u << 15;
inline constexpr uint32_t kDLeft  = 1u << 16;
inline constexpr uint32_t kDRight = 1u << 17;
inline constexpr uint32_t kCount  = 18;
}  // namespace btn

inline constexpr uint32_t kWireMagic   = 0x52474231;  // "RGB1"
inline constexpr uint16_t kWireVersion = 1;

// Axis sign convention: +X is right, +Y is DOWN.
//
// Down-positive matches both evdev (ABS_Y) and USB HID gamepad reports, so the value
// passes end to end without an inversion at either boundary. Every inversion is a place
// to get the sign wrong, so we have zero of them rather than two.
struct GamepadState {
  uint32_t magic   = kWireMagic;
  uint16_t version = kWireVersion;
  uint16_t size    = sizeof(GamepadState);

  uint32_t seq = 0;         // monotonically increasing, per-source
  uint32_t buttons = 0;     // rgb::btn bitfield

  // Kernel's timestamp for the originating event (CLOCK_MONOTONIC; see EVIOCSCLOCKID in
  // EvdevSource). This is as close to the hardware event as userspace can get.
  uint64_t t_mono_ns = 0;
  // Wall clock sampled when we processed it. Exists solely to align captures against
  // camera frames; never do interval math with it.
  uint64_t t_real_ns = 0;

  int16_t lx = 0, ly = 0;   // full int16 range, centered at 0
  int16_t rx = 0, ry = 0;
  uint8_t lt = 0, rt = 0;   // analog triggers, 0..255

  // Explicit, zero-initialized tail padding. The uint64 timestamps force 8-byte alignment,
  // so the compiler would otherwise insert 6 anonymous bytes here -- and anonymous padding
  // is uninitialized memory that this struct would write to disk and to a socket. Naming
  // it keeps captures deterministic and byte-comparable.
  uint8_t _pad[6] = {0, 0, 0, 0, 0, 0};

  constexpr bool pressed(uint32_t mask) const { return (buttons & mask) != 0; }
  constexpr void set(uint32_t mask, bool down) {
    buttons = down ? (buttons | mask) : (buttons & ~mask);
  }

  // Compares only the parts a sink would transmit -- ignores seq and timestamps, so that
  // an unchanged stick doesn't look like a new event every poll.
  bool same_input_as(const GamepadState& o) const {
    return buttons == o.buttons && lx == o.lx && ly == o.ly && rx == o.rx && ry == o.ry &&
           lt == o.lt && rt == o.rt;
  }
};

static_assert(sizeof(GamepadState) == 48, "wire format changed; bump kWireVersion");
static_assert(alignof(GamepadState) == 8, "unexpected alignment for a wire struct");

}  // namespace rgb
