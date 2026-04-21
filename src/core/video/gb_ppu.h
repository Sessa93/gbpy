/*
 * Game Boy / Game Boy Color PPU (Pixel Processing Unit)
 *
 * Reference: Game Boy CPU Manual v1.01 (resources/gba-manual.md)
 *
 * Section 2.8: Video (Tiles, Sprites, Background, Window, Scrolling)
 * Section 2.9: Sprite attributes in OAM
 *
 * LCD Timing (Section 2.8.1):
 *   Mode 0 (HBlank):    ~204 cycles   (48.6 µs)
 *   Mode 1 (VBlank):    ~4560 cycles  (1.08 ms, 10 lines)
 *   Mode 2 (OAM scan):  ~80 cycles    (19 µs)
 *   Mode 3 (Transfer):  ~172 cycles   (41 µs)
 *   One line = 456 cycles
 *   VBlank = 4560 cycles (10 lines)
 *   Full frame = 70224 cycles
 *
 * Resolution: 160x144 (visible) + 10 VBlank lines = 154 lines total
 */

#ifndef GBPY_GB_PPU_H
#define GBPY_GB_PPU_H

#include "types.h"

/* PPU modes (Section 2.8.1) */
typedef enum {
    PPU_MODE_HBLANK   = 0,  /* Mode 0: CPU can access VRAM + OAM */
    PPU_MODE_VBLANK   = 1,  /* Mode 1: CPU can access all */
    PPU_MODE_OAM_SCAN = 2,  /* Mode 2: CPU cannot access OAM */
    PPU_MODE_TRANSFER = 3,  /* Mode 3: CPU cannot access VRAM + OAM */
} PPUMode;

/* LCDC register bits (Section 2.8.1, FF40) */
#define LCDC_BG_ENABLE      0x01  /* Bit 0: BG Display */
#define LCDC_OBJ_ENABLE     0x02  /* Bit 1: OBJ Display */
#define LCDC_OBJ_SIZE       0x04  /* Bit 2: OBJ Size (0=8x8, 1=8x16) */
#define LCDC_BG_TILEMAP     0x08  /* Bit 3: BG Tile Map (0=9800, 1=9C00) */
#define LCDC_BG_TILEDATA    0x10  /* Bit 4: BG&Win Tile Data (0=8800, 1=8000) */
#define LCDC_WIN_ENABLE     0x20  /* Bit 5: Window Display */
#define LCDC_WIN_TILEMAP    0x40  /* Bit 6: Window Tile Map (0=9800, 1=9C00) */
#define LCDC_LCD_ENABLE     0x80  /* Bit 7: LCD Display Enable */

/* STAT register bits (Section 2.8.1, FF41) */
#define STAT_MODE_FLAG      0x03  /* Bits 0-1: Mode flag */
#define STAT_COINCIDENCE    0x04  /* Bit 2: Coincidence Flag (LY==LYC) */
#define STAT_HBLANK_INT     0x08  /* Bit 3: Mode 0 HBlank Interrupt */
#define STAT_VBLANK_INT     0x10  /* Bit 4: Mode 1 VBlank Interrupt */
#define STAT_OAM_INT        0x20  /* Bit 5: Mode 2 OAM Interrupt */
#define STAT_LYC_INT        0x40  /* Bit 6: LYC=LY Coincidence Interrupt */

/* Sprite/OBJ attributes (Section 2.9) */
typedef struct {
    uint8_t y;        /* Byte 0: Y Position (actual = y - 16) */
    uint8_t x;        /* Byte 1: X Position (actual = x - 8) */
    uint8_t tile;     /* Byte 2: Tile/Pattern Number */
    uint8_t flags;    /* Byte 3: Attributes/Flags */
} OAMEntry;

/* Sprite flag bits (Section 2.9) */
#define OBJ_PRIORITY    0x80  /* Bit 7: 0=Above BG, 1=Behind BG colors 1-3 */
#define OBJ_FLIP_Y      0x40  /* Bit 6: Y flip */
#define OBJ_FLIP_X      0x20  /* Bit 5: X flip */
#define OBJ_PALETTE     0x10  /* Bit 4: (Non-GBC) Palette number */
#define OBJ_BANK_GBC    0x08  /* Bit 3: GBC VRAM Bank */
#define OBJ_PAL_GBC     0x07  /* Bits 0-2: GBC palette number */

/* GBC BG map attribute bits */
#define BG_PAL_GBC      0x07  /* Bits 0-2: Palette number */
#define BG_BANK_GBC     0x08  /* Bit 3: VRAM bank */
#define BG_FLIP_X_GBC   0x20  /* Bit 5: Horizontal flip */
#define BG_FLIP_Y_GBC   0x40  /* Bit 6: Vertical flip */
#define BG_PRIORITY_GBC 0x80  /* Bit 7: BG-to-OAM Priority */

typedef struct GbPPU {
    /* Framebuffer (RGBA) */
    uint32_t framebuffer[GB_SCREEN_WIDTH * GB_SCREEN_HEIGHT];

    /* Internal state */
    PPUMode mode;
    uint32_t mode_clock;        /* Cycles spent in current mode */
    uint8_t line;               /* Current scanline (LY) */
    uint8_t window_line;        /* Internal window line counter */
    bool frame_ready;           /* True when a complete frame is drawn */
    bool stat_irq_line;         /* Previous STAT IRQ line state (for edge detection) */

    /* GBC color palettes (Section 2.8.6) */
    uint8_t bg_palette_data[64];   /* 8 palettes * 4 colors * 2 bytes */
    uint8_t obj_palette_data[64];
    uint8_t bg_palette_idx;
    uint8_t obj_palette_idx;
    bool bg_palette_auto_inc;
    bool obj_palette_auto_inc;

    /* Reference to memory for VRAM/OAM/IO reads */
    uint8_t *vram;        /* Pointer to MMU's VRAM */
    uint8_t *oam;         /* Pointer to MMU's OAM */
    uint8_t *io;          /* Pointer to MMU's I/O registers */

    GbpyMode gb_mode;

    /* Background line pixel data (for priority resolution) */
    uint8_t bg_priority[GB_SCREEN_WIDTH];
} GbPPU;

/* Initialize PPU */
void gb_ppu_init(GbPPU *ppu, GbpyMode mode);

/* Reset PPU state */
void gb_ppu_reset(GbPPU *ppu);

/* Step the PPU by a number of CPU cycles. Returns interrupt flags to raise. */
uint8_t gb_ppu_step(GbPPU *ppu, uint32_t cycles);

/* Render a single scanline */
void gb_ppu_render_scanline(GbPPU *ppu);

/* Read/write GBC color palette registers */
uint8_t gb_ppu_read_palette(GbPPU *ppu, uint16_t addr);
void gb_ppu_write_palette(GbPPU *ppu, uint16_t addr, uint8_t val);

/* Serialization */
size_t gb_ppu_serialize(const GbPPU *ppu, uint8_t *buf, size_t buf_size);
size_t gb_ppu_deserialize(GbPPU *ppu, const uint8_t *buf, size_t buf_size);

#endif /* GBPY_GB_PPU_H */
