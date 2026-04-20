/*
 * Emulator Implementation
 *
 * Ties together all subsystems for both GB/GBC and GBA modes.
 * Wires up memory callbacks, initializes subsystems, and runs frames.
 */

#include "emulator.h"
#include <stdio.h>

/* ---------- Init ---------- */

void emu_init(Emulator *emu) {
    memset(emu, 0, sizeof(Emulator));
    emu->mode = GBPY_MODE_GB;
}

/* ---------- Button mapping from unified Button → GbButton ---------- */
static GbButton button_to_gb(Button btn) {
    switch (btn) {
        case BTN_A:      return GB_BTN_A;
        case BTN_B:      return GB_BTN_B;
        case BTN_SELECT: return GB_BTN_SELECT;
        case BTN_START:  return GB_BTN_START;
        case BTN_RIGHT:  return GB_BTN_RIGHT;
        case BTN_LEFT:   return GB_BTN_LEFT;
        case BTN_UP:     return GB_BTN_UP;
        case BTN_DOWN:   return GB_BTN_DOWN;
        default:         return GB_BTN_A;
    }
}

/* ---------- Wire up subsystems ---------- */

static void setup_gb(Emulator *emu) {
    GbpyMode mode = (emu->cart.type == CART_TYPE_GBC) ? GBPY_MODE_GBC : GBPY_MODE_GB;
    emu->mode = mode;

    /* MBC */
    mbc_init(&emu->mbc, emu->cart.rom, emu->cart.rom_size);
    emu->mbc.ram = emu->cart.ram;
    emu->mbc.ram_size = emu->cart.ram_size;

    /* MMU */
    mmu_init(&emu->mmu, mode);
    emu->mmu.rom = emu->cart.rom;
    emu->mmu.rom_size = emu->cart.rom_size;
    emu->mmu.eram = emu->cart.ram;
    emu->mmu.eram_size = emu->cart.ram_size;
    emu->mmu.mbc = &emu->mbc;
    emu->mmu.ppu = &emu->gb_ppu;
    emu->mmu.apu = &emu->gb_apu;
    emu->mmu.timer = &emu->gb_timer;
    emu->mmu.input = &emu->gb_input;

    /* CPU */
    sm83_init(&emu->cpu_sm83, mmu_read, mmu_write, &emu->mmu);
    sm83_reset(&emu->cpu_sm83, mode);

    /* PPU */
    gb_ppu_init(&emu->gb_ppu, mode);
    emu->gb_ppu.vram = emu->mmu.vram;
    emu->gb_ppu.oam  = emu->mmu.oam;
    emu->gb_ppu.io   = emu->mmu.io;

    /* APU */
    apu_init(&emu->gb_apu);
    emu->gb_apu.io = emu->mmu.io;

    /* Timer */
    timer_init(&emu->gb_timer);

    /* Input */
    input_init(&emu->gb_input);
}

static void setup_gba(Emulator *emu) {
    emu->mode = GBPY_MODE_GBA;

    /* Memory */
    gba_mem_init(&emu->gba_mem);
    emu->gba_mem.rom = emu->cart.rom;
    emu->gba_mem.rom_size = (uint32_t)emu->cart.rom_size;
    if (emu->cart.ram && emu->cart.ram_size > 0) {
        size_t copy_size = emu->cart.ram_size < sizeof(emu->gba_mem.sram) ?
                           emu->cart.ram_size : sizeof(emu->gba_mem.sram);
        memcpy(emu->gba_mem.sram, emu->cart.ram, copy_size);
        emu->gba_mem.sram_size = (uint32_t)copy_size;
    }
    emu->gba_mem.cpu = &emu->cpu_arm7;
    emu->gba_mem.ppu = &emu->gba_ppu;
    emu->gba_mem.apu = &emu->gba_apu;

    /* CPU */
    arm7_init(&emu->cpu_arm7);
    emu->cpu_arm7.read8 = gba_mem_read8;
    emu->cpu_arm7.read16 = gba_mem_read16;
    emu->cpu_arm7.read32 = gba_mem_read32;
    emu->cpu_arm7.write8 = gba_mem_write8;
    emu->cpu_arm7.write16 = gba_mem_write16;
    emu->cpu_arm7.write32 = gba_mem_write32;
    emu->cpu_arm7.mem_ctx = &emu->gba_mem;
    arm7_reset(&emu->cpu_arm7);

    /* PPU */
    gba_ppu_init(&emu->gba_ppu);
    emu->gba_ppu.mem = &emu->gba_mem;

    /* APU */
    gba_apu_init(&emu->gba_apu);
    emu->gba_apu.mem = &emu->gba_mem;

    /* Set KEYINPUT to all released (active low: 0x3FF means all released) */
    emu->gba_mem.keyinput = 0x03FF;
}

/* ---------- Load ROM ---------- */

int emu_load_rom(Emulator *emu, const uint8_t *data, size_t size) {
    emu_destroy(emu);
    emu_init(emu);

    int result = cartridge_load(&emu->cart, data, size);
    if (result != GBPY_OK) return result;

    switch (emu->cart.type) {
        case CART_TYPE_GB:
        case CART_TYPE_GBC:
            setup_gb(emu);
            break;
        case CART_TYPE_GBA:
            setup_gba(emu);
            break;
        default:
            return GBPY_ERR_INVALID_ROM;
    }

    emu->running = true;
    return GBPY_OK;
}

int emu_load_rom_file(Emulator *emu, const char *path) {
    emu_destroy(emu);
    emu_init(emu);

    int result = cartridge_load_file(&emu->cart, path);
    if (result != GBPY_OK) return result;

    switch (emu->cart.type) {
        case CART_TYPE_GB:
        case CART_TYPE_GBC:
            setup_gb(emu);
            break;
        case CART_TYPE_GBA:
            setup_gba(emu);
            break;
        default:
            return GBPY_ERR_INVALID_ROM;
    }

    /* Try to load battery save */
    if (emu->cart.has_battery) {
        cartridge_load_save(&emu->cart, NULL);
    }

    emu->running = true;
    return GBPY_OK;
}

/* ---------- Reset ---------- */

void emu_reset(Emulator *emu) {
    switch (emu->mode) {
        case GBPY_MODE_GB:
        case GBPY_MODE_GBC:
            sm83_reset(&emu->cpu_sm83, emu->mode);
            gb_ppu_init(&emu->gb_ppu, emu->mode);
            emu->gb_ppu.vram = emu->mmu.vram;
            emu->gb_ppu.oam  = emu->mmu.oam;
            emu->gb_ppu.io   = emu->mmu.io;
            apu_init(&emu->gb_apu);
            emu->gb_apu.io = emu->mmu.io;
            timer_init(&emu->gb_timer);
            input_init(&emu->gb_input);
            break;
        case GBPY_MODE_GBA:
            arm7_reset(&emu->cpu_arm7);
            gba_ppu_reset(&emu->gba_ppu);
            gba_apu_reset(&emu->gba_apu);
            gba_mem_reset(&emu->gba_mem);
            /* Rewire */
            emu->gba_mem.cpu = &emu->cpu_arm7;
            emu->gba_mem.ppu = &emu->gba_ppu;
            emu->gba_mem.apu = &emu->gba_apu;
            emu->gba_ppu.mem = &emu->gba_mem;
            emu->gba_apu.mem = &emu->gba_mem;
            emu->gba_mem.keyinput = 0x03FF;
            break;
    }
    emu->total_cycles = 0;
    emu->frame_ready = false;
}

/* ---------- Run one frame ---------- */

static void run_gb_frame(Emulator *emu) {
    /* GB: 70224 cycles per frame (456 per line * 154 lines) */
    const uint32_t FRAME_CYCLES = 70224;
    uint64_t target = emu->total_cycles + FRAME_CYCLES;

    while (emu->total_cycles < target) {
        uint32_t cycles = sm83_step(&emu->cpu_sm83);
        cycles += sm83_handle_interrupts(&emu->cpu_sm83);

        /* Tick subsystems */
        uint8_t timer_int = timer_step(&emu->gb_timer, cycles);
        if (timer_int) {
            sm83_request_interrupt(&emu->cpu_sm83, 0x04); /* INT_TIMER */
        }
        uint8_t ppu_int = gb_ppu_step(&emu->gb_ppu, cycles);
        if (ppu_int & 0x01) { /* INT_VBLANK */
            sm83_request_interrupt(&emu->cpu_sm83, 0x01);
        }
        if (ppu_int & 0x02) { /* INT_LCD */
            sm83_request_interrupt(&emu->cpu_sm83, 0x02);
        }
        apu_step(&emu->gb_apu, cycles);
        mmu_dma_tick(&emu->mmu);

        emu->total_cycles += cycles;
    }

    /* Copy audio samples from APU circular buffer */
    if (emu->gb_apu.buffer_pos > 0) {
        uint32_t count = emu->gb_apu.buffer_pos;
        if (count > GBPY_AUDIO_BUFFER_SIZE) count = GBPY_AUDIO_BUFFER_SIZE;
        for (uint32_t i = 0; i < count; i++) {
            emu->audio_buffer[i * 2]     = emu->gb_apu.buffer[i].left;
            emu->audio_buffer[i * 2 + 1] = emu->gb_apu.buffer[i].right;
        }
        emu->audio_samples_ready = count;
        emu->gb_apu.buffer_pos = 0;
        emu->gb_apu.buffer_full = false;
    }

    emu->frame_ready = true;
}

static void run_gba_frame(Emulator *emu) {
    /* GBA: 280896 cycles per frame (1232 per line * 228 lines) */
    const uint32_t FRAME_CYCLES = 280896;
    uint64_t target = emu->total_cycles + FRAME_CYCLES;

    emu->gba_ppu.frame_ready = false;

    while (emu->total_cycles < target) {
        uint32_t cycles = arm7_step(&emu->cpu_arm7);

        /* Tick subsystems */
        gba_ppu_step(&emu->gba_ppu, cycles);
        gba_timer_step(&emu->gba_mem, cycles);
        gba_apu_step(&emu->gba_apu, cycles);
        gba_dma_step(&emu->gba_mem);

        emu->total_cycles += cycles;
    }

    /* Copy audio */
    if (emu->gba_apu.sample_count > 0) {
        uint32_t count = emu->gba_apu.sample_count;
        if (count > GBPY_AUDIO_BUFFER_SIZE) count = GBPY_AUDIO_BUFFER_SIZE;
        memcpy(emu->audio_buffer, emu->gba_apu.sample_buffer, count * 2 * sizeof(int16_t));
        emu->audio_samples_ready = count;
        emu->gba_apu.sample_count = 0;
        emu->gba_apu.buffer_full = false;
    }

    emu->frame_ready = true;
}

void emu_run_frame(Emulator *emu) {
    if (!emu->running) return;
    switch (emu->mode) {
        case GBPY_MODE_GB:
        case GBPY_MODE_GBC:
            run_gb_frame(emu);
            break;
        case GBPY_MODE_GBA:
            run_gba_frame(emu);
            break;
    }
}

/* ---------- Step ---------- */

void emu_step(Emulator *emu, uint32_t steps) {
    if (!emu->running) return;
    for (uint32_t i = 0; i < steps; i++) {
        switch (emu->mode) {
            case GBPY_MODE_GB:
            case GBPY_MODE_GBC: {
                uint32_t c = sm83_step(&emu->cpu_sm83);
                c += sm83_handle_interrupts(&emu->cpu_sm83);
                uint8_t ti = timer_step(&emu->gb_timer, c);
                if (ti) sm83_request_interrupt(&emu->cpu_sm83, 0x04);
                uint8_t pi = gb_ppu_step(&emu->gb_ppu, c);
                if (pi & 0x01) sm83_request_interrupt(&emu->cpu_sm83, 0x01);
                if (pi & 0x02) sm83_request_interrupt(&emu->cpu_sm83, 0x02);
                apu_step(&emu->gb_apu, c);
                emu->total_cycles += c;
                break;
            }
            case GBPY_MODE_GBA: {
                uint32_t c = arm7_step(&emu->cpu_arm7);
                gba_ppu_step(&emu->gba_ppu, c);
                gba_timer_step(&emu->gba_mem, c);
                gba_apu_step(&emu->gba_apu, c);
                gba_dma_step(&emu->gba_mem);
                emu->total_cycles += c;
                break;
            }
        }
    }
}

/* ---------- Input ---------- */

void emu_button_press(Emulator *emu, Button btn) {
    switch (emu->mode) {
        case GBPY_MODE_GB:
        case GBPY_MODE_GBC:
            input_press(&emu->gb_input, button_to_gb(btn));
            break;
        case GBPY_MODE_GBA:
            if (btn <= BTN_R) {
                emu->gba_mem.keyinput &= ~(1 << btn);
            }
            break;
    }
}

void emu_button_release(Emulator *emu, Button btn) {
    switch (emu->mode) {
        case GBPY_MODE_GB:
        case GBPY_MODE_GBC:
            input_release(&emu->gb_input, button_to_gb(btn));
            break;
        case GBPY_MODE_GBA:
            if (btn <= BTN_R) {
                emu->gba_mem.keyinput |= (1 << btn);
            }
            break;
    }
}

/* ---------- Framebuffer ---------- */

const uint8_t *emu_get_framebuffer(const Emulator *emu) {
    switch (emu->mode) {
        case GBPY_MODE_GB:
        case GBPY_MODE_GBC:
            return (const uint8_t *)emu->gb_ppu.framebuffer;
        case GBPY_MODE_GBA:
            return emu->gba_ppu.framebuffer;
        default:
            return NULL;
    }
}

int emu_get_screen_width(const Emulator *emu) {
    return (emu->mode == GBPY_MODE_GBA) ? GBA_SCREEN_WIDTH : GB_SCREEN_WIDTH;
}

int emu_get_screen_height(const Emulator *emu) {
    return (emu->mode == GBPY_MODE_GBA) ? GBA_SCREEN_HEIGHT : GB_SCREEN_HEIGHT;
}

/* ---------- Audio ---------- */

const int16_t *emu_get_audio_buffer(const Emulator *emu) {
    return emu->audio_buffer;
}

uint32_t emu_get_audio_samples(const Emulator *emu) {
    return emu->audio_samples_ready;
}

void emu_consume_audio(Emulator *emu) {
    emu->audio_samples_ready = 0;
}

/* ---------- Save/Load state ---------- */

int emu_save_state(const Emulator *emu, const char *path) {
    return state_save(emu, path);
}

int emu_load_state(Emulator *emu, const char *path) {
    return state_load(emu, path);
}

/* ---------- Battery RAM ---------- */

int emu_save_ram(const Emulator *emu) {
    if (!emu->cart.has_battery) return GBPY_OK;

    if (emu->mode == GBPY_MODE_GBA) {
        /* Copy SRAM back to cartridge RAM before saving */
        /* (GBA SRAM is in gba_mem.sram, cart.ram is the persistent copy) */
        Cartridge *cart = (Cartridge *)&emu->cart;
        if (cart->ram && cart->ram_size > 0) {
            size_t copy_size = cart->ram_size < sizeof(emu->gba_mem.sram) ?
                               cart->ram_size : sizeof(emu->gba_mem.sram);
            memcpy(cart->ram, emu->gba_mem.sram, copy_size);
        }
    }
    return cartridge_save_ram(&emu->cart, NULL);
}

int emu_load_ram(Emulator *emu) {
    if (!emu->cart.has_battery) return GBPY_OK;
    int result = cartridge_load_save(&emu->cart, NULL);

    if (result == GBPY_OK && emu->mode == GBPY_MODE_GBA) {
        if (emu->cart.ram && emu->cart.ram_size > 0) {
            size_t copy_size = emu->cart.ram_size < sizeof(emu->gba_mem.sram) ?
                               emu->cart.ram_size : sizeof(emu->gba_mem.sram);
            memcpy(emu->gba_mem.sram, emu->cart.ram, copy_size);
        }
    }
    return result;
}

/* ---------- Cleanup ---------- */

void emu_destroy(Emulator *emu) {
    if (!emu) return;
    cartridge_free(&emu->cart);
    emu->running = false;
}
