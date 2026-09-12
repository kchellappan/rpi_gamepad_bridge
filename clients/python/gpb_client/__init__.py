"""Client library for the Raspberry Pi gamepad bridge.

Two directions, one wire format:

    control   your machine -> the Pi    drives the console
    capture   the Pi -> your machine    every state as it happens, for aligning
                                        against video captured on this side

Standard library only, so submoduling this repo adds no dependencies to yours.
"""

from .state import GamepadState, Button
from .control import ControlClient
from .capture import CaptureReceiver
from .capabilities import Capabilities, query_capabilities

__all__ = ["GamepadState", "Button", "ControlClient", "CaptureReceiver",
           "Capabilities", "query_capabilities"]
__version__ = "0.1.0"
