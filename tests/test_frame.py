"""
Tests for frame execution and framebuffer/audio output.
"""

import os
import pytest

from gbpy import Emulator, BTN_A

ROM_DIR = os.path.join(os.path.dirname(os.path.dirname(__file__)), "roms")
GBA_ROM = os.path.join(ROM_DIR, "BtnTest.gba")
GBC_ROM = os.path.join(ROM_DIR, "pokemon-gold.gbc")


@pytest.mark.skipif(not os.path.exists(GBA_ROM), reason="GBA test ROM not found")
class TestFrameGBA:
    """Test frame execution for GBA ROMs."""

    def test_run_frame(self):
        emu = Emulator()
        emu.load_rom_file(GBA_ROM)
        emu.run_frame()
        assert emu.running is True

    def test_framebuffer_size(self):
        emu = Emulator()
        emu.load_rom_file(GBA_ROM)
        emu.run_frame()
        fb = emu.get_framebuffer()
        assert fb is not None
        assert len(fb) == 240 * 160 * 4

    def test_framebuffer_buffer_protocol(self):
        emu = Emulator()
        emu.load_rom_file(GBA_ROM)
        emu.run_frame()
        view = memoryview(emu)
        assert len(view) == 240 * 160 * 4

    def test_step(self):
        emu = Emulator()
        emu.load_rom_file(GBA_ROM)
        emu.step(100)
        assert emu.running is True

    def test_multiple_frames(self):
        emu = Emulator()
        emu.load_rom_file(GBA_ROM)
        for _ in range(10):
            emu.run_frame()
        assert emu.running is True


@pytest.mark.skipif(not os.path.exists(GBC_ROM), reason="GBC test ROM not found")
class TestFrameGBC:
    """Test frame execution for GBC ROMs."""

    def test_run_frame(self):
        emu = Emulator()
        emu.load_rom_file(GBC_ROM)
        emu.run_frame()
        assert emu.running is True

    def test_framebuffer_size(self):
        emu = Emulator()
        emu.load_rom_file(GBC_ROM)
        emu.run_frame()
        fb = emu.get_framebuffer()
        assert fb is not None
        assert len(fb) == 160 * 144 * 4


class TestAudioConstants:
    """Test audio constant exports."""

    def test_sample_rate(self):
        from gbpy import AUDIO_SAMPLE_RATE
        assert AUDIO_SAMPLE_RATE == 44100

    def test_buffer_size(self):
        from gbpy import AUDIO_BUFFER_SIZE
        assert AUDIO_BUFFER_SIZE == 2048
