// Prints HMAC tags for fixed inputs, so the suite can check them against Python's hmac.
//
// The SHA-256 and HMAC here are vendored to keep this project free of library dependencies.
// That is a reasonable trade only if the implementation is verified against a reference
// rather than trusted because it compiles -- a wrong hash still produces confident output.
#include <cstdio>
#include <cstring>
#include <string>

#include "gpb/wire.hpp"

int main() {
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
