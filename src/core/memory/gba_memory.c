/*
 * GBA Memory Implementation
 *
 * Full GBA memory map, IO register handling, DMA, timers.
 */

#include "gba_memory.h"
#include "../cpu/arm7tdmi.h"
#include "../audio/gba_apu.h"
#include <stdio.h>

/* ---------- Init/Reset ---------- */

void gba_mem_init(GbaMemory *mem) {
    memset(mem, 0, sizeof(GbaMemory));
    mem->keyinput = 0x03FF; /* All buttons released (active low) */
    mem->io[0x300] = 0x01;  /* POSTFLG: BIOS startup finished */
    *(uint16_t *)&mem->io[0x088] = 0x0200; /* SOUNDBIAS default after BIOS */
    mem->bios_readable = true;
}

void gba_mem_reset(GbaMemory *mem) {
    uint8_t *rom = mem->rom;
    uint32_t rom_size = mem->rom_size;
    ARM7TDMI *cpu = mem->cpu;
    void *ppu = mem->ppu;
    void *apu = mem->apu;

    memset(mem->ewram, 0, sizeof(mem->ewram));
    memset(mem->iwram, 0, sizeof(mem->iwram));
    memset(mem->io, 0, sizeof(mem->io));
    memset(mem->palette, 0, sizeof(mem->palette));
    memset(mem->vram, 0, sizeof(mem->vram));
    memset(mem->oam, 0, sizeof(mem->oam));
    memset(mem->dma, 0, sizeof(mem->dma));
    memset(mem->timers, 0, sizeof(mem->timers));

    mem->rom = rom;
    mem->rom_size = rom_size;
    mem->cpu = cpu;
    mem->ppu = (GbaPPU *)ppu;
    mem->apu = (GbaAPU *)apu;
    mem->keyinput = 0x03FF;
    mem->io[0x300] = 0x01;  /* POSTFLG: BIOS startup finished */
    *(uint16_t *)&mem->io[0x088] = 0x0200; /* SOUNDBIAS default after BIOS */
    mem->ie = 0;
    mem->ifl = 0;
    mem->ime = 0;
    mem->open_bus = 0;
    mem->bios_latch = 0;
    mem->bios_readable = true;
}

/* ---------- Internal IO read/write ---------- */

static uint16_t io_read16(GbaMemory *mem, uint32_t offset) {
    switch (offset) {
        /* Display */
        case 0x000: return *(uint16_t *)&mem->io[0x000]; /* DISPCNT */
        case 0x004: return *(uint16_t *)&mem->io[0x004]; /* DISPSTAT */
        case 0x006: return *(uint16_t *)&mem->io[0x006]; /* VCOUNT */
        case 0x008: return *(uint16_t *)&mem->io[0x008]; /* BG0CNT */
        case 0x00A: return *(uint16_t *)&mem->io[0x00A]; /* BG1CNT */
        case 0x00C: return *(uint16_t *)&mem->io[0x00C]; /* BG2CNT */
        case 0x00E: return *(uint16_t *)&mem->io[0x00E]; /* BG3CNT */

        /* Sound */
        case 0x060: return *(uint16_t *)&mem->io[0x060]; /* SOUND1CNT_L */
        case 0x062: return *(uint16_t *)&mem->io[0x062]; /* SOUND1CNT_H */
        case 0x064: return *(uint16_t *)&mem->io[0x064]; /* SOUND1CNT_X */
        case 0x068: return *(uint16_t *)&mem->io[0x068]; /* SOUND2CNT_L */
        case 0x06C: return *(uint16_t *)&mem->io[0x06C]; /* SOUND2CNT_H */
        case 0x070: return *(uint16_t *)&mem->io[0x070]; /* SOUND3CNT_L */
        case 0x072: return *(uint16_t *)&mem->io[0x072]; /* SOUND3CNT_H */
        case 0x074: return *(uint16_t *)&mem->io[0x074]; /* SOUND3CNT_X */
        case 0x078: return *(uint16_t *)&mem->io[0x078]; /* SOUND4CNT_L */
        case 0x07C: return *(uint16_t *)&mem->io[0x07C]; /* SOUND4CNT_H */
        case 0x080: return *(uint16_t *)&mem->io[0x080]; /* SOUNDCNT_L */
        case 0x082: return *(uint16_t *)&mem->io[0x082]; /* SOUNDCNT_H */
        case 0x084: return *(uint16_t *)&mem->io[0x084]; /* SOUNDCNT_X */
        case 0x088: return *(uint16_t *)&mem->io[0x088]; /* SOUNDBIAS */

        /* DMA */
        case 0x0BA: return mem->dma[0].control;
        case 0x0C6: return mem->dma[1].control;
        case 0x0D2: return mem->dma[2].control;
        case 0x0DE: return mem->dma[3].control;

        /* Timers */
        case 0x100: return mem->timers[0].counter;
        case 0x102: return mem->timers[0].control;
        case 0x104: return mem->timers[1].counter;
        case 0x106: return mem->timers[1].control;
        case 0x108: return mem->timers[2].counter;
        case 0x10A: return mem->timers[2].control;
        case 0x10C: return mem->timers[3].counter;
        case 0x10E: return mem->timers[3].control;

        /* Key input */
        case 0x130: return mem->keyinput;
        case 0x132: return mem->keycnt;

        /* Interrupts */
        case 0x200: return mem->ie;
        case 0x202: return mem->ifl;
        case 0x208: return mem->ime;

        /* Wait state control */
        case 0x204: return mem->waitcnt;

        default:
            if (offset < 0x400)
                return *(uint16_t *)&mem->io[offset];
            return 0;
    }
}

static void io_write16(GbaMemory *mem, uint32_t offset, uint16_t val) {
    static int io_trace = 0;
    if (io_trace < 200 && (offset == 0x000 || offset == 0x200 || offset == 0x202 || offset == 0x208)) {
        fprintf(stderr, "[IO_W16] offset=0x%03X val=0x%04X\n", offset, val);
        io_trace++;
    }
    switch (offset) {
        /* Display */
        case 0x000: *(uint16_t *)&mem->io[0x000] = val; return;
        case 0x004: {
            /* DISPSTAT: writable bits are 3-5 and 8-15 */
            uint16_t old = *(uint16_t *)&mem->io[0x004];
            *(uint16_t *)&mem->io[0x004] = (old & 0x7) | (val & 0xFFF8);
            return;
        }
        case 0x008: case 0x00A: case 0x00C: case 0x00E: /* BGxCNT */
            *(uint16_t *)&mem->io[offset] = val;
            return;

        /* BG scroll registers */
        case 0x010: case 0x012: case 0x014: case 0x016:
        case 0x018: case 0x01A: case 0x01C: case 0x01E:
            *(uint16_t *)&mem->io[offset] = val;
            return;

        /* BG rotation/scaling (BG2/3) */
        case 0x020: case 0x022: case 0x024: case 0x026:
        case 0x028: case 0x02A: case 0x02C: case 0x02E:
        case 0x030: case 0x032: case 0x034: case 0x036:
        case 0x038: case 0x03A: case 0x03C: case 0x03E:
            *(uint16_t *)&mem->io[offset] = val;
            return;

        /* Window */
        case 0x040: case 0x042: case 0x044: case 0x046:
        case 0x048: case 0x04A:
            *(uint16_t *)&mem->io[offset] = val;
            return;

        /* Mosaic */
        case 0x04C:
            *(uint16_t *)&mem->io[offset] = val;
            return;

        /* Blend */
        case 0x050: case 0x052: case 0x054:
            *(uint16_t *)&mem->io[offset] = val;
            return;

        /* Sound registers — update APU state for legacy channels */
        case 0x060: case 0x062: case 0x064: case 0x068:
        case 0x06C: case 0x070: case 0x072: case 0x074:
        case 0x078: case 0x07C: case 0x080: case 0x084:
        case 0x088:
            *(uint16_t *)&mem->io[offset] = val;
            if (mem->apu) {
                gba_apu_write_reg(mem->apu, offset, val & 0xFF);
                gba_apu_write_reg(mem->apu, offset + 1, (val >> 8) & 0xFF);
            }
            return;

        /* SOUNDCNT_H — handle FIFO reset bits */
        case 0x082:
            if (mem->apu) {
                if (val & (1 << 11)) gba_fifo_reset(&mem->apu->fifo_a);
                if (val & (1 << 15)) gba_fifo_reset(&mem->apu->fifo_b);
            }
            /* Clear write-only reset bits before storing */
            *(uint16_t *)&mem->io[0x082] = val & ~((1 << 11) | (1 << 15));
            return;

        /* Sound FIFO — push samples into APU ring buffers */
        case 0x0A0: case 0x0A2: /* FIFO_A */
            if (mem->apu) {
                gba_fifo_write(&mem->apu->fifo_a, (int8_t)(val & 0xFF));
                gba_fifo_write(&mem->apu->fifo_a, (int8_t)((val >> 8) & 0xFF));
            }
            return;
        case 0x0A4: case 0x0A6: /* FIFO_B */
            if (mem->apu) {
                gba_fifo_write(&mem->apu->fifo_b, (int8_t)(val & 0xFF));
                gba_fifo_write(&mem->apu->fifo_b, (int8_t)((val >> 8) & 0xFF));
            }
            return;

        /* DMA 0 */
        case 0x0B0: mem->dma[0].src = (mem->dma[0].src & 0xFFFF0000) | val; return;
        case 0x0B2: mem->dma[0].src = (mem->dma[0].src & 0x0000FFFF) | ((uint32_t)val << 16); return;
        case 0x0B4: mem->dma[0].dst = (mem->dma[0].dst & 0xFFFF0000) | val; return;
        case 0x0B6: mem->dma[0].dst = (mem->dma[0].dst & 0x0000FFFF) | ((uint32_t)val << 16); return;
        case 0x0B8: mem->dma[0].count = val; return;
        case 0x0BA: {
            bool was_enabled = mem->dma[0].control & (1 << 15);
            mem->dma[0].control = val;
            if (!was_enabled && (val & (1 << 15))) {
                mem->dma[0].internal_src = mem->dma[0].src & 0x07FFFFFF;
                mem->dma[0].internal_dst = mem->dma[0].dst & 0x07FFFFFF;
                mem->dma[0].internal_count = mem->dma[0].count ? mem->dma[0].count : 0x4000;
                if (((val >> 12) & 3) == DMA_TIMING_NOW) {
                    mem->dma[0].active = true;
                }
            }
            return;
        }

        /* DMA 1 */
        case 0x0BC: mem->dma[1].src = (mem->dma[1].src & 0xFFFF0000) | val; return;
        case 0x0BE: mem->dma[1].src = (mem->dma[1].src & 0x0000FFFF) | ((uint32_t)val << 16); return;
        case 0x0C0: mem->dma[1].dst = (mem->dma[1].dst & 0xFFFF0000) | val; return;
        case 0x0C2: mem->dma[1].dst = (mem->dma[1].dst & 0x0000FFFF) | ((uint32_t)val << 16); return;
        case 0x0C4: mem->dma[1].count = val; return;
        case 0x0C6: {
            bool was_enabled = mem->dma[1].control & (1 << 15);
            mem->dma[1].control = val;
            if (!was_enabled && (val & (1 << 15))) {
                mem->dma[1].internal_src = mem->dma[1].src & 0x0FFFFFFF;
                mem->dma[1].internal_dst = mem->dma[1].dst & 0x07FFFFFF;
                mem->dma[1].internal_count = mem->dma[1].count ? mem->dma[1].count : 0x4000;
                if (((val >> 12) & 3) == DMA_TIMING_NOW) {
                    mem->dma[1].active = true;
                }
            }
            return;
        }

        /* DMA 2 */
        case 0x0C8: mem->dma[2].src = (mem->dma[2].src & 0xFFFF0000) | val; return;
        case 0x0CA: mem->dma[2].src = (mem->dma[2].src & 0x0000FFFF) | ((uint32_t)val << 16); return;
        case 0x0CC: mem->dma[2].dst = (mem->dma[2].dst & 0xFFFF0000) | val; return;
        case 0x0CE: mem->dma[2].dst = (mem->dma[2].dst & 0x0000FFFF) | ((uint32_t)val << 16); return;
        case 0x0D0: mem->dma[2].count = val; return;
        case 0x0D2: {
            bool was_enabled = mem->dma[2].control & (1 << 15);
            mem->dma[2].control = val;
            if (!was_enabled && (val & (1 << 15))) {
                mem->dma[2].internal_src = mem->dma[2].src & 0x0FFFFFFF;
                mem->dma[2].internal_dst = mem->dma[2].dst & 0x07FFFFFF;
                mem->dma[2].internal_count = mem->dma[2].count ? mem->dma[2].count : 0x4000;
                if (((val >> 12) & 3) == DMA_TIMING_NOW) {
                    mem->dma[2].active = true;
                }
            }
            return;
        }

        /* DMA 3 */
        case 0x0D4: mem->dma[3].src = (mem->dma[3].src & 0xFFFF0000) | val; return;
        case 0x0D6: mem->dma[3].src = (mem->dma[3].src & 0x0000FFFF) | ((uint32_t)val << 16); return;
        case 0x0D8: mem->dma[3].dst = (mem->dma[3].dst & 0xFFFF0000) | val; return;
        case 0x0DA: mem->dma[3].dst = (mem->dma[3].dst & 0x0000FFFF) | ((uint32_t)val << 16); return;
        case 0x0DC: mem->dma[3].count = val; return;
        case 0x0DE: {
            bool was_enabled = mem->dma[3].control & (1 << 15);
            mem->dma[3].control = val;
            if (!was_enabled && (val & (1 << 15))) {
                mem->dma[3].internal_src = mem->dma[3].src & 0x0FFFFFFF;
                mem->dma[3].internal_dst = mem->dma[3].dst & 0x0FFFFFFF;
                mem->dma[3].internal_count = mem->dma[3].count ? mem->dma[3].count : 0x10000;
                if (((val >> 12) & 3) == DMA_TIMING_NOW) {
                    mem->dma[3].active = true;
                }
            }
            return;
        }

        /* Timers */
        case 0x100: mem->timers[0].reload = val; return;
        case 0x102: {
            bool was_off = !(mem->timers[0].control & TIMER_ENABLE);
            mem->timers[0].control = val;
            if (was_off && (val & TIMER_ENABLE)) {
                mem->timers[0].counter = mem->timers[0].reload;
                mem->timers[0].prescaler = 0;
            }
            return;
        }
        case 0x104: mem->timers[1].reload = val; return;
        case 0x106: {
            bool was_off = !(mem->timers[1].control & TIMER_ENABLE);
            mem->timers[1].control = val;
            if (was_off && (val & TIMER_ENABLE)) {
                mem->timers[1].counter = mem->timers[1].reload;
                mem->timers[1].prescaler = 0;
            }
            return;
        }
        case 0x108: mem->timers[2].reload = val; return;
        case 0x10A: {
            bool was_off = !(mem->timers[2].control & TIMER_ENABLE);
            mem->timers[2].control = val;
            if (was_off && (val & TIMER_ENABLE)) {
                mem->timers[2].counter = mem->timers[2].reload;
                mem->timers[2].prescaler = 0;
            }
            return;
        }
        case 0x10C: mem->timers[3].reload = val; return;
        case 0x10E: {
            bool was_off = !(mem->timers[3].control & TIMER_ENABLE);
            mem->timers[3].control = val;
            if (was_off && (val & TIMER_ENABLE)) {
                mem->timers[3].counter = mem->timers[3].reload;
                mem->timers[3].prescaler = 0;
            }
            return;
        }

        /* Key control */
        case 0x132: mem->keycnt = val; return;

        /* Interrupts */
        case 0x200: mem->ie = val; return;
        case 0x202: mem->ifl &= ~val; return; /* Write 1 to acknowledge */
        case 0x208: {
            uint16_t old_ime = mem->ime;
            mem->ime = val & 1;
            static int ime_trace = 0;
            if (ime_trace < 20) {
                fprintf(stderr, "[IME] %d -> %d (val=0x%04X)\n", old_ime, mem->ime, val);
                ime_trace++;
            }
            if (old_ime && !mem->ime) {
                static int ime_clr = 0;
                if (ime_clr < 5) {
                    fprintf(stderr, "[IME_CLR] IME cleared! val=0x%04X\n", val);
                    ime_clr++;
                }
            }
            return;
        }

        /* Wait control */
        case 0x204: mem->waitcnt = val; return;

        /* Halt */
        case 0x300:
            mem->postflg = val & 1;
            return;
        case 0x301: /* HALTCNT - actually written via 8-bit */
            return;

        default:
            if (offset < 0x400)
                *(uint16_t *)&mem->io[offset] = val;
            return;
    }
}

static void io_write8(GbaMemory *mem, uint32_t offset, uint8_t val) {
    /* HALTCNT special case */
    if (offset == 0x301) {
        if (val & 0x80) {
            /* Stop mode - not typically used */
        } else {
            /* Halt mode */
            if (mem->cpu) mem->cpu->halted = true;
        }
        return;
    }

    /* POSTFLG */
    if (offset == 0x300) {
        mem->postflg = val & 1;
        return;
    }

    /* Sound FIFO_A (0x4000A0-0x4000A3) */
    if (offset >= 0xA0 && offset <= 0xA3) {
        if (mem->apu) gba_fifo_write(&mem->apu->fifo_a, (int8_t)val);
        return;
    }
    /* Sound FIFO_B (0x4000A4-0x4000A7) */
    if (offset >= 0xA4 && offset <= 0xA7) {
        if (mem->apu) gba_fifo_write(&mem->apu->fifo_b, (int8_t)val);
        return;
    }

    /* General: read-modify-write the 16-bit register */
    uint32_t aligned = offset & ~1u;
    uint16_t old = io_read16(mem, aligned);
    if (offset & 1) {
        old = (old & 0x00FF) | ((uint16_t)val << 8);
    } else {
        old = (old & 0xFF00) | val;
    }
    io_write16(mem, aligned, old);
}

/* ---------- Flash state machine ---------- */

#define FLASH_STATE_READY   0
#define FLASH_STATE_CMD1    1  /* Received 0xAA at 0x5555 */
#define FLASH_STATE_CMD2    2  /* Received 0x55 at 0x2AAA */
#define FLASH_STATE_ERASE1  3  /* Received 0x80 erase command, waiting for 0xAA */
#define FLASH_STATE_ERASE2  4  /* Received 0xAA in erase sequence */
#define FLASH_STATE_ERASE3  5  /* Received 0x55 in erase sequence */

static uint8_t flash_read(GbaMemory *mem, uint32_t addr) {
    uint16_t offset = addr & 0xFFFF;

    if (mem->flash_id_mode) {
        if (offset == 0x0000) return mem->flash_manufacturer;
        if (offset == 0x0001) return mem->flash_device;
        return 0;
    }

    /* Normal read: apply bank offset for 128KB Flash */
    uint32_t flash_addr = (uint32_t)mem->flash_bank * 0x10000 + offset;
    if (flash_addr < mem->sram_size)
        return mem->sram[flash_addr];
    return 0xFF;
}

static void flash_write(GbaMemory *mem, uint32_t addr, uint8_t val) {
    uint16_t offset = addr & 0xFFFF;

    /* Single byte write (after 0xA0 program command) */
    if (mem->flash_write_enable) {
        uint32_t flash_addr = (uint32_t)mem->flash_bank * 0x10000 + offset;
        if (flash_addr < mem->sram_size)
            mem->sram[flash_addr] = val;
        mem->flash_write_enable = false;
        mem->flash_state = FLASH_STATE_READY;
        return;
    }

    /* Bank select (after 0xB0 command): write bank number to 0x0000 */
    if (mem->flash_state == FLASH_STATE_CMD2 && offset == 0x0000 &&
        mem->sram_size > 0x10000) {
        /* This actually happens after a specific sequence, but for simplicity: */
    }

    /* Command sequence processing */
    switch (mem->flash_state) {
        case FLASH_STATE_READY:
            if (offset == 0x5555 && val == 0xAA) {
                mem->flash_state = FLASH_STATE_CMD1;
            }
            break;

        case FLASH_STATE_CMD1:
            if (offset == 0x2AAA && val == 0x55) {
                mem->flash_state = FLASH_STATE_CMD2;
            } else {
                mem->flash_state = FLASH_STATE_READY;
            }
            break;

        case FLASH_STATE_CMD2:
            if (offset == 0x5555) {
                switch (val) {
                    case 0x90: /* Enter ID mode */
                        mem->flash_id_mode = true;
                        mem->flash_state = FLASH_STATE_READY;
                        break;
                    case 0xF0: /* Exit ID mode / reset */
                        mem->flash_id_mode = false;
                        mem->flash_state = FLASH_STATE_READY;
                        break;
                    case 0x80: /* Erase command (first part) */
                        mem->flash_state = FLASH_STATE_ERASE1;
                        break;
                    case 0xA0: /* Write single byte */
                        mem->flash_write_enable = true;
                        mem->flash_state = FLASH_STATE_READY;
                        break;
                    case 0xB0: /* Bank switch (128KB Flash) */
                        /* Next write to 0x0000 selects the bank */
                        mem->flash_state = FLASH_STATE_READY;
                        /* Actually, bank byte comes to address 0x0000 next */
                        if (mem->sram_size > 0x10000) {
                            /* Wait for bank number at 0x0000 - handle inline */
                            /* The real Flash expects write to 0x0000 after B0 cmd */
                            /* We set a flag and catch it on next write */
                            mem->flash_state = 0xB0; /* special: bank select pending */
                        }
                        break;
                    default:
                        mem->flash_state = FLASH_STATE_READY;
                        break;
                }
            } else {
                mem->flash_state = FLASH_STATE_READY;
            }
            break;

        case FLASH_STATE_ERASE1:
            if (offset == 0x5555 && val == 0xAA) {
                mem->flash_state = FLASH_STATE_ERASE2;
            } else {
                mem->flash_state = FLASH_STATE_READY;
            }
            break;

        case FLASH_STATE_ERASE2:
            if (offset == 0x2AAA && val == 0x55) {
                mem->flash_state = FLASH_STATE_ERASE3;
            } else {
                mem->flash_state = FLASH_STATE_READY;
            }
            break;

        case FLASH_STATE_ERASE3:
            if (offset == 0x5555 && val == 0x10) {
                /* Erase entire chip */
                memset(mem->sram, 0xFF, mem->sram_size);
            } else if (val == 0x30) {
                /* Erase 4KB sector at offset */
                uint32_t sector = (uint32_t)mem->flash_bank * 0x10000 +
                                  (offset & 0xF000);
                if (sector + 0x1000 <= mem->sram_size)
                    memset(&mem->sram[sector], 0xFF, 0x1000);
            }
            mem->flash_state = FLASH_STATE_READY;
            break;

        case 0xB0: /* Bank select pending */
            if (offset == 0x0000) {
                mem->flash_bank = val & 1;
            }
            mem->flash_state = FLASH_STATE_READY;
            break;

        default:
            mem->flash_state = FLASH_STATE_READY;
            break;
    }

    /* Handle terminate ID mode from any state */
    if (val == 0xF0) {
        mem->flash_id_mode = false;
    }
}

/* ---------- Memory read ---------- */

uint8_t gba_mem_read8(void *ctx, uint32_t addr) {
    GbaMemory *mem = (GbaMemory *)ctx;
    uint32_t region = (addr >> 24) & 0xFF;

    switch (region) {
        case 0x00: /* BIOS */
            if (addr < 0x4000 && mem->bios_readable) {
                mem->bios_latch = mem->bios[addr];
                return mem->bios[addr];
            }
            return (uint8_t)mem->bios_latch;

        case 0x02: /* EWRAM */
            return mem->ewram[addr & 0x3FFFF];

        case 0x03: /* IWRAM */
            return mem->iwram[addr & 0x7FFF];

        case 0x04: /* IO */
            if ((addr & 0xFFFF) < 0x400) {
                uint32_t offset = addr & 0x3FF;
                uint16_t val16 = io_read16(mem, offset & ~1u);
                return (offset & 1) ? (val16 >> 8) : (val16 & 0xFF);
            }
            return 0;

        case 0x05: /* Palette */
            return mem->palette[addr & 0x3FF];

        case 0x06: { /* VRAM */
            uint32_t vaddr = addr & 0x1FFFF;
            if (vaddr >= 0x18000) vaddr -= 0x8000; /* Mirror */
            return mem->vram[vaddr];
        }

        case 0x07: /* OAM */
            return mem->oam[addr & 0x3FF];

        case 0x08: case 0x09: /* ROM Wait State 0 */
        case 0x0A: case 0x0B: /* ROM Wait State 1 */
        case 0x0C: case 0x0D: { /* ROM Wait State 2 */
            uint32_t rom_addr = addr & 0x01FFFFFF;
            if (rom_addr < mem->rom_size && mem->rom)
                return mem->rom[rom_addr];
            return (rom_addr >> 1) & 0xFF; /* Open bus */
        }

        case 0x0E: case 0x0F: /* SRAM / Flash */
            if (mem->flash_is_flash)
                return flash_read(mem, addr);
            if ((addr & 0xFFFF) < mem->sram_size)
                return mem->sram[addr & 0xFFFF];
            return 0xFF;

        default:
            return (uint8_t)mem->open_bus;
    }
}

uint16_t gba_mem_read16(void *ctx, uint32_t addr) {
    GbaMemory *mem = (GbaMemory *)ctx;
    addr &= ~1u;
    uint32_t region = (addr >> 24) & 0xFF;

    switch (region) {
        case 0x00:
            if (addr < 0x4000 && mem->bios_readable) {
                uint16_t v = *(uint16_t *)&mem->bios[addr];
                mem->bios_latch = v;
                return v;
            }
            return (uint16_t)mem->bios_latch;

        case 0x02: return *(uint16_t *)&mem->ewram[addr & 0x3FFFE];
        case 0x03: return *(uint16_t *)&mem->iwram[addr & 0x7FFE];
        case 0x04:
            if ((addr & 0xFFFF) < 0x400)
                return io_read16(mem, addr & 0x3FE);
            return 0;
        case 0x05: return *(uint16_t *)&mem->palette[addr & 0x3FE];
        case 0x06: {
            uint32_t vaddr = addr & 0x1FFFE;
            if (vaddr >= 0x18000) vaddr -= 0x8000;
            return *(uint16_t *)&mem->vram[vaddr];
        }
        case 0x07: return *(uint16_t *)&mem->oam[addr & 0x3FE];
        case 0x08: case 0x09:
        case 0x0A: case 0x0B:
        case 0x0C: case 0x0D: {
            uint32_t rom_addr = addr & 0x01FFFFFE;
            if (rom_addr < mem->rom_size && mem->rom)
                return *(uint16_t *)&mem->rom[rom_addr];
            return (rom_addr >> 1) & 0xFFFF;
        }
        case 0x0E: case 0x0F:
            return gba_mem_read8(ctx, addr) * 0x0101;
        default:
            return (uint16_t)mem->open_bus;
    }
}

uint32_t gba_mem_read32(void *ctx, uint32_t addr) {
    GbaMemory *mem = (GbaMemory *)ctx;
    addr &= ~3u;
    uint32_t region = (addr >> 24) & 0xFF;

    switch (region) {
        case 0x00:
            if (addr < 0x4000 && mem->bios_readable) {
                uint32_t v = *(uint32_t *)&mem->bios[addr];
                mem->bios_latch = v;
                return v;
            }
            return mem->bios_latch;

        case 0x02: return *(uint32_t *)&mem->ewram[addr & 0x3FFFC];
        case 0x03: return *(uint32_t *)&mem->iwram[addr & 0x7FFC];
        case 0x04:
            if ((addr & 0xFFFF) < 0x400) {
                uint32_t off = addr & 0x3FC;
                return (uint32_t)io_read16(mem, off) |
                       ((uint32_t)io_read16(mem, off + 2) << 16);
            }
            return 0;
        case 0x05: return *(uint32_t *)&mem->palette[addr & 0x3FC];
        case 0x06: {
            uint32_t vaddr = addr & 0x1FFFC;
            if (vaddr >= 0x18000) vaddr -= 0x8000;
            return *(uint32_t *)&mem->vram[vaddr];
        }
        case 0x07: return *(uint32_t *)&mem->oam[addr & 0x3FC];
        case 0x08: case 0x09:
        case 0x0A: case 0x0B:
        case 0x0C: case 0x0D: {
            uint32_t rom_addr = addr & 0x01FFFFFC;
            if (rom_addr < mem->rom_size && mem->rom)
                return *(uint32_t *)&mem->rom[rom_addr];
            uint16_t lo = (rom_addr >> 1) & 0xFFFF;
            uint16_t hi = ((rom_addr + 2) >> 1) & 0xFFFF;
            return (uint32_t)lo | ((uint32_t)hi << 16);
        }
        case 0x0E: case 0x0F: {
            uint8_t v = gba_mem_read8(ctx, addr);
            return (uint32_t)v * 0x01010101u;
        }
        default:
            return mem->open_bus;
    }
}

/* ---------- Memory write ---------- */

void gba_mem_write8(void *ctx, uint32_t addr, uint8_t val) {
    GbaMemory *mem = (GbaMemory *)ctx;
    uint32_t region = (addr >> 24) & 0xFF;

    switch (region) {
        case 0x02: mem->ewram[addr & 0x3FFFF] = val; return;
        case 0x03: mem->iwram[addr & 0x7FFF] = val; return;
        case 0x04:
            if ((addr & 0xFFFF) < 0x400)
                io_write8(mem, addr & 0x3FF, val);
            return;
        case 0x05: {
            /* 8-bit palette writes expand to 16-bit */
            uint32_t a = addr & 0x3FE;
            mem->palette[a] = val;
            mem->palette[a + 1] = val;
            return;
        }
        case 0x06: {
            /* 8-bit VRAM writes: expand in BG area, ignore in OBJ area */
            uint32_t vaddr = addr & 0x1FFFF;
            if (vaddr >= 0x18000) vaddr -= 0x8000;
            uint16_t dispcnt = *(uint16_t *)&mem->io[0];
            uint32_t obj_boundary = (dispcnt & 7) >= 3 ? 0x14000 : 0x10000;
            if (vaddr < obj_boundary) {
                vaddr &= ~1u;
                mem->vram[vaddr] = val;
                mem->vram[vaddr + 1] = val;
            }
            return;
        }
        case 0x07: return; /* OAM: 8-bit writes ignored */
        case 0x0E: case 0x0F:
            if (mem->flash_is_flash) {
                flash_write(mem, addr, val);
                return;
            }
            if ((addr & 0xFFFF) < mem->sram_size)
                mem->sram[addr & 0xFFFF] = val;
            return;
        default: return;
    }
}

void gba_mem_write16(void *ctx, uint32_t addr, uint16_t val) {
    GbaMemory *mem = (GbaMemory *)ctx;
    addr &= ~1u;
    uint32_t region = (addr >> 24) & 0xFF;

    switch (region) {
        case 0x02: *(uint16_t *)&mem->ewram[addr & 0x3FFFE] = val; return;
        case 0x03: *(uint16_t *)&mem->iwram[addr & 0x7FFE] = val; return;
        case 0x04:
            if ((addr & 0xFFFF) < 0x400)
                io_write16(mem, addr & 0x3FE, val);
            return;
        case 0x05: *(uint16_t *)&mem->palette[addr & 0x3FE] = val; return;
        case 0x06: {
            uint32_t vaddr = addr & 0x1FFFE;
            if (vaddr >= 0x18000) vaddr -= 0x8000;
            *(uint16_t *)&mem->vram[vaddr] = val;
            return;
        }
        case 0x07: *(uint16_t *)&mem->oam[addr & 0x3FE] = val; return;
        case 0x0E: case 0x0F:
            gba_mem_write8(ctx, addr, val & 0xFF);
            return;
        default: return;
    }
}

void gba_mem_write32(void *ctx, uint32_t addr, uint32_t val) {
    GbaMemory *mem = (GbaMemory *)ctx;
    addr &= ~3u;
    uint32_t region = (addr >> 24) & 0xFF;

    switch (region) {
        case 0x02: *(uint32_t *)&mem->ewram[addr & 0x3FFFC] = val; return;
        case 0x03: *(uint32_t *)&mem->iwram[addr & 0x7FFC] = val; return;
        case 0x04:
            if ((addr & 0xFFFF) < 0x400) {
                uint32_t off = addr & 0x3FC;
                /* FIFO writes must go as full 32-bit words to the APU */
                if (off == 0x0A0 && mem->apu) {
                    gba_fifo_write32(&mem->apu->fifo_a, val);
                    return;
                }
                if (off == 0x0A4 && mem->apu) {
                    gba_fifo_write32(&mem->apu->fifo_b, val);
                    return;
                }
                io_write16(mem, off, val & 0xFFFF);
                io_write16(mem, off + 2, val >> 16);
            }
            return;
        case 0x05: *(uint32_t *)&mem->palette[addr & 0x3FC] = val; return;
        case 0x06: {
            uint32_t vaddr = addr & 0x1FFFC;
            if (vaddr >= 0x18000) vaddr -= 0x8000;
            *(uint32_t *)&mem->vram[vaddr] = val;
            return;
        }
        case 0x07: *(uint32_t *)&mem->oam[addr & 0x3FC] = val; return;
        case 0x0E: case 0x0F:
            gba_mem_write8(ctx, addr, val & 0xFF);
            return;
        default: return;
    }
}

/* ---------- DMA ---------- */

static void dma_run_channel(GbaMemory *mem, int ch) {
    GbaDMA *dma = &mem->dma[ch];
    if (!dma->active) return;

    uint16_t ctrl = dma->control;
    uint8_t dst_ctrl = (ctrl >> 5) & 3;
    uint8_t src_ctrl = (ctrl >> 7) & 3;
    bool word_size = (ctrl >> 10) & 1; /* 0=16bit, 1=32bit */
    uint8_t timing = (ctrl >> 12) & 3;
    uint32_t step = word_size ? 4 : 2;

    /* Sound FIFO DMA (DMA1/2 with SPECIAL timing): always 4 words, 32-bit */
    uint32_t count = dma->internal_count;
    if (timing == DMA_TIMING_SPECIAL && (ch == 1 || ch == 2)) {
        count = 4;
        word_size = true;
        step = 4;
    }

    int32_t src_inc = (src_ctrl == DMA_SRC_DEC) ? -(int32_t)step :
                      (src_ctrl == DMA_SRC_FIXED) ? 0 : (int32_t)step;
    int32_t dst_inc = (dst_ctrl == DMA_DEST_DEC) ? -(int32_t)step :
                      (dst_ctrl == DMA_DEST_FIXED) ? 0 : (int32_t)step;

    for (uint32_t i = 0; i < count; i++) {
        if (word_size) {
            uint32_t val = gba_mem_read32(mem, dma->internal_src);
            gba_mem_write32(mem, dma->internal_dst, val);
        } else {
            uint16_t val = gba_mem_read16(mem, dma->internal_src);
            gba_mem_write16(mem, dma->internal_dst, val);
        }
        dma->internal_src += src_inc;
        dma->internal_dst += dst_inc;
    }

    /* IRQ on completion */
    if (ctrl & (1 << 14)) {
        gba_request_interrupt(mem, GBA_INT_DMA0 << ch);
    }

    /* Repeat? */
    bool repeat = (ctrl >> 9) & 1;
    if (repeat && timing != DMA_TIMING_NOW) {
        dma->internal_count = dma->count ? dma->count : (ch == 3 ? 0x10000 : 0x4000);
        if (dst_ctrl == DMA_DEST_RELOAD) {
            dma->internal_dst = dma->dst & (ch == 3 ? 0x0FFFFFFF : 0x07FFFFFF);
        }
        dma->active = false; /* Wait for next trigger */
    } else {
        dma->control &= ~(1 << 15); /* Disable */
        dma->active = false;
    }
}

void gba_dma_trigger(GbaMemory *mem, int timing) {
    for (int ch = 0; ch < 4; ch++) {
        GbaDMA *dma = &mem->dma[ch];
        if (!(dma->control & (1 << 15))) continue;
        uint8_t dma_timing = (dma->control >> 12) & 3;
        if (dma_timing == timing) {
            dma->active = true;
            dma_run_channel(mem, ch);
        }
    }
}

void gba_dma_step(GbaMemory *mem) {
    for (int ch = 0; ch < 4; ch++) {
        if (mem->dma[ch].active) {
            dma_run_channel(mem, ch);
        }
    }
}

/* ---------- Timers ---------- */

void gba_timer_step(GbaMemory *mem, uint32_t cycles) {
    for (int i = 0; i < 4; i++) {
        GbaTimer *t = &mem->timers[i];
        t->overflow = false;

        if (!(t->control & TIMER_ENABLE)) continue;

        if (t->control & TIMER_CASCADE) {
            /* Cascade: increment on previous timer overflow */
            if (i > 0 && mem->timers[i - 1].overflow) {
                uint32_t old = t->counter;
                t->counter++;
                if (t->counter < old || t->counter == 0) {
                    t->overflow = true;
                    t->counter = t->reload;
                    if (t->control & TIMER_IRQ) {
                        gba_request_interrupt(mem, GBA_INT_TIMER0 << i);
                    }
                    /* Notify APU for DMA sound */
                    if (mem->apu) gba_apu_timer_overflow(mem->apu, i);
                }
            }
            continue;
        }

        uint16_t prescaler_val = gba_timer_prescalers[t->control & 3];
        t->prescaler += cycles;

        while (t->prescaler >= prescaler_val) {
            t->prescaler -= prescaler_val;
            uint16_t old = t->counter;
            t->counter++;
            if (t->counter == 0) { /* Overflow */
                t->overflow = true;
                t->counter = t->reload;
                if (t->control & TIMER_IRQ) {
                    gba_request_interrupt(mem, GBA_INT_TIMER0 << i);
                }
                /* Notify APU for DMA sound */
                if (mem->apu) gba_apu_timer_overflow(mem->apu, i);
            }
            (void)old;
        }
    }
}

/* ---------- Interrupts ---------- */

void gba_request_interrupt(GbaMemory *mem, uint16_t flag) {
    static int vblank_irq_trace = 0;
    static uint64_t last_vblank_irq_cycle = 0;
    if ((flag & GBA_INT_VBLANK) && mem->cpu && vblank_irq_trace < 20) {
        uint64_t now = mem->cpu->total_cycles;
        uint64_t delta = (last_vblank_irq_cycle == 0) ? 0 : (now - last_vblank_irq_cycle);
        fprintf(stderr, "[VBL_IRQ] cycle=%llu delta=%llu IE=0x%04X IF=0x%04X IME=%d\n",
            (unsigned long long)now, (unsigned long long)delta,
            mem->ie, mem->ifl, mem->ime);
        last_vblank_irq_cycle = now;
        vblank_irq_trace++;
    }
    mem->ifl |= flag;

    /* NOTE: BIOS IF at 0x03007FF8 is updated by the BIOS IRQ stub
     * (ARM code at 0x80), not here. The stub reads IE & IF, ORs the
     * matched bits into [0x03FFFFF8], then calls the user handler.
     * Doing it here prematurely would wake IntrWait before the IRQ
     * handler has run. */

    if (mem->ime && (mem->ie & mem->ifl)) {
        if (mem->cpu) {
            arm7_request_irq(mem->cpu);
            mem->cpu->halted = false;
        }
    }
}

/* ---------- Serialization ---------- */

size_t gba_mem_serialize(const GbaMemory *mem, uint8_t *buf, size_t buf_size) {
    size_t needed = sizeof(mem->ewram) + sizeof(mem->iwram) + sizeof(mem->io)
                  + sizeof(mem->palette) + sizeof(mem->vram) + sizeof(mem->oam)
                  + sizeof(mem->sram) + sizeof(mem->dma) + sizeof(mem->timers)
                  + 32;
    if (!buf || buf_size < needed) return needed;

    size_t pos = 0;
    #define S(f) memcpy(buf+pos, mem->f, sizeof(mem->f)); pos += sizeof(mem->f)
    S(ewram); S(iwram); S(io); S(palette); S(vram); S(oam); S(sram);
    S(dma); S(timers);
    #undef S
    #define S2(f) memcpy(buf+pos, &mem->f, sizeof(mem->f)); pos += sizeof(mem->f)
    S2(ie); S2(ifl); S2(ime); S2(keyinput); S2(keycnt);
    S2(waitcnt); S2(postflg); S2(bios_latch);
    #undef S2
    return pos;
}

size_t gba_mem_deserialize(GbaMemory *mem, const uint8_t *buf, size_t buf_size) {
    size_t pos = 0;
    #define L(f) do { if(pos+sizeof(mem->f)>buf_size) return 0; \
        memcpy(mem->f, buf+pos, sizeof(mem->f)); pos += sizeof(mem->f); } while(0)
    L(ewram); L(iwram); L(io); L(palette); L(vram); L(oam); L(sram);
    L(dma); L(timers);
    #undef L
    #define L2(f) do { if(pos+sizeof(mem->f)>buf_size) return 0; \
        memcpy(&mem->f, buf+pos, sizeof(mem->f)); pos += sizeof(mem->f); } while(0)
    L2(ie); L2(ifl); L2(ime); L2(keyinput); L2(keycnt);
    L2(waitcnt); L2(postflg); L2(bios_latch);
    #undef L2
    return pos;
}
