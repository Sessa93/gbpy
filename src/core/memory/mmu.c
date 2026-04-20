/*
 * MMU Implementation - Game Boy / Game Boy Color Memory Management
 *
 * Reference: Game Boy CPU Manual v1.01 (resources/gba-manual.md)
 * Section 2.5: General Memory Map
 *
 * Memory Map (Section 2.5.1):
 *   0000-3FFF   16KB ROM Bank 00 (in cartridge, fixed)
 *   4000-7FFF   16KB ROM Bank 01..NN (switchable via MBC)
 *   8000-9FFF   8KB Video RAM (VRAM)
 *   A000-BFFF   8KB External RAM (cartridge, switchable)
 *   C000-CFFF   4KB Work RAM Bank 0
 *   D000-DFFF   4KB Work RAM Bank 1 (switchable 1-7 in GBC)
 *   E000-FDFF   Echo of C000-DDFF
 *   FE00-FE9F   Sprite Attribute Table (OAM)
 *   FEA0-FEFF   Not Usable
 *   FF00-FF7F   I/O Ports
 *   FF80-FFFE   High RAM (HRAM)
 *   FFFF        Interrupt Enable Register
 */

#include "mmu.h"
#include "mbc.h"
#include "gb_ppu.h"
#include "input.h"
#include "../audio/apu.h"

/*
 * I/O Register Addresses (Section 2.13.1)
 */
#define IO_P1       0x00  /* Joypad */
#define IO_SB       0x01  /* Serial transfer data */
#define IO_SC       0x02  /* Serial transfer control */
#define IO_DIV      0x04  /* Divider Register */
#define IO_TIMA     0x05  /* Timer counter */
#define IO_TMA      0x06  /* Timer Modulo */
#define IO_TAC      0x07  /* Timer Control */
#define IO_IF       0x0F  /* Interrupt Flag */
#define IO_NR10     0x10  /* Sound Channel 1 Sweep */
#define IO_NR11     0x11  /* Sound Channel 1 Length/Wave */
#define IO_NR12     0x12  /* Sound Channel 1 Volume Envelope */
#define IO_NR13     0x13  /* Sound Channel 1 Frequency lo */
#define IO_NR14     0x14  /* Sound Channel 1 Frequency hi */
#define IO_NR21     0x16  /* Sound Channel 2 Length/Wave */
#define IO_NR22     0x17  /* Sound Channel 2 Volume Envelope */
#define IO_NR23     0x18  /* Sound Channel 2 Frequency lo */
#define IO_NR24     0x19  /* Sound Channel 2 Frequency hi */
#define IO_NR30     0x1A  /* Sound Channel 3 On/Off */
#define IO_NR31     0x1B  /* Sound Channel 3 Length */
#define IO_NR32     0x1C  /* Sound Channel 3 Output Level */
#define IO_NR33     0x1D  /* Sound Channel 3 Frequency lo */
#define IO_NR34     0x1E  /* Sound Channel 3 Frequency hi */
#define IO_NR41     0x20  /* Sound Channel 4 Length */
#define IO_NR42     0x21  /* Sound Channel 4 Volume Envelope */
#define IO_NR43     0x22  /* Sound Channel 4 Polynomial Counter */
#define IO_NR44     0x23  /* Sound Channel 4 Counter/Consecutive */
#define IO_NR50     0x24  /* Channel control / ON-OFF / Volume */
#define IO_NR51     0x25  /* Selection of Sound output terminal */
#define IO_NR52     0x26  /* Sound on/off */
#define IO_LCDC     0x40  /* LCD Control */
#define IO_STAT     0x41  /* LCD Status */
#define IO_SCY      0x42  /* Scroll Y */
#define IO_SCX      0x43  /* Scroll X */
#define IO_LY       0x44  /* LCDC Y-Coordinate */
#define IO_LYC      0x45  /* LY Compare */
#define IO_DMA      0x46  /* DMA Transfer */
#define IO_BGP      0x47  /* BG Palette Data (non-GBC) */
#define IO_OBP0     0x48  /* Object Palette 0 Data */
#define IO_OBP1     0x49  /* Object Palette 1 Data */
#define IO_WY       0x4A  /* Window Y Position */
#define IO_WX       0x4B  /* Window X Position */
#define IO_KEY1     0x4D  /* GBC: Speed Switch */
#define IO_VBK      0x4F  /* GBC: VRAM Bank */
#define IO_HDMA1    0x51  /* GBC: HDMA Source High */
#define IO_HDMA2    0x52  /* GBC: HDMA Source Low */
#define IO_HDMA3    0x53  /* GBC: HDMA Dest High */
#define IO_HDMA4    0x54  /* GBC: HDMA Dest Low */
#define IO_HDMA5    0x55  /* GBC: HDMA Length/Mode/Start */
#define IO_BCPS     0x68  /* GBC: BG Palette Index */
#define IO_BCPD     0x69  /* GBC: BG Palette Data */
#define IO_OCPS     0x6A  /* GBC: OBJ Palette Index */
#define IO_OCPD     0x6B  /* GBC: OBJ Palette Data */
#define IO_SVBK     0x70  /* GBC: WRAM Bank */

void mmu_init(MMU *mmu, GbpyMode mode) {
    memset(mmu, 0, sizeof(MMU));
    mmu->mode = mode;
    mmu->wram_bank = 1; /* WRAM bank 0 is always at C000, bank 1+ at D000 */
    mmu->vram_bank = 0;

    /*
     * Post-boot IO register values (Section 2.7.1 Power Up Sequence).
     * When skipping the boot ROM (PC starts at 0x100), hardware registers
     * must be set to their post-boot values.
     */
    mmu->io[IO_P1]   = 0xCF; /* Joypad */
    mmu->io[IO_SB]   = 0x00;
    mmu->io[IO_SC]   = 0x7E;
    mmu->io[IO_DIV]  = 0xAB;
    mmu->io[IO_TIMA] = 0x00;
    mmu->io[IO_TMA]  = 0x00;
    mmu->io[IO_TAC]  = 0xF8;
    mmu->io[IO_IF]   = 0xE1;
    /* Sound registers */
    mmu->io[IO_NR10] = 0x80;
    mmu->io[IO_NR11] = 0xBF;
    mmu->io[IO_NR12] = 0xF3;
    mmu->io[IO_NR13] = 0xFF;
    mmu->io[IO_NR14] = 0xBF;
    mmu->io[IO_NR21] = 0x3F;
    mmu->io[IO_NR22] = 0x00;
    mmu->io[IO_NR23] = 0xFF;
    mmu->io[IO_NR24] = 0xBF;
    mmu->io[IO_NR30] = 0x7F;
    mmu->io[IO_NR31] = 0xFF;
    mmu->io[IO_NR32] = 0x9F;
    mmu->io[IO_NR33] = 0xFF;
    mmu->io[IO_NR34] = 0xBF;
    mmu->io[IO_NR41] = 0xFF;
    mmu->io[IO_NR42] = 0x00;
    mmu->io[IO_NR43] = 0x00;
    mmu->io[IO_NR44] = 0xBF;
    mmu->io[IO_NR50] = 0x77;
    mmu->io[IO_NR51] = 0xF3;
    mmu->io[IO_NR52] = 0xF1;
    /* LCD registers */
    mmu->io[IO_LCDC] = 0x91; /* LCD on, BG on, BG tile data at 0x8000 */
    mmu->io[IO_STAT] = 0x85;
    mmu->io[IO_SCY]  = 0x00;
    mmu->io[IO_SCX]  = 0x00;
    mmu->io[IO_LY]   = 0x00;
    mmu->io[IO_LYC]  = 0x00;
    mmu->io[IO_BGP]  = 0xFC; /* Default BG palette: 11 10 00 00 */
    mmu->io[IO_OBP0] = 0xFF;
    mmu->io[IO_OBP1] = 0xFF;
    mmu->io[IO_WY]   = 0x00;
    mmu->io[IO_WX]   = 0x00;
    mmu->ie = 0x00;
}

void mmu_destroy(MMU *mmu) {
    if (mmu->eram) {
        free(mmu->eram);
        mmu->eram = NULL;
    }
}

GbpyResult mmu_load_rom(MMU *mmu, const uint8_t *rom, size_t rom_size) {
    mmu->rom = rom;
    mmu->rom_size = rom_size;
    return GBPY_OK;
}

/*
 * Memory Read - Section 2.5
 */
uint8_t mmu_read(void *ctx, uint16_t addr) {
    MMU *mmu = (MMU *)ctx;

    /* 0000-7FFF: ROM (via MBC) */
    if (addr < 0x8000) {
        if (mmu->mbc) {
            return mbc_read_rom(mmu->mbc, addr);
        }
        if (addr < mmu->rom_size) {
            return mmu->rom[addr];
        }
        return 0xFF;
    }

    /* 8000-9FFF: VRAM */
    if (addr < 0xA000) {
        uint16_t offset = addr - 0x8000;
        if (mmu->mode == GBPY_MODE_GBC) {
            return mmu->vram[mmu->vram_bank * 0x2000 + offset];
        }
        return mmu->vram[offset];
    }

    /* A000-BFFF: External RAM (via MBC) */
    if (addr < 0xC000) {
        if (mmu->mbc) {
            return mbc_read_ram(mmu->mbc, addr);
        }
        return 0xFF;
    }

    /* C000-CFFF: WRAM Bank 0 */
    if (addr < 0xD000) {
        return mmu->wram[addr - 0xC000];
    }

    /* D000-DFFF: WRAM Bank 1 (or switchable 1-7 in GBC) */
    if (addr < 0xE000) {
        if (mmu->mode == GBPY_MODE_GBC) {
            uint32_t offset = (uint32_t)mmu->wram_bank * 0x1000 + (addr - 0xD000);
            return mmu->wram[offset];
        }
        return mmu->wram[addr - 0xC000];
    }

    /* E000-FDFF: Echo RAM (mirror of C000-DDFF) */
    if (addr < 0xFE00) {
        return mmu_read(ctx, addr - 0x2000);
    }

    /* FE00-FE9F: OAM */
    if (addr < 0xFEA0) {
        return mmu->oam[addr - 0xFE00];
    }

    /* FEA0-FEFF: Not Usable */
    if (addr < 0xFF00) {
        return 0xFF;
    }

    /* FF00-FF7F: I/O Registers */
    if (addr < 0xFF80) {
        uint8_t reg = addr & 0x7F;

        switch (reg) {
            case IO_P1:
                /* Joypad register: combine select bits with button state */
                if (mmu->input) {
                    return input_read(mmu->input);
                }
                return mmu->io[reg];

            case IO_BCPS:
            case IO_BCPD:
            case IO_OCPS:
            case IO_OCPD:
                /* GBC palette registers: delegate to PPU */
                if (mmu->mode == GBPY_MODE_GBC && mmu->ppu) {
                    return gb_ppu_read_palette(mmu->ppu, addr);
                }
                return mmu->io[reg];

            default:
                /* Sound registers: NR10-NR52 (0x10-0x26) and Wave RAM (0x30-0x3F) */
                if (mmu->apu && ((reg >= 0x10 && reg <= 0x26) || (reg >= 0x30 && reg <= 0x3F))) {
                    return apu_read(mmu->apu, addr);
                }
                return mmu->io[reg];
        }
    }

    /* FF80-FFFE: HRAM */
    if (addr < 0xFFFF) {
        return mmu->hram[addr - 0xFF80];
    }

    /* FFFF: Interrupt Enable */
    return mmu->ie;
}

/*
 * Memory Write - Section 2.5
 */
void mmu_write(void *ctx, uint16_t addr, uint8_t val) {
    MMU *mmu = (MMU *)ctx;

    /* 0000-7FFF: ROM area writes go to MBC registers */
    if (addr < 0x8000) {
        if (mmu->mbc) {
            mbc_write(mmu->mbc, addr, val);
        }
        return;
    }

    /* 8000-9FFF: VRAM */
    if (addr < 0xA000) {
        uint16_t offset = addr - 0x8000;
        if (mmu->mode == GBPY_MODE_GBC) {
            mmu->vram[mmu->vram_bank * 0x2000 + offset] = val;
        } else {
            mmu->vram[offset] = val;
        }
        return;
    }

    /* A000-BFFF: External RAM */
    if (addr < 0xC000) {
        if (mmu->mbc) {
            mbc_write_ram(mmu->mbc, addr, val);
        }
        return;
    }

    /* C000-CFFF: WRAM Bank 0 */
    if (addr < 0xD000) {
        mmu->wram[addr - 0xC000] = val;
        return;
    }

    /* D000-DFFF: WRAM Bank 1+ */
    if (addr < 0xE000) {
        if (mmu->mode == GBPY_MODE_GBC) {
            uint32_t offset = (uint32_t)mmu->wram_bank * 0x1000 + (addr - 0xD000);
            mmu->wram[offset] = val;
        } else {
            mmu->wram[addr - 0xC000] = val;
        }
        return;
    }

    /* E000-FDFF: Echo RAM */
    if (addr < 0xFE00) {
        mmu_write(ctx, addr - 0x2000, val);
        return;
    }

    /* FE00-FE9F: OAM */
    if (addr < 0xFEA0) {
        mmu->oam[addr - 0xFE00] = val;
        return;
    }

    /* FEA0-FEFF: Not Usable */
    if (addr < 0xFF00) {
        return;
    }

    /* FF00-FF7F: I/O Registers */
    if (addr < 0xFF80) {
        uint8_t reg = addr & 0x7F;

        switch (reg) {
            case IO_DMA: {
                /* OAM DMA Transfer (Section 2.13.1 #37)
                 * Source: val * 0x100 to val * 0x100 + 0x9F
                 * Dest: FE00-FE9F */
                uint16_t src = (uint16_t)val << 8;
                for (int i = 0; i < 0xA0; i++) {
                    mmu->oam[i] = mmu_read(ctx, src + i);
                }
                mmu->io[reg] = val;
                break;
            }

            case IO_VBK:
                /* GBC VRAM bank select */
                if (mmu->mode == GBPY_MODE_GBC) {
                    mmu->vram_bank = val & 0x01;
                }
                mmu->io[reg] = val;
                break;

            case IO_SVBK:
                /* GBC WRAM bank select */
                if (mmu->mode == GBPY_MODE_GBC) {
                    mmu->wram_bank = val & 0x07;
                    if (mmu->wram_bank == 0) mmu->wram_bank = 1;
                }
                mmu->io[reg] = val;
                break;

            case IO_P1:
                /* Joypad register: write select bits */
                if (mmu->input) {
                    input_write(mmu->input, val);
                }
                mmu->io[reg] = val;
                break;

            case IO_DIV:
                /* Writing any value to DIV resets it to 0 (Section 2.10.1) */
                mmu->io[reg] = 0;
                break;

            case IO_BCPS:
            case IO_BCPD:
            case IO_OCPS:
            case IO_OCPD:
                /* GBC palette registers: delegate to PPU */
                if (mmu->mode == GBPY_MODE_GBC && mmu->ppu) {
                    gb_ppu_write_palette(mmu->ppu, addr, val);
                }
                mmu->io[reg] = val;
                break;

            default:
                /* Sound registers: NR10-NR52 (0x10-0x26) and Wave RAM (0x30-0x3F) */
                if (mmu->apu && ((reg >= 0x10 && reg <= 0x26) || (reg >= 0x30 && reg <= 0x3F))) {
                    apu_write(mmu->apu, addr, val);
                }
                mmu->io[reg] = val;
                break;
        }
        return;
    }

    /* FF80-FFFE: HRAM */
    if (addr < 0xFFFF) {
        mmu->hram[addr - 0xFF80] = val;
        return;
    }

    /* FFFF: Interrupt Enable */
    mmu->ie = val;
}

/* DMA tick for cycle-accurate OAM DMA */
void mmu_dma_tick(MMU *mmu) {
    if (!mmu->dma_active) return;

    if (mmu->dma_offset < 0xA0) {
        uint16_t src = mmu->dma_source + mmu->dma_offset;
        mmu->oam[mmu->dma_offset] = mmu_read(mmu, src);
        mmu->dma_offset++;
    }

    if (mmu->dma_offset >= 0xA0) {
        mmu->dma_active = false;
    }
}

/* ---------- Serialization ---------- */

size_t mmu_serialize(const MMU *mmu, uint8_t *buf, size_t buf_size) {
    size_t needed = sizeof(mmu->vram) + sizeof(mmu->wram) + sizeof(mmu->oam)
                  + sizeof(mmu->hram) + sizeof(mmu->io) + sizeof(mmu->ie)
                  + sizeof(mmu->vram_bank) + sizeof(mmu->wram_bank) + 16;
    if (!buf || buf_size < needed) return needed;

    size_t pos = 0;
    #define SAVE_BUF(data, sz) memcpy(buf + pos, data, sz); pos += sz
    SAVE_BUF(mmu->vram, sizeof(mmu->vram));
    SAVE_BUF(mmu->wram, sizeof(mmu->wram));
    SAVE_BUF(mmu->oam, sizeof(mmu->oam));
    SAVE_BUF(mmu->hram, sizeof(mmu->hram));
    SAVE_BUF(mmu->io, sizeof(mmu->io));
    SAVE_BUF(&mmu->ie, sizeof(mmu->ie));
    SAVE_BUF(&mmu->vram_bank, sizeof(mmu->vram_bank));
    SAVE_BUF(&mmu->wram_bank, sizeof(mmu->wram_bank));
    #undef SAVE_BUF
    return pos;
}

size_t mmu_deserialize(MMU *mmu, const uint8_t *buf, size_t buf_size) {
    size_t pos = 0;
    #define LOAD_BUF(data, sz) do { \
        if (pos + sz > buf_size) return 0; \
        memcpy(data, buf + pos, sz); pos += sz; \
    } while(0)
    LOAD_BUF(mmu->vram, sizeof(mmu->vram));
    LOAD_BUF(mmu->wram, sizeof(mmu->wram));
    LOAD_BUF(mmu->oam, sizeof(mmu->oam));
    LOAD_BUF(mmu->hram, sizeof(mmu->hram));
    LOAD_BUF(mmu->io, sizeof(mmu->io));
    LOAD_BUF(&mmu->ie, sizeof(mmu->ie));
    LOAD_BUF(&mmu->vram_bank, sizeof(mmu->vram_bank));
    LOAD_BUF(&mmu->wram_bank, sizeof(mmu->wram_bank));
    #undef LOAD_BUF
    return pos;
}
