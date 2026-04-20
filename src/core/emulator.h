/*
 * Top-level Emulator struct - ties all subsystems together
 *
 * Supports both GB/GBC (SM83 + MMU) and GBA (ARM7TDMI + GBA Memory) modes.
 * The mode is determined by the loaded cartridge type.
 */

#ifndef GBPY_EMULATOR_H
#define GBPY_EMULATOR_H

#include "types.h"
#include "cartridge.h"
#include "cpu/sm83.h"
#include "cpu/arm7tdmi.h"
#include "memory/mmu.h"
#include "memory/mbc.h"
#include "memory/gba_memory.h"
#include "video/gb_ppu.h"
#include "video/gba_ppu.h"
#include "audio/apu.h"
#include "audio/gba_apu.h"
#include "timer.h"
#include "input.h"
#include "state.h"

/* Button mapping (unified for GB + GBA) */
typedef enum {
    BTN_A      = 0,
    BTN_B      = 1,
    BTN_SELECT = 2,
    BTN_START  = 3,
    BTN_RIGHT  = 4,
    BTN_LEFT   = 5,
    BTN_UP     = 6,
    BTN_DOWN   = 7,
    BTN_L      = 8,  /* GBA only */
    BTN_R      = 9,  /* GBA only */
} Button;

typedef struct Emulator {
    GbpyMode mode;
    bool running;

    /* Cartridge */
    Cartridge cart;

    /* GB/GBC subsystems */
    SM83      cpu_sm83;
    MMU       mmu;
    MBC       mbc;
    GbPPU     gb_ppu;
    APU       gb_apu;
    GbTimer   gb_timer;
    GbInput   gb_input;

    /* GBA subsystems */
    ARM7TDMI  cpu_arm7;
    GbaMemory gba_mem;
    GbaPPU    gba_ppu;
    GbaAPU    gba_apu;

    /* Shared */
    uint64_t  total_cycles;
    bool      frame_ready;

    /* Audio output buffer (for Python to consume) */
    int16_t   audio_buffer[GBPY_AUDIO_BUFFER_SIZE * 2]; /* L/R interleaved */
    uint32_t  audio_samples_ready;
} Emulator;

/* API */
void emu_init(Emulator *emu);
int  emu_load_rom(Emulator *emu, const uint8_t *data, size_t size);
int  emu_load_rom_file(Emulator *emu, const char *path);
void emu_reset(Emulator *emu);

/* Run one frame (~280896 GBA cycles or ~70224 GB cycles) */
void emu_run_frame(Emulator *emu);

/* Step N CPU instructions */
void emu_step(Emulator *emu, uint32_t steps);

/* Input */
void emu_button_press(Emulator *emu, Button btn);
void emu_button_release(Emulator *emu, Button btn);

/* Framebuffer access */
const uint8_t *emu_get_framebuffer(const Emulator *emu);
int emu_get_screen_width(const Emulator *emu);
int emu_get_screen_height(const Emulator *emu);

/* Audio */
const int16_t *emu_get_audio_buffer(const Emulator *emu);
uint32_t emu_get_audio_samples(const Emulator *emu);
void emu_consume_audio(Emulator *emu);

/* Save/Load state */
int emu_save_state(const Emulator *emu, const char *path);
int emu_load_state(Emulator *emu, const char *path);

/* Save/Load battery RAM */
int emu_save_ram(const Emulator *emu);
int emu_load_ram(Emulator *emu);

/* Cleanup */
void emu_destroy(Emulator *emu);

#endif /* GBPY_EMULATOR_H */
