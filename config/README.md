# Controller notes

What each shipped config had to get right about its pad. Every binding in these files was
measured with the mapping wizard on the real controller, and each config passes a Switch 2's
own controller test on a Raspberry Pi 5. The general configuration reference is in the
[top-level README](../README.md#configuration).

The pads are worth comparing, because they disagree about things the evdev code names suggest
should be fixed:

| | [Stadia](stadia_to_switch.ini) | [DualSense](dualsense_to_switch.ini) | [Switch Pro](switch_pro_to_switch.ini) |
|---|---|---|---|
| Kernel driver | `hid-generic` | `hid-playstation` | `hid-nintendo` |
| Right stick | `ABS_Z` / `ABS_RZ` | `ABS_RX` / `ABS_RY` | `ABS_RX` / `ABS_RY` |
| D-pad | hat | hat | hat |
| ZL / ZR | analog, `ABS_BRAKE` / `ABS_GAS` | bound digitally, `BTN_TL2` / `BTN_TR2` | digital only, `BTN_TL2` / `BTN_TR2` |
| `BTN_NORTH` is physically | **west** | north | north |
| `BTN_WEST` is physically | **north** | west | west |
| Capture | `BTN_TRIGGER_HAPPY1` | none to spare | `BTN_Z` |
| Home | `BTN_MODE` | `BTN_MODE`, unbound | `BTN_MODE` |

The same two codes, `BTN_NORTH` and `BTN_WEST`, describe opposite positions on the Stadia and
the other two pads. Nothing in `input-event-codes.h` tells you which you have; that is what
the wizard exists to determine. The same goes for `ABS_Z`/`ABS_RZ`, which are the right stick
on the Stadia and the analog triggers on the DualSense.

## Google Stadia Controller

- **Its sticks report 1..255, and read 0 until the pad first reports.** `EVIOCGABS` returns
  an uninitialized value below the axis's own minimum, which is why ranges come from the
  kernel and the wizard refuses a resting value outside the range.
- **Its by-id path embeds a serial number,** so the config matches it with a glob.

## Sony DualSense (PS5)

- **Its triggers are analog but are bound as buttons,** because the Switch's ZL/ZR are
  digital. Binding `axis.ABS_Z = lt` / `axis.ABS_RZ = rt` instead also works and gives
  `trigger_deadzone` something to act on.
- **It has no control to spare for Capture.** Its Create button reports `BTN_SELECT`, which is
  already Minus. A pad running out of controls is not a mapping error.

## Nintendo Switch Pro Controller

- **It creates two input nodes.** The second is for the motion sensors. Its by-id link ends
  `-event-if00`, so the config's `-event-joystick` glob and the panel's device list both leave
  it out.
- **Its device name depends on the kernel:** "Nintendo Switch Pro Controller" on 6.6,
  "Nintendo Co., Ltd. Pro Controller" on 7.0. The by-id path is the same on both, so match on
  that.
- **Its button codes follow the printed letter,** which on a Nintendo layout matches position.
  So `face_by_position` must stay `true`; `false` would swap A with B and X with Y.
- **Its sticks rest a little off centre,** about 1,600 counts on one unit, inside the default
  2,000 deadzone. Raise `profile.right.deadzone` if a worn stick drifts.
