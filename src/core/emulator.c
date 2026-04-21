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

    /* Write an HLE BIOS stub for IRQ handling.
     * The IRQ vector at 0x18 branches to a handler at 0x80 that:
     * 1. Saves R0-R3, R12, LR on the IRQ stack
     * 2. Reads IE & IF, computes matched = IE & IF
     * 3. ORs matched bits into BIOS IF at [0x03FFFFF8] (mirror of 0x03007FF8)
     *    — required for IntrWait / VBlankIntrWait to detect the interrupt
     * 4. Reads the user's IRQ handler from [0x03FFFFFC]
     * 5. If handler != 0, calls it via BX (supporting Thumb handlers)
     * 6. Restores regs and returns from interrupt (SUBS PC, LR, #4)
     */
    {
        uint32_t *bios32 = (uint32_t *)emu->gba_mem.bios;
        /* 0x18: B 0x80 — branch to IRQ handler stub */
        bios32[0x18 / 4] = 0xEA000018;
        /* 0x80: STMDB SP!, {R0-R3,R12,LR} */
        bios32[0x80 / 4] = 0xE92D500F;
        /* 0x84: MOV R0, #0x04000000 — IO base */
        bios32[0x84 / 4] = 0xE3A00301;
        /* 0x88: ADD R12, R0, #0x200 — R12 = 0x04000200 (IE/IF base) */
        bios32[0x88 / 4] = 0xE280CC02;
        /* 0x8C: LDR R1, [R12] — R1 = IE | (IF << 16) */
        bios32[0x8C / 4] = 0xE59C1000;
        /* 0x90: AND R1, R1, R1, LSR #16 — R1 = IE & IF (matched) */
        bios32[0x90 / 4] = 0xE0011821;
        /* 0x94: LDR R2, [R0, #-8] — R2 = BIOS IF at [0x03FFFFF8] */
        bios32[0x94 / 4] = 0xE5102008;
        /* 0x98: ORR R2, R2, R1 — update BIOS IF with matched bits */
        bios32[0x98 / 4] = 0xE1822001;
        /* 0x9C: STR R2, [R0, #-8] — write back BIOS IF */
        bios32[0x9C / 4] = 0xE5002008;
        /* 0xA0: LDR R1, [R0, #-4] — R1 = user handler from [0x03FFFFFC] */
        bios32[0xA0 / 4] = 0xE5101004;
        /* 0xA4: CMP R1, #0 — check if handler is set */
        bios32[0xA4 / 4] = 0xE3510000;
        /* 0xA8: BEQ 0xB4 — skip call if no handler */
        bios32[0xA8 / 4] = 0x0A000001;
        /* 0xAC: ADD LR, PC, #0 — LR = 0xB4 (return addr after BX) */
        bios32[0xAC / 4] = 0xE28FE000;
        /* 0xB0: BX R1 — call user handler */
        bios32[0xB0 / 4] = 0xE12FFF11;
        /* 0xB4: LDMIA SP!, {R0-R3,R12,LR} */
        bios32[0xB4 / 4] = 0xE8BD500F;
        /* 0xB8: SUBS PC, LR, #4 — return from IRQ, restore CPSR */
        bios32[0xB8 / 4] = 0xE25EF004;
    }
    emu->gba_mem.rom = emu->cart.rom;
    emu->gba_mem.rom_size = (uint32_t)emu->cart.rom_size;
    if (emu->cart.ram && emu->cart.ram_size > 0) {
        size_t copy_size = emu->cart.ram_size < sizeof(emu->gba_mem.sram) ?
                           emu->cart.ram_size : sizeof(emu->gba_mem.sram);
        memcpy(emu->gba_mem.sram, emu->cart.ram, copy_size);
        emu->gba_mem.sram_size = (uint32_t)copy_size;
    }

    /* Configure Flash emulation based on detected save type */
    if (emu->cart.gba_save_type == GBA_SAVE_FLASH_128K) {
        emu->gba_mem.flash_is_flash = true;
        emu->gba_mem.flash_manufacturer = 0x62; /* Sanyo */
        emu->gba_mem.flash_device = 0x13;       /* 128KB */
        emu->gba_mem.sram_size = 128 * 1024;
    } else if (emu->cart.gba_save_type == GBA_SAVE_FLASH_64K) {
        emu->gba_mem.flash_is_flash = true;
        emu->gba_mem.flash_manufacturer = 0x32; /* Panasonic */
        emu->gba_mem.flash_device = 0x1B;       /* 64KB */
        emu->gba_mem.sram_size = 64 * 1024;
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

    /* Post-BIOS state: the real BIOS sets banked SPs and ends in SYS mode.
       We skip the BIOS, so replicate the final register state here.
       IMPORTANT: switch mode FIRST, then set banked registers, to avoid
       arm7_switch_mode overwriting them when saving the old mode's regs. */
    arm7_switch_mode(&emu->cpu_arm7, ARM_MODE_SYS);
    emu->cpu_arm7.r[13] = 0x03007F00;    /* SP_usr/sys (current mode) */
    emu->cpu_arm7.svc_r13 = 0x03007FE0;  /* SP_svc (banked) */
    emu->cpu_arm7.irq_r13 = 0x03007FA0;  /* SP_irq (banked) */
    /* Clear I and F flags: BIOS enables IRQs before jumping to ROM */
    emu->cpu_arm7.cpsr &= ~(ARM_FLAG_I | ARM_FLAG_F);

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

    /* Load battery save BEFORE setup so cart.ram has data when copied to gba_mem.sram */
    if (emu->cart.has_battery) {
        cartridge_load_save(&emu->cart, NULL);
    }

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
        /* Execute one CPU instruction */
        uint32_t cpu_cycles = sm83_step(&emu->cpu_sm83);

        /* Handle interrupts right after instruction (preserves original timing) */
        uint32_t int_cycles = sm83_handle_interrupts(&emu->cpu_sm83);
        uint32_t total_cycles = cpu_cycles + int_cycles;

        /* Tick timer M-cycle by M-cycle for accurate timer interrupt timing */
        for (uint32_t m = 0; m < total_cycles; m += 4) {
            uint8_t timer_int = timer_step(&emu->gb_timer, 4);
            if (timer_int) {
                sm83_request_interrupt(&emu->cpu_sm83, 0x04);
            }
        }
        /* Tick PPU for the full batch */
        uint8_t ppu_int = gb_ppu_step(&emu->gb_ppu, total_cycles);
        if (ppu_int & 0x01) {
            sm83_request_interrupt(&emu->cpu_sm83, 0x01);
        }
        if (ppu_int & 0x02) {
            sm83_request_interrupt(&emu->cpu_sm83, 0x02);
        }
        apu_step(&emu->gb_apu, total_cycles);
        mmu_dma_tick(&emu->mmu);

        emu->total_cycles += total_cycles;
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

    static int gba_frame_count = 0;

    while (emu->total_cycles < target) {
        uint32_t cycles = arm7_step(&emu->cpu_arm7);

        /* Tick subsystems */
        gba_ppu_step(&emu->gba_ppu, cycles);
        gba_timer_step(&emu->gba_mem, cycles);
        gba_apu_step(&emu->gba_apu, cycles);
        gba_dma_step(&emu->gba_mem);

        emu->total_cycles += cycles;
    }

    gba_frame_count++;
    if (gba_frame_count == 5 || gba_frame_count == 120 || gba_frame_count == 600 ||
        (gba_frame_count >= 300 && gba_frame_count <= 21100 && (gba_frame_count % 60 == 0))) {
        fprintf(stderr, "[DBG] Frame %d: PC=0x%08X CPSR=0x%08X mode=%d halted=%d KEYINPUT=0x%04X\n",
            gba_frame_count, emu->cpu_arm7.r[15], emu->cpu_arm7.cpsr,
            emu->cpu_arm7.cpsr & 0x1F, emu->cpu_arm7.halted, emu->gba_mem.keyinput);
        /* DISPCNT is first 16 bits of IO */
        uint16_t dispcnt = *(uint16_t *)&emu->gba_mem.io[0];
        uint16_t dispstat = *(uint16_t *)&emu->gba_mem.io[4];
        uint16_t ime = emu->gba_mem.ime;
        uint16_t ie = emu->gba_mem.ie;
        uint16_t ifl = emu->gba_mem.ifl;
        fprintf(stderr, "[DBG] DISPCNT=0x%04X DISPSTAT=0x%04X IME=%d IE=0x%04X IF=0x%04X (&ime=%p)\n",
            dispcnt, dispstat, ime, ie, ifl, (void*)&emu->gba_mem.ime);
        /* Check palette: any nonzero entries? */
        int pal_nz = 0;
        for (int i = 0; i < 512; i++) {
            if (emu->gba_mem.palette[i]) { pal_nz++; }
        }
        /* Check VRAM */
        int vram_nz = 0;
        for (uint32_t i = 0; i < 0x18000; i++) {
            if (emu->gba_mem.vram[i]) { vram_nz++; }
        }
        fprintf(stderr, "[DBG] Palette nonzero bytes: %d/512  VRAM nonzero bytes: %d/98304\n",
            pal_nz, vram_nz);
        /* Dump BG control, scroll, and window registers */
        {
            uint16_t bg0cnt = *(uint16_t *)&emu->gba_mem.io[0x08];
            uint16_t bg1cnt = *(uint16_t *)&emu->gba_mem.io[0x0A];
            uint16_t bg2cnt = *(uint16_t *)&emu->gba_mem.io[0x0C];
            uint16_t bg0hofs = *(uint16_t *)&emu->gba_mem.io[0x10];
            uint16_t bg0vofs = *(uint16_t *)&emu->gba_mem.io[0x12];
            uint16_t win0h = *(uint16_t *)&emu->gba_mem.io[0x40];
            uint16_t win0v = *(uint16_t *)&emu->gba_mem.io[0x44];
            uint16_t winin = *(uint16_t *)&emu->gba_mem.io[0x48];
            uint16_t winout = *(uint16_t *)&emu->gba_mem.io[0x4A];
            uint16_t bldcnt = *(uint16_t *)&emu->gba_mem.io[0x50];
            fprintf(stderr, "[DBG] BG0CNT=0x%04X BG1CNT=0x%04X BG2CNT=0x%04X\n",
                bg0cnt, bg1cnt, bg2cnt);
            fprintf(stderr, "[DBG] BG0HOFS=%d BG0VOFS=%d\n", bg0hofs, bg0vofs);
            fprintf(stderr, "[DBG] WIN0H=0x%04X WIN0V=0x%04X WININ=0x%04X WINOUT=0x%04X BLDCNT=0x%04X\n",
                win0h, win0v, winin, winout, bldcnt);
            /* Dump BLDY too */
            uint16_t bldy = *(uint16_t *)&emu->gba_mem.io[0x54];
            fprintf(stderr, "[DBG] BLDY=0x%04X\n", bldy);
        }
        /* One-time VRAM dump at menu screen */
        if (dispcnt == 0x3140 && gba_frame_count > 900) {
            static int vram_dumped = 0;
            if (!vram_dumped) {
                vram_dumped = 1;
                /* BG0CNT=0x1F08: tile_base=2 (0x8000), map_base=31 (0xF800), 4bpp */
                fprintf(stderr, "[DBG] === VRAM tilemap dump at 0xF800 ===\n");
                int nonzero = 0;
                for (int row = 0; row < 32; row++) {
                    int has_nz = 0;
                    for (int col = 0; col < 32; col++) {
                        uint16_t e = *(uint16_t *)&emu->gba_mem.vram[0xF800 + row * 64 + col * 2];
                        if (e) { has_nz = 1; nonzero++; }
                    }
                    if (has_nz) {
                        fprintf(stderr, "[DBG] Row %2d:", row);
                        for (int col = 0; col < 32; col++) {
                            uint16_t e = *(uint16_t *)&emu->gba_mem.vram[0xF800 + row * 64 + col * 2];
                            fprintf(stderr, " %04X", e);
                        }
                        fprintf(stderr, "\n");
                    }
                }
                fprintf(stderr, "[DBG] Nonzero tilemap entries: %d\n", nonzero);
                /* Count nonzero tiles at char base 0x8000, covering up to tile 1024 */
                int nz_tiles = 0;
                for (int t = 0; t < 1024; t++) {
                    uint32_t off = 0x8000 + t * 32;
                    if (off + 32 > 0x18000) break;
                    int any = 0;
                    for (int b = 0; b < 32; b++) {
                        if (emu->gba_mem.vram[off + b]) { any = 1; break; }
                    }
                    if (any) nz_tiles++;
                }
                fprintf(stderr, "[DBG] Nonzero 4bpp tiles at 0x8000: %d/1024\n", nz_tiles);
                /* Check specific tiles from tilemap */
                uint16_t check_tiles[] = {0x25B, 0x25C, 0x25D, 0x25E, 0x25F, 0x260, 0x261, 0x262, 0x263, 0x1C6, 0x1E4};
                for (int i = 0; i < 11; i++) {
                    uint32_t off = 0x8000 + check_tiles[i] * 32;
                    if (off + 32 > 0x18000) { fprintf(stderr, "[DBG] Tile 0x%03X: OUT OF RANGE\n", check_tiles[i]); continue; }
                    int any = 0;
                    for (int b = 0; b < 32; b++) {
                        if (emu->gba_mem.vram[off + b]) { any = 1; break; }
                    }
                    fprintf(stderr, "[DBG] Tile 0x%03X at VRAM 0x%05X: %s  bytes:", check_tiles[i], off, any ? "HAS DATA" : "EMPTY");
                    for (int b = 0; b < 8; b++) fprintf(stderr, " %02X", emu->gba_mem.vram[off + b]);
                    fprintf(stderr, "\n");
                }
                /* VRAM region summary */
                fprintf(stderr, "[DBG] VRAM region nz: ");
                for (int r = 0; r < 12; r++) {
                    int cnt = 0;
                    uint32_t base = r * 0x2000;
                    for (uint32_t j = 0; j < 0x2000 && base+j < 0x18000; j++) {
                        if (emu->gba_mem.vram[base + j]) cnt++;
                    }
                    fprintf(stderr, "%05X:%d ", base, cnt);
                }
                fprintf(stderr, "\n");
                /* Check palette colors for key entries */
                uint16_t bd_col = *(uint16_t *)&emu->gba_mem.palette[0]; /* backdrop */
                uint16_t fill_col = *(uint16_t *)&emu->gba_mem.palette[238 * 2]; 
                uint16_t border_col = *(uint16_t *)&emu->gba_mem.palette[225 * 2];
                uint16_t text_col = *(uint16_t *)&emu->gba_mem.palette[255 * 2];
                fprintf(stderr, "[DBG] Palette: backdrop=0x%04X fill(238)=0x%04X border(225)=0x%04X text(255)=0x%04X\n",
                    bd_col, fill_col, border_col, text_col);
                fprintf(stderr, "[DBG] PalBank14:");
                for (int i = 0; i < 16; i++) {
                    uint16_t c = *(uint16_t *)&emu->gba_mem.palette[(14*16+i)*2];
                    fprintf(stderr, " %04X", c);
                }
                fprintf(stderr, "\n");
                fprintf(stderr, "[DBG] PalBank15:");
                for (int i = 0; i < 16; i++) {
                    uint16_t c = *(uint16_t *)&emu->gba_mem.palette[(15*16+i)*2];
                    fprintf(stderr, " %04X", c);
                }
                fprintf(stderr, "\n");
            }
        }
        if (gba_frame_count == 120) {
            for (int r = 0; r < 16; r++)
                fprintf(stderr, "[DBG]   R%d=0x%08X\n", r, emu->cpu_arm7.r[r]);
            /* Dump 16 halfwords around PC */
            uint32_t pc = emu->cpu_arm7.r[15];
            int is_thumb = (emu->cpu_arm7.cpsr >> 5) & 1;
            fprintf(stderr, "[DBG]   Thumb=%d  Code around PC:\n", is_thumb);
            for (int i = -8; i < 8; i++) {
                uint32_t a = pc + i * 2;
                uint16_t v = emu->cpu_arm7.read16(emu->cpu_arm7.mem_ctx, a);
                fprintf(stderr, "[DBG]   0x%08X: 0x%04X%s\n", a, v, (a == pc) ? " <-- PC" : "");
            }
        }
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
                fprintf(stderr, "[DBG] Button PRESS btn=%d KEYINPUT=0x%04X\n", btn, emu->gba_mem.keyinput);
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
                fprintf(stderr, "[DBG] Button RELEASE btn=%d KEYINPUT=0x%04X\n", btn, emu->gba_mem.keyinput);
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
