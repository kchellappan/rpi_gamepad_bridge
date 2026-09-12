// Prints HMAC tags for fixed inputs, so the suite can check them against Python's hmac.
//
// The SHA-256 and HMAC here are vendored to keep this project free of library dependencies.
// That is a reasonable trade only if the implementation is verified against a reference
// rather than trusted because it compiles -- a wrong hash still produces confident output.
#include <cstdio>
#include <cstring>
#include <string>

#include "gpb/gamepad_state.hpp"
#include "gpb/wire.hpp"

// Emit known GamepadStates as hex, so the suite can compare them byte for byte against the
// Python client's packing of the same logical states.
//
// Agreement on sizeof() is not agreement on layout. Two definitions can be the same size
// with fields transposed or padding misplaced, and every check that existed before this one
// would still pass while every value on the wire was wrong.
static void print_states() {
  struct Case {
    uint32_t seq, buttons;
    int16_t lx, ly, rx, ry;
    uint8_t lt, rt;
    uint64_t mono, real;
  } cases[] = {
      {0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
      // Every axis a different value, so a transposition cannot pass unnoticed.
      {1, 0, 1, 2, 3, 4, 5, 6, 7, 8},
      // Extremes, including negatives, which is where a sign or width error shows.
      {0xdeadbeef, 0x0003ffff, -32767, 32767, -1, 1, 255, 254,
       0x0123456789abcdefULL, 0xfedcba9876543210ULL},
      // Lowest and highest button bits only.
      {42, (1u << 0) | (1u << 17), 0, 0, 0, 0, 0, 0, 1, 2},
  };
  for (const auto& c : cases) {
    gpb::GamepadState s{};
    s.seq = c.seq;
    s.buttons = c.buttons;
    s.lx = c.lx; s.ly = c.ly; s.rx = c.rx; s.ry = c.ry;
    s.lt = c.lt; s.rt = c.rt;
    s.t_mono_ns = c.mono; s.t_real_ns = c.real;
    const auto* p = reinterpret_cast<const unsigned char*>(&s);
    for (size_t i = 0; i < sizeof(s); ++i) std::printf("%02x", p[i]);
    std::printf("\n");
  }
}

int main(int argc, char** argv) {
  if (argc > 1 && std::strcmp(argv[1], "states") == 0) {
    print_states();
    return 0;
  }
  struct Case {
    const char* key;
    const char* msg;
  } cases[] = {
      {"", ""},
      {"k", "hello"},
      {"a-longer-key-than-one-byte", "the quick brown fox"},
      // Longer than the 64-byte block, so the key-hashing path is covered too.
      {"0123456789012345678901234567890123456789012345678901234567890123456789", "x"},
      {"gpb", "\x01\x02\x03\x04"},
  };
  for (const auto& c : cases) {
    uint8_t tag[gpb::kTagBytes];
    gpb::hmac_tag(c.key, reinterpret_cast<const uint8_t*>(c.msg), std::strlen(c.msg), tag);
    for (size_t i = 0; i < gpb::kTagBytes; ++i) std::printf("%02x", tag[i]);
    std::printf("\n");
  }
  return 0;
}
