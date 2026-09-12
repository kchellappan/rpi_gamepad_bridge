#pragma once
// What a sink's target actually has, so a client does not have to guess.
//
// The problem this solves is concrete. The HORIPAD's ZL/ZR are BUTTONS, not analog
// triggers. A client that streams lt/rt because its own controller has analog triggers was
// producing nothing at all on that target, with no error and no clue as to why. A PC target
// would want the opposite. Neither the client nor the person writing it can be expected to
// know which, so the bridge says.
//
// Sinks accept whichever representation arrives where they reasonably can -- advertising is
// for letting a client send the *right* thing, not an excuse to reject the other one.

#include <string>
#include <vector>

namespace gpb {

struct Capabilities {
  std::string sink;            // "ns_hid"
  std::string target;          // "HORIPAD for Nintendo Switch"
  // "digital": the target's triggers are buttons; send l2/r2. Analog lt/rt is accepted and
  // thresholded, but a client with a real analog trigger loses nothing by sending both.
  // "analog": send lt/rt; the digital bits are derived.
  std::string trigger_mode;
  std::vector<std::string> axes;      // normalized axis names that mean something here
  std::vector<std::string> buttons;   // normalized button names that exist on the target
  std::string notes;

  std::string to_json() const;
};

}  // namespace gpb
