"""
Tests for save state round-trip serialization.
"""

import os
import tempfile
import pytest

from gbpy import Emulator

ROM_DIR = os.path.join(os.path.dirname(os.path.dirname(__file__)), "roms")
GBA_ROM = os.path.join(ROM_DIR, "BtnTest.gba")
GBC_ROM = os.path.join(ROM_DIR, "pokemon-gold.gbc")


@pytest.mark.skipif(not os.path.exists(GBA_ROM), reason="GBA test ROM not found")
class TestSaveStateGBA:
    """Test save/load state round-trip for GBA ROMs."""

    def test_save_state_creates_file(self):
        emu = Emulator()
        emu.load_rom_file(GBA_ROM)
        emu.run_frame()

        with tempfile.NamedTemporaryFile(suffix=".state", delete=False) as f:
            path = f.name
        try:
            emu.save_state(path)
            assert os.path.exists(path)
            assert os.path.getsize(path) > 0
        finally:
            os.unlink(path)

    def test_load_state_restores(self):
        emu = Emulator()
        emu.load_rom_file(GBA_ROM)

        # Run a few frames
        for _ in range(5):
            emu.run_frame()

        with tempfile.NamedTemporaryFile(suffix=".state", delete=False) as f:
            path = f.name
        try:
            emu.save_state(path)

            # Run more frames (state diverges)
            for _ in range(10):
                emu.run_frame()

            # Restore
            emu.load_state(path)
            assert emu.running is True
            assert emu.mode == "GBA"
        finally:
            os.unlink(path)

    def test_load_nonexistent_state(self):
        emu = Emulator()
        emu.load_rom_file(GBA_ROM)
        with pytest.raises(RuntimeError):
            emu.load_state("/nonexistent/state.file")

    def test_save_without_rom(self):
        emu = Emulator()
        with pytest.raises(RuntimeError):
            emu.save_state("/tmp/test.state")


@pytest.mark.skipif(not os.path.exists(GBC_ROM), reason="GBC test ROM not found")
class TestSaveStateGBC:
    """Test save/load state round-trip for GBC ROMs."""

    def test_round_trip(self):
        emu = Emulator()
        emu.load_rom_file(GBC_ROM)
        emu.run_frame()

        with tempfile.NamedTemporaryFile(suffix=".state", delete=False) as f:
            path = f.name
        try:
            emu.save_state(path)
            emu.load_state(path)
            assert emu.running is True
            assert emu.mode in ("GB", "GBC")
        finally:
            os.unlink(path)
