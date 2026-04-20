"""
GBPy - Game Boy / Game Boy Color / Game Boy Advance Emulator

All emulation runs in C for performance. Python handles the GUI only.
"""

__version__ = "0.1.0"

from gbpy._core import (
    Emulator,
    BTN_A, BTN_B, BTN_SELECT, BTN_START,
    BTN_RIGHT, BTN_LEFT, BTN_UP, BTN_DOWN,
    BTN_L, BTN_R,
    AUDIO_SAMPLE_RATE, AUDIO_BUFFER_SIZE,
)

__all__ = [
    "Emulator",
    "BTN_A", "BTN_B", "BTN_SELECT", "BTN_START",
    "BTN_RIGHT", "BTN_LEFT", "BTN_UP", "BTN_DOWN",
    "BTN_L", "BTN_R",
    "AUDIO_SAMPLE_RATE", "AUDIO_BUFFER_SIZE",
]
