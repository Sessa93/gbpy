/*
 * Game Boy / Game Boy Color PPU Implementation
 *
 * Reference: Game Boy CPU Manual v1.01 (resources/gba-manual.md)
 *
 * Tile Data (Section 2.8.1):
 *   Each tile is 8x8 pixels, 16 bytes (2 bytes per row).
 *   Each pixel is 2 bits; bit planes interleaved.
 *   Block 0: $8000-$87FF (tiles 0-127)
 *   Block 1: $8800-$8FFF (tiles 128-255 or signed -128 to -1)
 *   Block 2: $9000-$97FF (tiles 0-127 signed method)
 *
 * BG Tile Maps (Section 2.8.3):
 *   $9800-$9BFF: 32x32 tile map 0
 *   $9C00-$9FFF: 32x32 tile map 1
 *
 * DMG Palettes (Section 2.8.5):
 *   BGP  (FF47): BG palette
 *   OBP0 (FF48): Sprite palette 0
 *   OBP1 (FF49): Sprite palette 1
 *   Each: bits 1-0 = color 0, bits 3-2 = color 1, etc.
 *
 * Sprite (OBJ) attributes in OAM at FE00-FE9F (Section 2.9):
 *   4 bytes each, 40 sprites max, 10 per scanline max
 */

#include "gb_ppu.h"

/*
 * DMG monochrome colors (shades of green like original GB)
 * Stored as RGBA in byte order: R,G,B,A
 * On little-endian: uint32_t = 0xAABBGGRR
 */
static const uint32_t DMG_COLORS[4] = {
    0xFFD0F8E0,  /* Lightest (color 0): R=E0, G=F8, B=D0 */
    0xFF70C088,  /* Light    (color 1): R=88, G=C0, B=70 */
    0xFF565834,  /* Dark     (color 2): R=34, G=68, B=56 */
    0xFF201808,  /* Darkest  (color 3): R=08, G=18, B=20 */
};

/* I/O register offsets from FF00 */
#define IO_LCDC  0x40
#define IO_STAT  0x41
#define IO_SCY   0x42
#define IO_SCX   0x43
#define IO_LY    0x44
#define IO_LYC   0x45
#define IO_BGP   0x47
#define IO_OBP0  0x48
#define IO_OBP1  0x49
#define IO_WY    0x4A
#define IO_WX    0x4B

/* Interrupt bits */
#define INT_VBLANK_BIT 0x01
#define INT_LCD_BIT    0x02

/* Timing constants (Section 2.8.1) */
#define CYCLES_OAM_SCAN   80
#define CYCLES_TRANSFER    172
#define CYCLES_HBLANK      204
#define CYCLES_LINE         456
#define SCANLINES_VISIBLE   144
#define SCANLINES_TOTAL     154

void gb_ppu_init(GbPPU *ppu, GbpyMode mode) {
    memset(ppu, 0, sizeof(GbPPU));
    ppu->gb_mode = mode;
    ppu->mode = PPU_MODE_OAM_SCAN;
}

void gb_ppu_reset(GbPPU *ppu) {
    ppu->mode = PPU_MODE_OAM_SCAN;
    ppu->mode_clock = 0;
    ppu->line = 0;
    ppu->window_line = 0;
    ppu->frame_ready = false;
    memset(ppu->framebuffer, 0, sizeof(ppu->framebuffer));
}

/* Convert GBC 15-bit color (XBBBBBGGGGGRRRRR) to RGBA32 (byte order: R,G,B,A) */
GBPY_INLINE uint32_t gbc_color_to_rgba(uint16_t color) {
    uint8_t r = (color & 0x1F) << 3;
    uint8_t g = ((color >> 5) & 0x1F) << 3;
    uint8_t b = ((color >> 10) & 0x1F) << 3;
    /* Extend 5-bit to 8-bit properly */
    r |= (r >> 5);
    g |= (g >> 5);
    b |= (b >> 5);
    /* RGBA byte order: on little-endian, 0xAABBGGRR stores as R,G,B,A */
    return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | 0xFF000000;
}

/*
 * Get tile pixel color index.
 * Section 2.8.1: "Each Tile occupies 16 bytes, where each 2 bytes represent a line"
 * "For each line, the first byte defines the least significant bits of the color
 *  numbers, and the second byte defines the upper bits."
 */
GBPY_INLINE uint8_t get_tile_pixel(const uint8_t *vram, uint16_t tile_addr,
                                    uint8_t px, uint8_t py) {
    uint16_t offset = tile_addr + (py * 2);
    uint8_t lo = vram[offset];
    uint8_t hi = vram[offset + 1];
    uint8_t bit = 7 - px;
    return ((hi >> bit) & 1) << 1 | ((lo >> bit) & 1);
}

/* Apply DMG palette mapping */
GBPY_INLINE uint8_t apply_dmg_palette(uint8_t palette, uint8_t color_idx) {
    return (palette >> (color_idx * 2)) & 0x03;
}

/*
 * Render Background (Section 2.8.3)
 * Background is a 256x256 pixel (32x32 tile) map that scrolls via SCX/SCY
 */
static void render_bg_line(GbPPU *ppu) {
    uint8_t lcdc = ppu->io[IO_LCDC];

    /* DMG: if BG disabled, fill with color 0 */
    if (ppu->gb_mode != GBPY_MODE_GBC && !(lcdc & LCDC_BG_ENABLE)) {
        uint32_t offset = ppu->line * GB_SCREEN_WIDTH;
        for (int x = 0; x < GB_SCREEN_WIDTH; x++) {
            ppu->framebuffer[offset + x] = DMG_COLORS[0];
            ppu->bg_priority[x] = 0;
        }
        return;
    }

    uint8_t scy = ppu->io[IO_SCY];
    uint8_t scx = ppu->io[IO_SCX];

    /* Tile map base (Section 2.8.3): LCDC bit 3 */
    uint16_t tile_map = (lcdc & LCDC_BG_TILEMAP) ? 0x1C00 : 0x1800;

    /* Tile data base (Section 2.8.1): LCDC bit 4 */
    bool signed_addr = !(lcdc & LCDC_BG_TILEDATA);

    uint8_t y = scy + ppu->line;
    uint8_t tile_row = y >> 3;       /* y / 8 */
    uint8_t tile_py  = y & 0x07;     /* y % 8 */

    uint32_t fb_offset = ppu->line * GB_SCREEN_WIDTH;

    for (int px = 0; px < GB_SCREEN_WIDTH; px++) {
        uint8_t x = scx + (uint8_t)px;
        uint8_t tile_col = x >> 3;
        uint8_t tile_px  = x & 0x07;

        /* Look up tile index in tile map */
        uint16_t map_addr = tile_map + (uint16_t)(tile_row * 32) + tile_col;
        uint8_t tile_idx = ppu->vram[map_addr];

        /* Calculate tile data address */
        uint16_t tile_addr;
        if (signed_addr) {
            tile_addr = 0x1000 + ((int8_t)tile_idx * 16);
        } else {
            tile_addr = tile_idx * 16;
        }

        uint8_t actual_px = tile_px;
        uint8_t actual_py = tile_py;

        /* GBC: read BG map attributes from VRAM bank 1 */
        uint8_t bg_attr = 0;
        if (ppu->gb_mode == GBPY_MODE_GBC) {
            bg_attr = ppu->vram[0x2000 + map_addr];
            if (bg_attr & BG_FLIP_X_GBC) actual_px = 7 - actual_px;
            if (bg_attr & BG_FLIP_Y_GBC) actual_py = 7 - actual_py;
        }

        /* Get color index from tile */
        const uint8_t *tile_vram = ppu->vram;
        if (ppu->gb_mode == GBPY_MODE_GBC && (bg_attr & BG_BANK_GBC)) {
            tile_vram = ppu->vram + 0x2000; /* VRAM bank 1 */
        }

        uint8_t color_idx = get_tile_pixel(tile_vram, tile_addr, actual_px, actual_py);
        ppu->bg_priority[px] = color_idx;

        /* Apply palette and write pixel */
        if (ppu->gb_mode == GBPY_MODE_GBC) {
            uint8_t pal_num = bg_attr & BG_PAL_GBC;
            uint16_t pal_offset = pal_num * 8 + color_idx * 2;
            uint16_t raw_color = ppu->bg_palette_data[pal_offset]
                               | ((uint16_t)ppu->bg_palette_data[pal_offset + 1] << 8);
            ppu->framebuffer[fb_offset + px] = gbc_color_to_rgba(raw_color);
        } else {
            uint8_t mapped = apply_dmg_palette(ppu->io[IO_BGP], color_idx);
            ppu->framebuffer[fb_offset + px] = DMG_COLORS[mapped];
        }
    }
}

/*
 * Render Window (Section 2.8.4)
 * Window is like a second BG layer at fixed position (WX-7, WY)
 */
static void render_window_line(GbPPU *ppu) {
    uint8_t lcdc = ppu->io[IO_LCDC];
    if (!(lcdc & LCDC_WIN_ENABLE)) return;

    uint8_t wy = ppu->io[IO_WY];
    uint8_t wx = ppu->io[IO_WX];
    if (ppu->line < wy) return;
    if (wx > 166) return;

    int start_x = (int)wx - 7;

    uint16_t tile_map = (lcdc & LCDC_WIN_TILEMAP) ? 0x1C00 : 0x1800;
    bool signed_addr = !(lcdc & LCDC_BG_TILEDATA);

    uint8_t win_y = ppu->window_line;
    uint8_t tile_row = win_y >> 3;
    uint8_t tile_py = win_y & 0x07;

    uint32_t fb_offset = ppu->line * GB_SCREEN_WIDTH;
    bool rendered = false;

    for (int px = 0; px < GB_SCREEN_WIDTH; px++) {
        int screen_x = px;
        if (screen_x < start_x) continue;
        rendered = true;

        int win_x = screen_x - start_x;
        uint8_t tile_col = (uint8_t)(win_x >> 3);
        uint8_t tile_px = (uint8_t)(win_x & 0x07);

        uint16_t map_addr = tile_map + (uint16_t)(tile_row * 32) + tile_col;
        uint8_t tile_idx = ppu->vram[map_addr];

        uint16_t tile_addr;
        if (signed_addr) {
            tile_addr = 0x1000 + ((int8_t)tile_idx * 16);
        } else {
            tile_addr = tile_idx * 16;
        }

        uint8_t actual_px = tile_px;
        uint8_t actual_py = tile_py;

        uint8_t bg_attr = 0;
        if (ppu->gb_mode == GBPY_MODE_GBC) {
            bg_attr = ppu->vram[0x2000 + map_addr];
            if (bg_attr & BG_FLIP_X_GBC) actual_px = 7 - actual_px;
            if (bg_attr & BG_FLIP_Y_GBC) actual_py = 7 - actual_py;
        }

        const uint8_t *tile_vram = ppu->vram;
        if (ppu->gb_mode == GBPY_MODE_GBC && (bg_attr & BG_BANK_GBC)) {
            tile_vram = ppu->vram + 0x2000;
        }

        uint8_t color_idx = get_tile_pixel(tile_vram, tile_addr, actual_px, actual_py);
        ppu->bg_priority[px] = color_idx;

        if (ppu->gb_mode == GBPY_MODE_GBC) {
            uint8_t pal_num = bg_attr & BG_PAL_GBC;
            uint16_t pal_offset = pal_num * 8 + color_idx * 2;
            uint16_t raw_color = ppu->bg_palette_data[pal_offset]
                               | ((uint16_t)ppu->bg_palette_data[pal_offset + 1] << 8);
            ppu->framebuffer[fb_offset + px] = gbc_color_to_rgba(raw_color);
        } else {
            uint8_t mapped = apply_dmg_palette(ppu->io[IO_BGP], color_idx);
            ppu->framebuffer[fb_offset + px] = DMG_COLORS[mapped];
        }
    }

    if (rendered) ppu->window_line++;
}

/*
 * Render Sprites / OBJ (Section 2.9)
 * OAM: FE00-FE9F, 40 entries of 4 bytes each
 * Max 10 sprites per scanline
 * Size 8x8 or 8x16 (LCDC bit 2)
 */
static void render_sprites_line(GbPPU *ppu) {
    uint8_t lcdc = ppu->io[IO_LCDC];
    if (!(lcdc & LCDC_OBJ_ENABLE)) return;

    uint8_t sprite_height = (lcdc & LCDC_OBJ_SIZE) ? 16 : 8;
    uint32_t fb_offset = ppu->line * GB_SCREEN_WIDTH;

    /* Collect visible sprites for this line (max 10, Section 2.9) */
    typedef struct { uint8_t idx; uint8_t x; } SpriteEntry;
    SpriteEntry visible[10];
    int count = 0;

    const OAMEntry *oam = (const OAMEntry *)ppu->oam;
    for (int i = 0; i < 40 && count < 10; i++) {
        int sy = (int)oam[i].y - 16;
        if (ppu->line >= sy && ppu->line < sy + sprite_height) {
            visible[count].idx = (uint8_t)i;
            visible[count].x = oam[i].x;
            count++;
        }
    }

    /* DMG: sprites with lower X have priority; same X = lower OAM index wins */
    /* GBC: only OAM order matters */
    if (ppu->gb_mode != GBPY_MODE_GBC) {
        for (int i = 0; i < count - 1; i++) {
            for (int j = i + 1; j < count; j++) {
                if (visible[j].x < visible[i].x ||
                    (visible[j].x == visible[i].x && visible[j].idx < visible[i].idx)) {
                    SpriteEntry tmp = visible[i];
                    visible[i] = visible[j];
                    visible[j] = tmp;
                }
            }
        }
    }

    /* Render in reverse order so higher priority sprites overwrite */
    for (int s = count - 1; s >= 0; s--) {
        const OAMEntry *sprite = &oam[visible[s].idx];
        int sy = (int)sprite->y - 16;
        int sx = (int)sprite->x - 8;
        uint8_t flags = sprite->flags;

        uint8_t py = (uint8_t)(ppu->line - sy);
        if (flags & OBJ_FLIP_Y) py = (sprite_height - 1) - py;

        uint8_t tile_idx = sprite->tile;
        if (sprite_height == 16) {
            tile_idx &= 0xFE; /* In 8x16 mode, bit 0 is ignored */
        }

        uint16_t tile_addr = (uint16_t)tile_idx * 16;

        /* GBC: select VRAM bank */
        const uint8_t *tile_vram = ppu->vram;
        if (ppu->gb_mode == GBPY_MODE_GBC && (flags & OBJ_BANK_GBC)) {
            tile_vram = ppu->vram + 0x2000;
        }

        for (int px_off = 0; px_off < 8; px_off++) {
            int screen_x = sx + px_off;
            if (screen_x < 0 || screen_x >= GB_SCREEN_WIDTH) continue;

            uint8_t tile_px = (flags & OBJ_FLIP_X) ? (7 - px_off) : px_off;
            uint8_t color_idx = get_tile_pixel(tile_vram, tile_addr, tile_px, py);

            /* Color 0 is always transparent for sprites (Section 2.9) */
            if (color_idx == 0) continue;

            /* Priority: if bit 7 set, sprite is behind BG colors 1-3 */
            if ((flags & OBJ_PRIORITY) && ppu->bg_priority[screen_x] != 0) {
                continue;
            }

            /* Apply palette */
            if (ppu->gb_mode == GBPY_MODE_GBC) {
                uint8_t pal_num = flags & OBJ_PAL_GBC;
                uint16_t pal_offset = pal_num * 8 + color_idx * 2;
                uint16_t raw_color = ppu->obj_palette_data[pal_offset]
                                   | ((uint16_t)ppu->obj_palette_data[pal_offset + 1] << 8);
                ppu->framebuffer[fb_offset + screen_x] = gbc_color_to_rgba(raw_color);
            } else {
                uint8_t palette_reg = (flags & OBJ_PALETTE) ? ppu->io[IO_OBP1] : ppu->io[IO_OBP0];
                uint8_t mapped = apply_dmg_palette(palette_reg, color_idx);
                ppu->framebuffer[fb_offset + screen_x] = DMG_COLORS[mapped];
            }
        }
    }
}

void gb_ppu_render_scanline(GbPPU *ppu) {
    if (ppu->line >= SCANLINES_VISIBLE) return;
    render_bg_line(ppu);
    render_window_line(ppu);
    render_sprites_line(ppu);
}

/*
 * Step PPU by given number of CPU cycles.
 * Returns interrupt flags to raise (INT_VBLANK_BIT and/or INT_LCD_BIT).
 *
 * Mode timing (Section 2.8.1):
 *   Mode 2 -> Mode 3 -> Mode 0 -> (next line)
 *   After line 143: Mode 1 for 10 lines
 */
uint8_t gb_ppu_step(GbPPU *ppu, uint32_t cycles) {
    uint8_t lcdc = ppu->io[IO_LCDC];
    uint8_t interrupts = 0;

    /* LCD disabled: stay in mode 0, LY = 0 */
    if (!(lcdc & LCDC_LCD_ENABLE)) {
        ppu->mode = PPU_MODE_HBLANK;
        ppu->mode_clock = 0;
        ppu->line = 0;
        ppu->io[IO_LY] = 0;
        ppu->io[IO_STAT] = (ppu->io[IO_STAT] & 0xFC);
        return 0;
    }

    ppu->mode_clock += cycles;

    switch (ppu->mode) {
        case PPU_MODE_OAM_SCAN:
            if (ppu->mode_clock >= CYCLES_OAM_SCAN) {
                ppu->mode_clock -= CYCLES_OAM_SCAN;
                ppu->mode = PPU_MODE_TRANSFER;
            }
            break;

        case PPU_MODE_TRANSFER:
            if (ppu->mode_clock >= CYCLES_TRANSFER) {
                ppu->mode_clock -= CYCLES_TRANSFER;
                ppu->mode = PPU_MODE_HBLANK;

                /* Render the scanline */
                gb_ppu_render_scanline(ppu);

                /* STAT Mode 0 interrupt */
                if (ppu->io[IO_STAT] & STAT_HBLANK_INT) {
                    interrupts |= INT_LCD_BIT;
                }
            }
            break;

        case PPU_MODE_HBLANK:
            if (ppu->mode_clock >= CYCLES_HBLANK) {
                ppu->mode_clock -= CYCLES_HBLANK;
                ppu->line++;
                ppu->io[IO_LY] = ppu->line;

                /* Check LYC coincidence */
                if (ppu->line == ppu->io[IO_LYC]) {
                    ppu->io[IO_STAT] |= STAT_COINCIDENCE;
                    if (ppu->io[IO_STAT] & STAT_LYC_INT) {
                        interrupts |= INT_LCD_BIT;
                    }
                } else {
                    ppu->io[IO_STAT] &= ~STAT_COINCIDENCE;
                }

                if (ppu->line >= SCANLINES_VISIBLE) {
                    /* Enter VBlank */
                    ppu->mode = PPU_MODE_VBLANK;
                    ppu->frame_ready = true;
                    interrupts |= INT_VBLANK_BIT;

                    if (ppu->io[IO_STAT] & STAT_VBLANK_INT) {
                        interrupts |= INT_LCD_BIT;
                    }
                } else {
                    ppu->mode = PPU_MODE_OAM_SCAN;
                    if (ppu->io[IO_STAT] & STAT_OAM_INT) {
                        interrupts |= INT_LCD_BIT;
                    }
                }
            }
            break;

        case PPU_MODE_VBLANK:
            if (ppu->mode_clock >= CYCLES_LINE) {
                ppu->mode_clock -= CYCLES_LINE;
                ppu->line++;
                ppu->io[IO_LY] = ppu->line;

                if (ppu->line == ppu->io[IO_LYC]) {
                    ppu->io[IO_STAT] |= STAT_COINCIDENCE;
                    if (ppu->io[IO_STAT] & STAT_LYC_INT) {
                        interrupts |= INT_LCD_BIT;
                    }
                } else {
                    ppu->io[IO_STAT] &= ~STAT_COINCIDENCE;
                }

                if (ppu->line >= SCANLINES_TOTAL) {
                    /* New frame */
                    ppu->line = 0;
                    ppu->window_line = 0;
                    ppu->io[IO_LY] = 0;
                    ppu->mode = PPU_MODE_OAM_SCAN;

                    if (ppu->io[IO_STAT] & STAT_OAM_INT) {
                        interrupts |= INT_LCD_BIT;
                    }
                }
            }
            break;
    }

    /* Update STAT mode bits */
    ppu->io[IO_STAT] = (ppu->io[IO_STAT] & 0xFC) | (ppu->mode & 0x03);

    return interrupts;
}

/* GBC color palette register read/write */
uint8_t gb_ppu_read_palette(GbPPU *ppu, uint16_t addr) {
    switch (addr & 0xFF) {
        case 0x68: return ppu->bg_palette_idx | (ppu->bg_palette_auto_inc ? 0x80 : 0);
        case 0x69: return ppu->bg_palette_data[ppu->bg_palette_idx & 0x3F];
        case 0x6A: return ppu->obj_palette_idx | (ppu->obj_palette_auto_inc ? 0x80 : 0);
        case 0x6B: return ppu->obj_palette_data[ppu->obj_palette_idx & 0x3F];
        default: return 0xFF;
    }
}

void gb_ppu_write_palette(GbPPU *ppu, uint16_t addr, uint8_t val) {
    switch (addr & 0xFF) {
        case 0x68:
            ppu->bg_palette_idx = val & 0x3F;
            ppu->bg_palette_auto_inc = (val & 0x80) != 0;
            break;
        case 0x69:
            ppu->bg_palette_data[ppu->bg_palette_idx & 0x3F] = val;
            if (ppu->bg_palette_auto_inc)
                ppu->bg_palette_idx = (ppu->bg_palette_idx + 1) & 0x3F;
            break;
        case 0x6A:
            ppu->obj_palette_idx = val & 0x3F;
            ppu->obj_palette_auto_inc = (val & 0x80) != 0;
            break;
        case 0x6B:
            ppu->obj_palette_data[ppu->obj_palette_idx & 0x3F] = val;
            if (ppu->obj_palette_auto_inc)
                ppu->obj_palette_idx = (ppu->obj_palette_idx + 1) & 0x3F;
            break;
    }
}

/* Serialization */
size_t gb_ppu_serialize(const GbPPU *ppu, uint8_t *buf, size_t buf_size) {
    size_t needed = sizeof(ppu->mode) + sizeof(ppu->mode_clock) + sizeof(ppu->line)
                  + sizeof(ppu->window_line) + sizeof(ppu->bg_palette_data)
                  + sizeof(ppu->obj_palette_data) + 8;
    if (!buf || buf_size < needed) return needed;

    size_t pos = 0;
    #define S(f) memcpy(buf+pos, &ppu->f, sizeof(ppu->f)); pos += sizeof(ppu->f)
    S(mode); S(mode_clock); S(line); S(window_line);
    S(bg_palette_data); S(obj_palette_data);
    S(bg_palette_idx); S(obj_palette_idx);
    S(bg_palette_auto_inc); S(obj_palette_auto_inc);
    #undef S
    return pos;
}

size_t gb_ppu_deserialize(GbPPU *ppu, const uint8_t *buf, size_t buf_size) {
    size_t pos = 0;
    #define L(f) do { if (pos+sizeof(ppu->f)>buf_size) return 0; \
        memcpy(&ppu->f, buf+pos, sizeof(ppu->f)); pos += sizeof(ppu->f); } while(0)
    L(mode); L(mode_clock); L(line); L(window_line);
    L(bg_palette_data); L(obj_palette_data);
    L(bg_palette_idx); L(obj_palette_idx);
    L(bg_palette_auto_inc); L(obj_palette_auto_inc);
    #undef L
    return pos;
}
