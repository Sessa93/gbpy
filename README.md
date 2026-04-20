# GBPy

A Game Boy, Game Boy Color, and Game Boy Advance emulator. All CPU emulation, video rendering, and audio synthesis run in C for performance; Python handles the GUI via PyQt6.

## Features

- **Full CPU emulation** — SM83 (GB/GBC) and ARM7TDMI (GBA) implemented in C
- **PPU rendering** — Scanline-based renderers for both GB (160×144) and GBA (240×160)
- **Audio** — APU emulation with stereo output for both platforms
- **MBC support** — Memory bank controllers for GB/GBC cartridges
- **Save states** — Chunk-based save/load to file
- **Battery RAM** — Persistent SRAM saves
- **Unified input** — All 10 buttons (A, B, Select, Start, D-pad, L, R) mapped through a single API
- **Python C extension** — Zero-copy framebuffer access via the buffer protocol
- **PyQt6 GUI** — Keyboard input, menu bar, 3× scaled display

## Requirements

- Python ≥ 3.10
- C11 compiler (gcc, clang, or MSVC)
- PyQt6 ≥ 6.5
- NumPy ≥ 1.24

## Installation

```bash
uv sync
```

This creates a virtualenv, installs dependencies, compiles the C extension, and installs the `gbpy` command.

To include dev dependencies (pytest):

```bash
uv sync --group dev
```

## Usage

### GUI

```bash
gbpy                        # Opens the emulator window, use File → Open ROM
gbpy path/to/rom.gba        # Opens directly with a ROM
```

**Keyboard controls:**

| Key        | Button |
| ---------- | ------ |
| Z          | A      |
| X          | B      |
| Enter      | Start  |
| Backspace  | Select |
| Arrow keys | D-pad  |
| A          | L      |
| S          | R      |

**Shortcuts:** `Ctrl+O` Open ROM, `F5` Save State, `F7` Load State, `Ctrl+R` Reset, `Ctrl+Q` Quit

### Python API

```python
from gbpy import Emulator, BTN_A, BTN_START

emu = Emulator()
emu.load_rom_file("game.gba")

# Run one frame
emu.run_frame()

# Get RGBA framebuffer (width × height × 4 bytes)
pixels = emu.get_framebuffer()

# Zero-copy buffer access
view = memoryview(emu)

# Input
emu.button_press(BTN_A)
emu.button_release(BTN_A)

# Audio (interleaved int16 L/R at 44100 Hz)
audio = emu.get_audio()

# Save/load state
emu.save_state("save.state")
emu.load_state("save.state")

# Properties
print(emu.mode)           # "GB", "GBC", or "GBA"
print(emu.screen_width)   # 160 or 240
print(emu.screen_height)  # 144 or 160
```

## Project Structure

```
src/
├── core/                   # C extension (all emulation logic)
│   ├── cpu/
│   │   ├── sm83.c/h        # Game Boy CPU
│   │   └── arm7tdmi.c/h    # GBA CPU
│   ├── memory/
│   │   ├── mmu.c/h         # GB/GBC memory management
│   │   ├── mbc.c/h         # Memory bank controllers
│   │   └── gba_memory.c/h  # GBA memory bus
│   ├── video/
│   │   ├── gb_ppu.c/h      # GB/GBC pixel processing
│   │   └── gba_ppu.c/h     # GBA pixel processing
│   ├── audio/
│   │   ├── apu.c/h         # GB/GBC audio
│   │   └── gba_apu.c/h     # GBA audio
│   ├── cartridge.c/h       # ROM loader & header parsing
│   ├── timer.c/h           # GB timer
│   ├── input.c/h           # GB joypad
│   ├── emulator.c/h        # Top-level emulator (wires all subsystems)
│   ├── state.c/h           # Save state serialization
│   ├── types.h             # Shared types & constants
│   └── module.c            # Python ↔ C binding (CPython API)
└── gbpy/                   # Python package
    ├── __init__.py          # Re-exports from _core
    ├── gui.py               # PyQt6 frontend
    └── main.py              # Entry point
tests/
├── test_cpu.py
├── test_cartridge.py
└── test_input.py
```

## Running Tests

```bash
uv run pytest
```

## Supported ROM Formats

| Extension | Platform         |
| --------- | ---------------- |
| `.gb`     | Game Boy         |
| `.gbc`    | Game Boy Color   |
| `.gba`    | Game Boy Advance |

## License

MIT
