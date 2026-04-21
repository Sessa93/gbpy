/*
 * Memory Management Unit (MMU) - Game Boy / Game Boy Color
 *
 * Reference: Game Boy CPU Manual v1.01 (resources/gba-manual.md)
 *
 * Section 2.5: Memory Map
 *   FFFF         - Interrupt Enable Register
 *   FF80-FFFE    - High RAM (HRAM)
 *   FF00-FF7F    - I/O Ports
 *   FEA0-FEFF    - Not usable
 *   FE00-FE9F    - Sprite Attribute Table (OAM)
 *   E000-FDFF    - Echo of 8kB Internal RAM
 *   C000-DFFF    - 8kB Internal RAM
 *   A000-BFFF    - 8kB Switchable RAM bank (cartridge)
 *   8000-9FFF    - 8kB Video RAM
 *   4000-7FFF    - 16kB Switchable ROM bank
 *   0000-3FFF    - 16kB ROM bank #0
 */

#ifndef GBPY_MMU_H
#define GBPY_MMU_H

#include "types.h"

/* Forward declarations */
typedef struct MBC MBC;
typedef struct GbPPU GbPPU;
typedef struct APU APU;
typedef struct GbTimer GbTimer;
typedef struct GbInput GbInput;

typedef struct MMU {
    /* ROM data (owned by cartridge, not freed by MMU) */
    const uint8_t *rom;
    size_t rom_size;

    /* Memory regions (Section 2.5.1) */
    uint8_t vram[0x4000];     /* Video RAM: 8KB (GB) or 16KB (GBC, 2 banks) */
    uint8_t wram[0x8000];     /* Work RAM: 8KB (GB) or 32KB (GBC, 8 banks) */
    uint8_t oam[GB_OAM_SIZE]; /* Sprite Attribute Memory */
    uint8_t hram[GB_HRAM_SIZE]; /* High RAM */
    uint8_t io[GB_IO_SIZE];   /* I/O Registers */
    uint8_t ie;               /* Interrupt Enable register (FFFF) */

    /* External RAM (cartridge battery-backed) */
    uint8_t *eram;
    size_t eram_size;

    /* GBC-specific */
    uint8_t vram_bank;        /* Current VRAM bank (0 or 1) */
    uint8_t wram_bank;        /* Current WRAM bank (1-7) */

    /* Memory Bank Controller */
    MBC *mbc;

    /* Subsystem references for I/O register reads/writes */
    GbPPU *ppu;
    APU *apu;
    GbTimer *timer;
    GbInput *input;

    /* Mode */
    GbpyMode mode;

    /* DMA state (Section 2.13.1 #37 - FF46) */
    bool dma_active;
    uint16_t dma_source;
    uint8_t dma_offset;

    /* Serial output capture (for test ROMs) */
    char serial_buf[4096];
    uint32_t serial_pos;
} MMU;

/* Initialize/destroy */
void mmu_init(MMU *mmu, GbpyMode mode);
void mmu_destroy(MMU *mmu);

/* Load ROM into MMU */
GbpyResult mmu_load_rom(MMU *mmu, const uint8_t *rom, size_t rom_size);

/* Memory access (used as CPU callbacks) */
uint8_t mmu_read(void *ctx, uint16_t addr);
void mmu_write(void *ctx, uint16_t addr, uint8_t val);

/* DMA transfer tick */
void mmu_dma_tick(MMU *mmu);

/* State serialization */
size_t mmu_serialize(const MMU *mmu, uint8_t *buf, size_t buf_size);
size_t mmu_deserialize(MMU *mmu, const uint8_t *buf, size_t buf_size);

#endif /* GBPY_MMU_H */
