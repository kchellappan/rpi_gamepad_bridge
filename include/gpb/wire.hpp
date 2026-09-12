#pragma once
// The datagram format shared by every transport and both client libraries.
//
// A datagram is the 48-byte GamepadState followed by an OPTIONAL 16-byte authentication
// tag. Appending rather than embedding is deliberate: the state struct stays byte-identical
// to what Recorder writes and SocketSource reads, so captures, replays and the Unix socket
// all remain compatible, and an unauthenticated deployment is simply the same datagram
// without the suffix.

#include <cstddef>
#include <cstdint>
#include <string>

#include "gpb/gamepad_state.hpp"

namespace gpb {

// A capability query. Sent instead of a state, distinguishable by length alone since it is
// nowhere near the 48 bytes a state occupies. The reply is JSON.
//
// Asking beats assuming: a client cannot otherwise know whether the target's triggers are
// analog or buttons, and guessing wrong fails silently.
inline constexpr char kQueryCaps[] = "GPBQCAPS";
inline constexpr size_t kQueryCapsLen = 8;

inline constexpr size_t kTagBytes = 16;   // truncated HMAC-SHA256
inline constexpr size_t kDatagramMax = sizeof(GamepadState) + kTagBytes;

// HMAC-SHA256 truncated to kTagBytes, over the raw state bytes.
//
// Implemented here rather than pulled from OpenSSL because this project has no library
// dependencies and adding one for 30 lines of hashing would be a poor trade on a board we
// may cross-compile for. The cost is a few microseconds per datagram at rates measured in
// hundreds per second.
void hmac_tag(const std::string& key, const uint8_t* data, size_t len, uint8_t out[kTagBytes]);

// Constant-time compare, so a timing oracle cannot be used to forge a tag byte at a time.
bool tag_equals(const uint8_t* a, const uint8_t* b, size_t len);

}  // namespace gpb
