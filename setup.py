import os
from setuptools import setup, Extension

# C extension modules for performance-critical emulation cores
core_sources = [
    "src/core/cpu/sm83.c",
    "src/core/cpu/arm7tdmi.c",
    "src/core/memory/mmu.c",
    "src/core/memory/mbc.c",
    "src/core/memory/gba_memory.c",
    "src/core/video/gb_ppu.c",
    "src/core/video/gba_ppu.c",
    "src/core/audio/apu.c",
    "src/core/audio/gba_apu.c",
    "src/core/timer.c",
    "src/core/input.c",
    "src/core/cartridge.c",
    "src/core/state.c",
    "src/core/emulator.c",
    "src/core/module.c",
]

gbpy_core = Extension(
    "gbpy._core",
    sources=core_sources,
    include_dirs=["src/core", "src/core/cpu", "src/core/memory", "src/core/video", "src/core/audio"],
    extra_compile_args=["-O3", "-std=c11", "-Wall", "-Wextra"],
    define_macros=[("GBPY_VERSION", '"0.1.0"')],
)

setup(
    ext_modules=[gbpy_core],
)
