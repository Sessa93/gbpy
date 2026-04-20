"""
Tests for button input handling.
"""

import os
import pytest

from gbpy import (
    Emulator, BTN_A, BTN_B, BTN_SELECT, BTN_START,
    BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_L, BTN_R,
)

ROM_DIR = os.path.join(os.path.dirname(os.path.dirname(__file__)), "roms")
GBA_ROM = os.path.join(ROM_DIR, "BtnTest.gba")


class TestInput:
    """Test button input API."""

    def test_button_constants_exist(self):
        """Button constants are exported."""
        assert BTN_A == 0
        assert BTN_B == 1
        assert BTN_SELECT == 2
        assert BTN_START == 3
        assert BTN_RIGHT == 4
        assert BTN_LEFT == 5
        assert BTN_UP == 6
        assert BTN_DOWN == 7
        assert BTN_L == 8
        assert BTN_R == 9

    def test_button_press_no_rom(self):
        """Pressing buttons without ROM doesn't crash."""
        emu = Emulator()
        emu.button_press(BTN_A)
        emu.button_release(BTN_A)

    def test_invalid_button(self):
        """Invalid button values raise ValueError."""
        emu = Emulator()
        with pytest.raises(ValueError):
            emu.button_press(99)
        with pytest.raises(ValueError):
            emu.button_release(-1)

    @pytest.mark.skipif(not os.path.exists(GBA_ROM), reason="GBA test ROM not found")
    def test_all_buttons(self):
        """All buttons can be pressed and released after loading a ROM."""
        emu = Emulator()
        emu.load_rom_file(GBA_ROM)
        for btn in [BTN_A, BTN_B, BTN_SELECT, BTN_START,
                     BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT,
                     BTN_L, BTN_R]:
            emu.button_press(btn)
            emu.button_release(btn)
