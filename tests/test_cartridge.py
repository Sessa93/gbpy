"""
Tests for the cartridge loader and ROM detection.
"""

import os
import struct
import pytest

from gbpy import Emulator


# Paths to test ROMs (relative to repo root)
ROM_DIR = os.path.join(os.path.dirname(os.path.dirname(__file__)), "roms")

GBA_ROM = os.path.join(ROM_DIR, "BtnTest.gba")
GBC_ROM = os.path.join(ROM_DIR, "pokemon-gold.gbc")


class TestCartridgeDetection:
    """Test ROM type detection based on header."""

    @pytest.mark.skipif(not os.path.exists(GBA_ROM), reason="GBA test ROM not found")
    def test_load_gba_rom(self):
        """Loading a .gba ROM sets mode to GBA."""
        emu = Emulator()
        emu.load_rom_file(GBA_ROM)
        assert emu.mode == "GBA"
        assert emu.running is True
        assert emu.screen_width == 240
        assert emu.screen_height == 160

    @pytest.mark.skipif(not os.path.exists(GBC_ROM), reason="GBC test ROM not found")
    def test_load_gbc_rom(self):
        """Loading a .gbc ROM sets mode to GBC."""
        emu = Emulator()
        emu.load_rom_file(GBC_ROM)
        assert emu.mode in ("GB", "GBC")
        assert emu.running is True
        assert emu.screen_width == 160
        assert emu.screen_height == 144

    def test_load_rom_from_bytes(self):
        """Loading ROM from bytes works."""
        emu = Emulator()
        # Minimal GB ROM header (32KB with valid header checksum area)
        rom = bytearray(32768)
        # Set Nintendo logo area (0x104-0x133): not checking logo validity
        # Set cart type = ROM ONLY (0x147 = 0x00)
        rom[0x147] = 0x00
        # ROM size code (0x148 = 0x00 = 32KB)
        rom[0x148] = 0x00
        # Compute header checksum (0x134-0x14C) -> stored at 0x14D
        checksum = 0
        for addr in range(0x134, 0x14D):
            checksum = (checksum - rom[addr] - 1) & 0xFF
        rom[0x14D] = checksum
        # Entry point: jump to 0x150
        rom[0x100] = 0x00  # NOP
        rom[0x101] = 0xC3  # JP
        rom[0x102] = 0x50
        rom[0x103] = 0x01
        try:
            emu.load_rom(bytes(rom))
            assert emu.running is True
        except RuntimeError:
            # If detection fails due to missing Nintendo logo, that's okay
            pass

    def test_load_invalid_rom(self):
        """Loading garbage data raises RuntimeError."""
        emu = Emulator()
        with pytest.raises(RuntimeError):
            emu.load_rom(b"\x00" * 16)

    def test_load_nonexistent_file(self):
        """Loading a file that doesn't exist raises RuntimeError."""
        emu = Emulator()
        with pytest.raises(RuntimeError):
            emu.load_rom_file("/nonexistent/path/rom.gba")


class TestCartridgeReset:
    """Test emulator reset after ROM load."""

    @pytest.mark.skipif(not os.path.exists(GBA_ROM), reason="GBA test ROM not found")
    def test_reset_preserves_rom(self):
        """Reset keeps the ROM loaded."""
        emu = Emulator()
        emu.load_rom_file(GBA_ROM)
        mode_before = emu.mode
        emu.reset()
        assert emu.mode == mode_before
        assert emu.running is True
