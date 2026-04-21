/*
 * GBA Memory Map Header
 *
 * Defines the GBA memory subsystem with all regions, DMA channels,
 * timer integration, and IO register access.
 */

#ifndef GBPY_GBA_MEMORY_H
#define GBPY_GBA_MEMORY_H

#include "../types.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* Forward declarations */
typedef struct ARM7TDMI ARM7TDMI;
typedef struct GbaPPU GbaPPU;
typedef struct GbaAPU GbaAPU;

/* ---------- DMA Channel ---------- */

typedef struct {
    uint32_t src;
    uint32_t dst;
    uint16_t count;
    uint16_t control;
    /* Internal latched values */
    uint32_t internal_src;
    uint32_t internal_dst;
    uint32_t internal_count;
    bool active;
} GbaDMA;

#define DMA_DEST_INC      0
#define DMA_DEST_DEC      1
#define DMA_DEST_FIXED    2
#define DMA_DEST_RELOAD   3
#define DMA_SRC_INC       0
#define DMA_SRC_DEC       1
#define DMA_SRC_FIXED     2

#define DMA_TIMING_NOW      0
#define DMA_TIMING_VBLANK   1
#define DMA_TIMING_HBLANK   2
#define DMA_TIMING_SPECIAL  3  /* Sound FIFO for DMA1/2, Video Capture for DMA3 */

/* ---------- Timer ---------- */

typedef struct {
    uint16_t counter;     /* Current counter value */
    uint16_t reload;      /* Reload value (TM0CNT_L) */
    uint16_t control;     /* TM0CNT_H */
    uint32_t prescaler;   /* Internal prescaler counter */
    bool overflow;        /* Did this timer overflow this step? */
} GbaTimer;

/* Timer prescaler values: 1, 64, 256, 1024 */
static const uint16_t gba_timer_prescalers[4] = { 1, 64, 256, 1024 };

#define TIMER_ENABLE    (1 << 7)
#define TIMER_IRQ       (1 << 6)
#define TIMER_CASCADE   (1 << 2)

/* ---------- GBA Memory ---------- */

typedef struct GbaMemory {
    /* Main memory regions */
    uint8_t bios[0x4000];          /* 16KB BIOS (0x00000000-0x00003FFF) */
    uint8_t ewram[0x40000];        /* 256KB external WRAM (0x02000000-0x0203FFFF) */
    uint8_t iwram[0x8000];         /* 32KB internal WRAM (0x03000000-0x03007FFF) */
    uint8_t io[0x400];             /* 1KB IO registers (0x04000000-0x040003FF) */
    uint8_t palette[0x400];        /* 1KB palette RAM (0x05000000-0x050003FF) */
    uint8_t vram[0x18000];         /* 96KB VRAM (0x06000000-0x06017FFF) */
    uint8_t oam[0x400];            /* 1KB OAM (0x07000000-0x070003FF) */

    /* Cartridge */
    uint8_t *rom;                  /* ROM data pointer */
    uint32_t rom_size;             /* ROM size in bytes */
    uint8_t sram[0x20000];         /* 128KB SRAM/Flash */
    uint32_t sram_size;

    /* Flash emulation state machine */
    uint8_t flash_state;           /* Current Flash command state */
    uint8_t flash_bank;            /* Bank select for 128KB Flash (0 or 1) */
    bool flash_id_mode;            /* In chip identification mode */
    bool flash_write_enable;       /* Single byte write pending */
    bool flash_is_flash;           /* Save type is Flash (vs plain SRAM) */
    uint8_t flash_manufacturer;    /* Manufacturer ID (e.g. 0x62 for Sanyo) */
    uint8_t flash_device;          /* Device ID (e.g. 0x13 for Sanyo 128KB) */

    /* DMA channels */
    GbaDMA dma[4];

    /* Timers */
    GbaTimer timers[4];

    /* Interrupt registers */
    uint16_t ie;       /* Interrupt Enable (0x4000200) */
    uint16_t ifl;      /* Interrupt Request Flags (0x4000202) */
    uint16_t ime;      /* Interrupt Master Enable (0x4000208) */
    uint8_t  postflg;  /* Post Boot Flag */
    uint8_t  haltcnt;  /* Halt/Stop control */

    /* Wait state control */
    uint16_t waitcnt;

    /* Key input */
    uint16_t keyinput;    /* KEYINPUT register (0x4000130) - active low */
    uint16_t keycnt;      /* KEYCNT register (0x4000132) */

    /* Open bus last value */
    uint32_t open_bus;

    /* BIOS protection: last BIOS read value */
    uint32_t bios_latch;
    bool bios_readable;

    /* Subsystem pointers */
    ARM7TDMI *cpu;
    GbaPPU   *ppu;
    GbaAPU   *apu;
} GbaMemory;

/* ---------- API ---------- */

void gba_mem_init(GbaMemory *mem);
void gba_mem_reset(GbaMemory *mem);

/* CPU-facing memory access */
uint8_t  gba_mem_read8(void *ctx, uint32_t addr);
uint16_t gba_mem_read16(void *ctx, uint32_t addr);
uint32_t gba_mem_read32(void *ctx, uint32_t addr);
void     gba_mem_write8(void *ctx, uint32_t addr, uint8_t val);
void     gba_mem_write16(void *ctx, uint32_t addr, uint16_t val);
void     gba_mem_write32(void *ctx, uint32_t addr, uint32_t val);

/* DMA */
void gba_dma_trigger(GbaMemory *mem, int timing);
void gba_dma_step(GbaMemory *mem);

/* Timers */
void gba_timer_step(GbaMemory *mem, uint32_t cycles);

/* Interrupt helpers */
void gba_request_interrupt(GbaMemory *mem, uint16_t flag);

/* Serialization */
size_t gba_mem_serialize(const GbaMemory *mem, uint8_t *buf, size_t buf_size);
size_t gba_mem_deserialize(GbaMemory *mem, const uint8_t *buf, size_t buf_size);

/* GBA interrupt flags */
#define GBA_INT_VBLANK   (1 << 0)
#define GBA_INT_HBLANK   (1 << 1)
#define GBA_INT_VCOUNT   (1 << 2)
#define GBA_INT_TIMER0   (1 << 3)
#define GBA_INT_TIMER1   (1 << 4)
#define GBA_INT_TIMER2   (1 << 5)
#define GBA_INT_TIMER3   (1 << 6)
#define GBA_INT_SERIAL   (1 << 7)
#define GBA_INT_DMA0     (1 << 8)
#define GBA_INT_DMA1     (1 << 9)
#define GBA_INT_DMA2     (1 << 10)
#define GBA_INT_DMA3     (1 << 11)
#define GBA_INT_KEYPAD   (1 << 12)
#define GBA_INT_GAMEPAK  (1 << 13)

#endif /* GBPY_GBA_MEMORY_H */
