/*
 * GBA PPU (Picture Processing Unit) Header
 *
 * GBA LCD: 240x160 pixels, 15-bit color (RGB555)
 * Modes 0-2: Tile/map based
 * Modes 3-5: Bitmap based
 * 4 background layers, 128 sprites, affine transformations
 * Alpha blending, windowing, mosaic
 */

#ifndef GBPY_GBA_PPU_H
#define GBPY_GBA_PPU_H

#include "../types.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* Forward declarations */
typedef struct GbaMemory GbaMemory;

/* GBA screen dimensions */
#define GBA_SCREEN_W  240
#define GBA_SCREEN_H  160
#define GBA_SCANLINES 228  /* 160 visible + 68 VBlank */

/* Timing: 1232 cycles per scanline (960 HDraw + 272 HBlank) */
#define GBA_HDRAW_CYCLES   960
#define GBA_HBLANK_CYCLES  272
#define GBA_LINE_CYCLES    1232
#define GBA_FRAME_CYCLES   (GBA_LINE_CYCLES * GBA_SCANLINES) /* 280896 */

/* DISPCNT bits */
#define DISPCNT_MODE(v)       ((v) & 7)
#define DISPCNT_FRAME_SEL(v)  (((v) >> 4) & 1)
#define DISPCNT_HBLANK_FREE(v)(((v) >> 5) & 1)
#define DISPCNT_OBJ_MAPPING(v)(((v) >> 6) & 1) /* 0=2D, 1=1D */
#define DISPCNT_FORCED_BLANK(v)(((v) >> 7) & 1)
#define DISPCNT_BG0_EN(v)    (((v) >> 8) & 1)
#define DISPCNT_BG1_EN(v)    (((v) >> 9) & 1)
#define DISPCNT_BG2_EN(v)    (((v) >> 10) & 1)
#define DISPCNT_BG3_EN(v)    (((v) >> 11) & 1)
#define DISPCNT_OBJ_EN(v)    (((v) >> 12) & 1)
#define DISPCNT_WIN0_EN(v)   (((v) >> 13) & 1)
#define DISPCNT_WIN1_EN(v)   (((v) >> 14) & 1)
#define DISPCNT_OBJWIN_EN(v) (((v) >> 15) & 1)

/* DISPSTAT bits */
#define DISPSTAT_VBLANK       (1 << 0)
#define DISPSTAT_HBLANK       (1 << 1)
#define DISPSTAT_VCOUNTER     (1 << 2)
#define DISPSTAT_VBLANK_IRQ   (1 << 3)
#define DISPSTAT_HBLANK_IRQ   (1 << 4)
#define DISPSTAT_VCOUNTER_IRQ (1 << 5)

/* BGCNT bits */
#define BGCNT_PRIORITY(v)   ((v) & 3)
#define BGCNT_TILE_BASE(v)  (((v) >> 2) & 3)
#define BGCNT_MOSAIC(v)     (((v) >> 6) & 1)
#define BGCNT_COLOR_MODE(v) (((v) >> 7) & 1)  /* 0=16/16, 1=256/1 */
#define BGCNT_MAP_BASE(v)   (((v) >> 8) & 0x1F)
#define BGCNT_OVERFLOW(v)   (((v) >> 13) & 1) /* Affine: 0=transparent, 1=wrap */
#define BGCNT_SIZE(v)       (((v) >> 14) & 3)

/* OAM attribute bits */
#define OBJ_ATTR0_Y(v)       ((v) & 0xFF)
#define OBJ_ATTR0_MODE(v)    (((v) >> 10) & 3) /* 0=normal, 1=semi-trans, 2=obj-window, 3=prohibited */
#define OBJ_ATTR0_GFX(v)     (((v) >> 8) & 3)  /* 0=normal, 1=affine, 2=disabled, 3=affine double */
#define OBJ_ATTR0_MOSAIC(v)  (((v) >> 12) & 1)
#define OBJ_ATTR0_COLOR(v)   (((v) >> 13) & 1) /* 0=16/16, 1=256/1 */
#define OBJ_ATTR0_SHAPE(v)   (((v) >> 14) & 3)

#define OBJ_ATTR1_X(v)       ((v) & 0x1FF)
#define OBJ_ATTR1_AFFINE_IDX(v) (((v) >> 9) & 0x1F)
#define OBJ_ATTR1_HFLIP(v)   (((v) >> 12) & 1) /* Non-affine */
#define OBJ_ATTR1_VFLIP(v)   (((v) >> 13) & 1) /* Non-affine */
#define OBJ_ATTR1_SIZE(v)    (((v) >> 14) & 3)

#define OBJ_ATTR2_TILE(v)    ((v) & 0x3FF)
#define OBJ_ATTR2_PRIORITY(v)(((v) >> 10) & 3)
#define OBJ_ATTR2_PALETTE(v) (((v) >> 12) & 0xF)

/* Blend mode */
#define BLEND_NONE     0
#define BLEND_ALPHA    1
#define BLEND_BRIGHTEN 2
#define BLEND_DARKEN   3

/* Layer IDs for priority sorting */
#define LAYER_BG0  0
#define LAYER_BG1  1
#define LAYER_BG2  2
#define LAYER_BG3  3
#define LAYER_OBJ  4
#define LAYER_BD   5  /* Backdrop */

typedef struct GbaPPU {
    /* Framebuffer: 240x160 in RGB888 (for easy display) */
    uint8_t framebuffer[GBA_SCREEN_W * GBA_SCREEN_H * 4]; /* RGBA */

    /* Scanline cycle counter */
    uint32_t cycle;
    uint16_t line;      /* Current scanline (VCOUNT) */
    bool in_hblank;
    bool in_vblank;

    /* Internal affine reference points (latched on VBlank) */
    int32_t bg2_ref_x;
    int32_t bg2_ref_y;
    int32_t bg3_ref_x;
    int32_t bg3_ref_y;

    /* Internal current affine positions */
    int32_t bg2_x;
    int32_t bg2_y;
    int32_t bg3_x;
    int32_t bg3_y;

    /* Per-scanline layer buffers for priority/blending */
    uint16_t bg_line[4][GBA_SCREEN_W];     /* Color index per BG layer */
    uint8_t  bg_priority[4];                /* Priority for each BG */
    bool     bg_pixel_valid[4][GBA_SCREEN_W]; /* Did this pixel have data? */
    uint16_t obj_line[GBA_SCREEN_W];       /* Sprite color */
    uint8_t  obj_prio[GBA_SCREEN_W];       /* Sprite priority */
    bool     obj_pixel_valid[GBA_SCREEN_W];
    bool     obj_semi_trans[GBA_SCREEN_W]; /* Semi-transparent flag */
    bool     obj_is_window[GBA_SCREEN_W];  /* OBJ window mask flag */

    /* Frame ready flag */
    bool frame_ready;

    /* Memory pointer */
    GbaMemory *mem;
} GbaPPU;

/* API */
void gba_ppu_init(GbaPPU *ppu);
void gba_ppu_reset(GbaPPU *ppu);
void gba_ppu_step(GbaPPU *ppu, uint32_t cycles);

/* Serialization */
size_t gba_ppu_serialize(const GbaPPU *ppu, uint8_t *buf, size_t buf_size);
size_t gba_ppu_deserialize(GbaPPU *ppu, const uint8_t *buf, size_t buf_size);

#endif /* GBPY_GBA_PPU_H */
