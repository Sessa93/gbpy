/*
 * GBA PPU Implementation
 *
 * Scanline-based renderer supporting all 6 display modes:
 * Mode 0: 4 text BGs
 * Mode 1: 2 text + 1 affine BG
 * Mode 2: 2 affine BGs
 * Mode 3: 240x160 16bpp bitmap
 * Mode 4: 240x160 8bpp bitmap (double buffered)
 * Mode 5: 160x128 16bpp bitmap (double buffered)
 *
 * Plus sprites (affine + regular), alpha blending, windowing, mosaic.
 */

#include "gba_ppu.h"
#include "../memory/gba_memory.h"

/* ---------- Helpers ---------- */

static GBPY_INLINE uint16_t ppu_read16(GbaMemory *mem, uint32_t addr) {
    return *(uint16_t *)&mem->vram[addr & 0x17FFE];
}

static GBPY_INLINE uint16_t palette_read(GbaMemory *mem, uint32_t idx) {
    return *(uint16_t *)&mem->palette[idx * 2];
}

/* Convert GBA BGR555 to RGBA8888 */
static GBPY_INLINE void bgr555_to_rgba(uint16_t color, uint8_t *out) {
    uint8_t r = (color & 0x1F) << 3;
    uint8_t g = ((color >> 5) & 0x1F) << 3;
    uint8_t b = ((color >> 10) & 0x1F) << 3;
    out[0] = r;
    out[1] = g;
    out[2] = b;
    out[3] = 0xFF;
}

/* Sign-extend 28-bit to 32-bit */
static GBPY_INLINE int32_t sign_extend_28(uint32_t val) {
    if (val & 0x08000000) return (int32_t)(val | 0xF0000000);
    return (int32_t)val;
}

/* Sprite sizes: shape x size -> width, height */
static const uint8_t obj_width[4][4] = {
    {8, 16, 32, 64}, /* Square */
    {16, 32, 32, 64}, /* Horizontal */
    {8, 8, 16, 32}, /* Vertical */
    {0, 0, 0, 0}
};
static const uint8_t obj_height[4][4] = {
    {8, 16, 32, 64}, /* Square */
    {8, 8, 16, 32}, /* Horizontal */
    {16, 32, 32, 64}, /* Vertical */
    {0, 0, 0, 0}
};

/* Text BG sizes */
static const uint16_t text_bg_width[4]  = {256, 512, 256, 512};
static const uint16_t text_bg_height[4] = {256, 256, 512, 512};

/* Affine BG sizes */
static const uint16_t affine_bg_size[4] = {128, 256, 512, 1024};

/* ---------- Init/Reset ---------- */

void gba_ppu_init(GbaPPU *ppu) {
    memset(ppu, 0, sizeof(GbaPPU));
}

void gba_ppu_reset(GbaPPU *ppu) {
    GbaMemory *m = ppu->mem;
    memset(ppu, 0, sizeof(GbaPPU));
    ppu->mem = m;
}

/* ---------- IO register accessors ---------- */

static GBPY_INLINE uint16_t get_dispcnt(GbaMemory *mem) {
    return *(uint16_t *)&mem->io[0x000];
}

static GBPY_INLINE uint16_t get_dispstat(GbaMemory *mem) {
    return *(uint16_t *)&mem->io[0x004];
}

static GBPY_INLINE void set_dispstat(GbaMemory *mem, uint16_t val) {
    /* Only set read-only bits 0-2, keep writable bits */
    uint16_t old = *(uint16_t *)&mem->io[0x004];
    *(uint16_t *)&mem->io[0x004] = (old & 0xFFF8) | (val & 0x7) | (old & 0xFFF8);
    /* Actually: preserve user-writable bits (3-5, 8-15), set status bits (0-2) */
    *(uint16_t *)&mem->io[0x004] = (old & ~0x7) | (val & 0x7);
}

static GBPY_INLINE void set_vcount(GbaMemory *mem, uint16_t line) {
    *(uint16_t *)&mem->io[0x006] = line;
}

static GBPY_INLINE uint16_t get_bgcnt(GbaMemory *mem, int bg) {
    return *(uint16_t *)&mem->io[0x008 + bg * 2];
}

static GBPY_INLINE uint16_t get_bghofs(GbaMemory *mem, int bg) {
    return *(uint16_t *)&mem->io[0x010 + bg * 4] & 0x1FF;
}

static GBPY_INLINE uint16_t get_bgvofs(GbaMemory *mem, int bg) {
    return *(uint16_t *)&mem->io[0x012 + bg * 4] & 0x1FF;
}

/* ---------- Render text BG scanline ---------- */

static void render_text_bg(GbaPPU *ppu, int bg, uint16_t line) {
    GbaMemory *mem = ppu->mem;
    uint16_t bgcnt = get_bgcnt(mem, bg);
    uint16_t hofs = get_bghofs(mem, bg);
    uint16_t vofs = get_bgvofs(mem, bg);

    uint32_t tile_base = BGCNT_TILE_BASE(bgcnt) * 0x4000;
    uint32_t map_base = BGCNT_MAP_BASE(bgcnt) * 0x800;
    bool color256 = BGCNT_COLOR_MODE(bgcnt);
    uint8_t size = BGCNT_SIZE(bgcnt);

    uint16_t bg_w = text_bg_width[size];
    uint16_t bg_h = text_bg_height[size];

    uint16_t y = (line + vofs) % bg_h;
    uint16_t tile_y = y / 8;
    uint16_t fine_y = y % 8;

    for (int x = 0; x < GBA_SCREEN_W; x++) {
        uint16_t px = (x + hofs) % bg_w;
        uint16_t tile_x = px / 8;
        uint16_t fine_x = px % 8;

        /* Calculate screen block index for tiles > 32 */
        uint32_t screen_block = 0;
        uint16_t local_tile_x = tile_x;
        uint16_t local_tile_y = tile_y;
        if (tile_x >= 32) { screen_block += 1; local_tile_x -= 32; }
        if (tile_y >= 32) { screen_block += (bg_w == 512) ? 2 : 1; local_tile_y -= 32; }

        uint32_t map_addr = map_base + screen_block * 0x800
                          + local_tile_y * 64 + local_tile_x * 2;

        uint16_t map_entry = *(uint16_t *)&mem->vram[map_addr & 0x17FFF];
        uint16_t tile_num = map_entry & 0x3FF;
        bool h_flip = (map_entry >> 10) & 1;
        bool v_flip = (map_entry >> 11) & 1;
        uint8_t pal_bank = (map_entry >> 12) & 0xF;

        uint16_t fy = v_flip ? (7 - fine_y) : fine_y;
        uint16_t fx = h_flip ? (7 - fine_x) : fine_x;

        uint8_t pixel;
        if (color256) {
            uint32_t addr = tile_base + tile_num * 64 + fy * 8 + fx;
            pixel = mem->vram[addr & 0x17FFF];
            if (pixel == 0) {
                ppu->bg_pixel_valid[bg][x] = false;
                continue;
            }
            ppu->bg_line[bg][x] = palette_read(mem, pixel);
        } else {
            uint32_t addr = tile_base + tile_num * 32 + fy * 4 + fx / 2;
            uint8_t byte = mem->vram[addr & 0x17FFF];
            pixel = (fx & 1) ? (byte >> 4) : (byte & 0xF);
            if (pixel == 0) {
                ppu->bg_pixel_valid[bg][x] = false;
                continue;
            }
            ppu->bg_line[bg][x] = palette_read(mem, pal_bank * 16 + pixel);
        }
        ppu->bg_pixel_valid[bg][x] = true;
    }

    ppu->bg_priority[bg] = BGCNT_PRIORITY(bgcnt);
}

/* ---------- Render affine BG scanline ---------- */

static void render_affine_bg(GbaPPU *ppu, int bg, uint16_t line) {
    GbaMemory *mem = ppu->mem;
    uint16_t bgcnt = get_bgcnt(mem, bg);

    uint32_t tile_base = BGCNT_TILE_BASE(bgcnt) * 0x4000;
    uint32_t map_base = BGCNT_MAP_BASE(bgcnt) * 0x800;
    uint8_t size_idx = BGCNT_SIZE(bgcnt);
    uint16_t bg_size = affine_bg_size[size_idx];
    bool wrap = BGCNT_OVERFLOW(bgcnt);

    /* Get current reference point and parameters */
    int32_t ref_x, ref_y;
    int16_t pa, pb, pc, pd;

    if (bg == 2) {
        ref_x = ppu->bg2_x;
        ref_y = ppu->bg2_y;
        pa = *(int16_t *)&mem->io[0x020];
        pb = *(int16_t *)&mem->io[0x022];
        pc = *(int16_t *)&mem->io[0x024];
        pd = *(int16_t *)&mem->io[0x026];
    } else { /* bg == 3 */
        ref_x = ppu->bg3_x;
        ref_y = ppu->bg3_y;
        pa = *(int16_t *)&mem->io[0x030];
        pb = *(int16_t *)&mem->io[0x032];
        pc = *(int16_t *)&mem->io[0x034];
        pd = *(int16_t *)&mem->io[0x036];
    }

    int32_t cx = ref_x;
    int32_t cy = ref_y;

    for (int x = 0; x < GBA_SCREEN_W; x++) {
        int32_t tx = cx >> 8;
        int32_t ty = cy >> 8;

        cx += pa;
        cy += pc;

        if (wrap) {
            tx = ((tx % bg_size) + bg_size) % bg_size;
            ty = ((ty % bg_size) + bg_size) % bg_size;
        } else if (tx < 0 || tx >= bg_size || ty < 0 || ty >= bg_size) {
            ppu->bg_pixel_valid[bg][x] = false;
            continue;
        }

        uint32_t tile_x = tx / 8;
        uint32_t tile_y = ty / 8;
        uint32_t fine_x = tx % 8;
        uint32_t fine_y = ty % 8;
        uint32_t tiles_per_row = bg_size / 8;

        uint8_t tile_num = mem->vram[(map_base + tile_y * tiles_per_row + tile_x) & 0x17FFF];
        uint32_t pixel_addr = tile_base + tile_num * 64 + fine_y * 8 + fine_x;
        uint8_t pixel = mem->vram[pixel_addr & 0x17FFF];

        if (pixel == 0) {
            ppu->bg_pixel_valid[bg][x] = false;
            continue;
        }

        ppu->bg_line[bg][x] = palette_read(mem, pixel);
        ppu->bg_pixel_valid[bg][x] = true;
    }

    /* Update reference points for next scanline */
    if (bg == 2) {
        ppu->bg2_x += pb;
        ppu->bg2_y += pd;
    } else {
        ppu->bg3_x += pb;
        ppu->bg3_y += pd;
    }

    ppu->bg_priority[bg] = BGCNT_PRIORITY(bgcnt);
}

/* ---------- Render bitmap BG (modes 3-5) ---------- */

static void render_bitmap_bg(GbaPPU *ppu, uint16_t line) {
    GbaMemory *mem = ppu->mem;
    uint16_t dispcnt = get_dispcnt(mem);
    uint8_t mode = DISPCNT_MODE(dispcnt);
    uint8_t frame = DISPCNT_FRAME_SEL(dispcnt);

    /* Use affine parameters for BG2 */
    int16_t pa = *(int16_t *)&mem->io[0x020];
    int16_t pc = *(int16_t *)&mem->io[0x024];
    int16_t pb = *(int16_t *)&mem->io[0x022];
    int16_t pd = *(int16_t *)&mem->io[0x026];

    int32_t cx = ppu->bg2_x;
    int32_t cy = ppu->bg2_y;

    for (int x = 0; x < GBA_SCREEN_W; x++) {
        int32_t tx = cx >> 8;
        int32_t ty = cy >> 8;
        cx += pa;
        cy += pc;

        uint16_t color = 0;
        bool valid = false;

        switch (mode) {
            case 3: /* 240x160, 16bpp */
                if (tx >= 0 && tx < 240 && ty >= 0 && ty < 160) {
                    uint32_t addr = (ty * 240 + tx) * 2;
                    color = *(uint16_t *)&mem->vram[addr];
                    valid = true;
                }
                break;

            case 4: /* 240x160, 8bpp, double buffered */
                if (tx >= 0 && tx < 240 && ty >= 0 && ty < 160) {
                    uint32_t base = frame ? 0xA000 : 0;
                    uint8_t pixel = mem->vram[base + ty * 240 + tx];
                    if (pixel) {
                        color = palette_read(mem, pixel);
                        valid = true;
                    }
                }
                break;

            case 5: /* 160x128, 16bpp, double buffered */
                if (tx >= 0 && tx < 160 && ty >= 0 && ty < 128) {
                    uint32_t base = frame ? 0xA000 : 0;
                    uint32_t addr = base + (ty * 160 + tx) * 2;
                    color = *(uint16_t *)&mem->vram[addr];
                    valid = true;
                }
                break;
        }

        ppu->bg_line[2][x] = color;
        ppu->bg_pixel_valid[2][x] = valid;
    }

    /* Update for next line */
    ppu->bg2_x += pb;
    ppu->bg2_y += pd;

    ppu->bg_priority[2] = BGCNT_PRIORITY(get_bgcnt(mem, 2));
}

/* ---------- Render sprites ---------- */

static void render_sprites(GbaPPU *ppu, uint16_t line) {
    GbaMemory *mem = ppu->mem;
    uint16_t dispcnt = get_dispcnt(mem);
    bool obj_1d = DISPCNT_OBJ_MAPPING(dispcnt);

    memset(ppu->obj_pixel_valid, 0, sizeof(ppu->obj_pixel_valid));
    memset(ppu->obj_semi_trans, 0, sizeof(ppu->obj_semi_trans));

    /* Iterate all 128 OAM entries (lower index = higher priority) */
    for (int i = 0; i < 128; i++) {
        uint16_t attr0 = *(uint16_t *)&mem->oam[i * 8 + 0];
        uint16_t attr1 = *(uint16_t *)&mem->oam[i * 8 + 2];
        uint16_t attr2 = *(uint16_t *)&mem->oam[i * 8 + 4];

        uint8_t gfx_mode = OBJ_ATTR0_GFX(attr0);
        if (gfx_mode == 2) continue; /* Disabled */

        uint8_t obj_mode = OBJ_ATTR0_MODE(attr0);
        uint8_t shape = OBJ_ATTR0_SHAPE(attr0);
        uint8_t obj_size = OBJ_ATTR1_SIZE(attr1);
        bool color256 = OBJ_ATTR0_COLOR(attr0);
        bool affine = (gfx_mode == 1 || gfx_mode == 3);
        bool double_size = (gfx_mode == 3);

        uint16_t tile_num = OBJ_ATTR2_TILE(attr2);
        uint8_t priority = OBJ_ATTR2_PRIORITY(attr2);
        uint8_t palette_bank = OBJ_ATTR2_PALETTE(attr2);

        int w = obj_width[shape][obj_size];
        int h = obj_height[shape][obj_size];
        if (w == 0 || h == 0) continue;

        int bound_w = double_size ? w * 2 : w;
        int bound_h = double_size ? h * 2 : h;

        int obj_y = OBJ_ATTR0_Y(attr0);
        if (obj_y >= 160) obj_y -= 256;
        int obj_x = OBJ_ATTR1_X(attr1);
        if (obj_x >= 240) obj_x -= 512;

        /* Check if sprite is on this scanline */
        int local_y = line - obj_y;
        if (local_y < 0 || local_y >= bound_h) continue;

        if (affine) {
            /* Get affine parameters from OAM */
            uint8_t affine_idx = OBJ_ATTR1_AFFINE_IDX(attr1);
            int16_t opa = *(int16_t *)&mem->oam[affine_idx * 32 + 0x06];
            int16_t opb = *(int16_t *)&mem->oam[affine_idx * 32 + 0x0E];
            int16_t opc = *(int16_t *)&mem->oam[affine_idx * 32 + 0x16];
            int16_t opd = *(int16_t *)&mem->oam[affine_idx * 32 + 0x1E];

            int half_w = bound_w / 2;
            int half_h = bound_h / 2;

            for (int sx = 0; sx < bound_w; sx++) {
                int screen_x = obj_x + sx;
                if (screen_x < 0 || screen_x >= GBA_SCREEN_W) continue;
                if (ppu->obj_pixel_valid[screen_x]) continue; /* Lower OAM index wins */

                int dx = sx - half_w;
                int dy = local_y - half_h;

                int tex_x = ((opa * dx + opb * dy) >> 8) + w / 2;
                int tex_y = ((opc * dx + opd * dy) >> 8) + h / 2;

                if (tex_x < 0 || tex_x >= w || tex_y < 0 || tex_y >= h) continue;

                uint8_t pixel;
                if (color256) {
                    uint32_t tile_row_size = 8;
                    uint32_t t_x = tex_x / 8;
                    uint32_t t_y = tex_y / 8;
                    uint32_t f_x = tex_x % 8;
                    uint32_t f_y = tex_y % 8;
                    uint32_t tile;
                    if (obj_1d) {
                        tile = tile_num + t_y * (w / 4) + t_x * 2;
                    } else {
                        tile = tile_num + t_y * 32 + t_x * 2;
                    }
                    uint32_t addr = 0x10000 + tile * 32 + f_y * tile_row_size + f_x;
                    pixel = mem->vram[addr & 0x17FFF];
                } else {
                    uint32_t t_x = tex_x / 8;
                    uint32_t t_y = tex_y / 8;
                    uint32_t f_x = tex_x % 8;
                    uint32_t f_y = tex_y % 8;
                    uint32_t tile;
                    if (obj_1d) {
                        tile = tile_num + t_y * (w / 8) + t_x;
                    } else {
                        tile = tile_num + t_y * 32 + t_x;
                    }
                    uint32_t addr = 0x10000 + tile * 32 + f_y * 4 + f_x / 2;
                    uint8_t byte = mem->vram[addr & 0x17FFF];
                    pixel = (f_x & 1) ? (byte >> 4) : (byte & 0xF);
                }

                if (pixel == 0) continue;

                uint16_t color;
                if (color256) {
                    color = palette_read(mem, 256 + pixel);
                } else {
                    color = palette_read(mem, 256 + palette_bank * 16 + pixel);
                }

                ppu->obj_line[screen_x] = color;
                ppu->obj_prio[screen_x] = priority;
                ppu->obj_pixel_valid[screen_x] = true;
                ppu->obj_semi_trans[screen_x] = (obj_mode == 1);
            }
        } else {
            /* Regular (non-affine) sprite */
            bool h_flip = OBJ_ATTR1_HFLIP(attr1);
            bool v_flip = OBJ_ATTR1_VFLIP(attr1);

            int tex_y = v_flip ? (h - 1 - local_y) : local_y;

            for (int sx = 0; sx < w; sx++) {
                int screen_x = obj_x + sx;
                if (screen_x < 0 || screen_x >= GBA_SCREEN_W) continue;
                if (ppu->obj_pixel_valid[screen_x]) continue;

                int tex_x = h_flip ? (w - 1 - sx) : sx;

                uint8_t pixel;
                uint32_t t_x = tex_x / 8;
                uint32_t t_y = tex_y / 8;
                uint32_t f_x = tex_x % 8;
                uint32_t f_y = tex_y % 8;

                if (color256) {
                    uint32_t tile;
                    if (obj_1d) {
                        tile = tile_num + t_y * (w / 4) + t_x * 2;
                    } else {
                        tile = tile_num + t_y * 32 + t_x * 2;
                    }
                    uint32_t addr = 0x10000 + tile * 32 + f_y * 8 + f_x;
                    pixel = mem->vram[addr & 0x17FFF];
                } else {
                    uint32_t tile;
                    if (obj_1d) {
                        tile = tile_num + t_y * (w / 8) + t_x;
                    } else {
                        tile = tile_num + t_y * 32 + t_x;
                    }
                    uint32_t addr = 0x10000 + tile * 32 + f_y * 4 + f_x / 2;
                    uint8_t byte = mem->vram[addr & 0x17FFF];
                    pixel = (f_x & 1) ? (byte >> 4) : (byte & 0xF);
                }

                if (pixel == 0) continue;

                uint16_t color;
                if (color256) {
                    color = palette_read(mem, 256 + pixel);
                } else {
                    color = palette_read(mem, 256 + palette_bank * 16 + pixel);
                }

                ppu->obj_line[screen_x] = color;
                ppu->obj_prio[screen_x] = priority;
                ppu->obj_pixel_valid[screen_x] = true;
                ppu->obj_semi_trans[screen_x] = (obj_mode == 1);
            }
        }
    }
}

/* ---------- Compose scanline with priority and blending ---------- */

static void compose_scanline(GbaPPU *ppu, uint16_t line) {
    GbaMemory *mem = ppu->mem;
    uint16_t dispcnt = get_dispcnt(mem);
    uint16_t bldcnt = *(uint16_t *)&mem->io[0x050];
    uint16_t bldalpha = *(uint16_t *)&mem->io[0x052];
    uint16_t bldy = *(uint16_t *)&mem->io[0x054];

    uint8_t blend_mode = (bldcnt >> 6) & 3;
    uint8_t first_targets = bldcnt & 0x3F;
    uint8_t second_targets = (bldcnt >> 8) & 0x3F;
    uint8_t eva = bldalpha & 0x1F;
    if (eva > 16) eva = 16;
    uint8_t evb = (bldalpha >> 8) & 0x1F;
    if (evb > 16) evb = 16;
    uint8_t evy = bldy & 0x1F;
    if (evy > 16) evy = 16;

    uint16_t backdrop = palette_read(mem, 0);
    uint8_t *fb = &ppu->framebuffer[line * GBA_SCREEN_W * 4];

    /* Check which BGs are enabled */
    bool bg_enabled[4] = {
        DISPCNT_BG0_EN(dispcnt) != 0,
        DISPCNT_BG1_EN(dispcnt) != 0,
        DISPCNT_BG2_EN(dispcnt) != 0,
        DISPCNT_BG3_EN(dispcnt) != 0
    };
    bool obj_enabled = DISPCNT_OBJ_EN(dispcnt) != 0;

    for (int x = 0; x < GBA_SCREEN_W; x++) {
        /* Find top two layers by priority */
        uint16_t top_color = backdrop;
        uint8_t top_layer = LAYER_BD;
        uint16_t second_color = backdrop;
        uint8_t second_layer = LAYER_BD;
        uint8_t top_prio = 4; /* Backdrop is lowest */
        uint8_t second_prio = 4;

        /* Check BGs */
        for (int bg = 0; bg < 4; bg++) {
            if (!bg_enabled[bg] || !ppu->bg_pixel_valid[bg][x]) continue;
            uint8_t p = ppu->bg_priority[bg];
            if (p < top_prio || (p == top_prio && bg < top_layer)) {
                second_color = top_color;
                second_layer = top_layer;
                second_prio = top_prio;
                top_color = ppu->bg_line[bg][x];
                top_layer = bg;
                top_prio = p;
            } else if (p < second_prio || (p == second_prio && bg < second_layer)) {
                second_color = ppu->bg_line[bg][x];
                second_layer = bg;
                second_prio = p;
            }
        }

        /* Check OBJ */
        if (obj_enabled && ppu->obj_pixel_valid[x]) {
            uint8_t p = ppu->obj_prio[x];
            if (p <= top_prio) {
                second_color = top_color;
                second_layer = top_layer;
                second_prio = top_prio;
                top_color = ppu->obj_line[x];
                top_layer = LAYER_OBJ;
                top_prio = p;
            } else if (p <= second_prio) {
                second_color = ppu->obj_line[x];
                second_layer = LAYER_OBJ;
                second_prio = p;
            }
        }

        /* Apply blending */
        uint16_t final_color = top_color;
        bool do_blend = false;

        /* Semi-transparent OBJ forces alpha blending */
        if (top_layer == LAYER_OBJ && ppu->obj_semi_trans[x]) {
            if (second_targets & (1 << second_layer)) {
                do_blend = true;
            }
        } else if (blend_mode == BLEND_ALPHA && (first_targets & (1 << top_layer))
                   && (second_targets & (1 << second_layer))) {
            do_blend = true;
        }

        if (do_blend) {
            /* Alpha blend */
            uint8_t r1 = (top_color & 0x1F);
            uint8_t g1 = ((top_color >> 5) & 0x1F);
            uint8_t b1 = ((top_color >> 10) & 0x1F);
            uint8_t r2 = (second_color & 0x1F);
            uint8_t g2 = ((second_color >> 5) & 0x1F);
            uint8_t b2 = ((second_color >> 10) & 0x1F);

            uint8_t r = (r1 * eva + r2 * evb) >> 4;
            uint8_t g = (g1 * eva + g2 * evb) >> 4;
            uint8_t b = (b1 * eva + b2 * evb) >> 4;
            if (r > 31) r = 31;
            if (g > 31) g = 31;
            if (b > 31) b = 31;

            final_color = r | (g << 5) | (b << 10);
        } else if (blend_mode == BLEND_BRIGHTEN && (first_targets & (1 << top_layer))) {
            uint8_t r = (top_color & 0x1F);
            uint8_t g = ((top_color >> 5) & 0x1F);
            uint8_t b = ((top_color >> 10) & 0x1F);
            r += (31 - r) * evy / 16;
            g += (31 - g) * evy / 16;
            b += (31 - b) * evy / 16;
            final_color = r | (g << 5) | (b << 10);
        } else if (blend_mode == BLEND_DARKEN && (first_targets & (1 << top_layer))) {
            uint8_t r = (top_color & 0x1F);
            uint8_t g = ((top_color >> 5) & 0x1F);
            uint8_t b = ((top_color >> 10) & 0x1F);
            r -= r * evy / 16;
            g -= g * evy / 16;
            b -= b * evy / 16;
            final_color = r | (g << 5) | (b << 10);
        }

        bgr555_to_rgba(final_color, &fb[x * 4]);
    }
}

/* ---------- Render full scanline ---------- */

static void render_scanline(GbaPPU *ppu) {
    GbaMemory *mem = ppu->mem;
    uint16_t dispcnt = get_dispcnt(mem);
    uint16_t line = ppu->line;

    /* Forced blank: white */
    if (DISPCNT_FORCED_BLANK(dispcnt)) {
        uint8_t *fb = &ppu->framebuffer[line * GBA_SCREEN_W * 4];
        memset(fb, 0xFF, GBA_SCREEN_W * 4);
        return;
    }

    /* Clear per-line buffers */
    for (int bg = 0; bg < 4; bg++) {
        memset(ppu->bg_pixel_valid[bg], 0, GBA_SCREEN_W);
    }

    uint8_t mode = DISPCNT_MODE(dispcnt);

    switch (mode) {
        case 0: /* 4 text BGs */
            if (DISPCNT_BG0_EN(dispcnt)) render_text_bg(ppu, 0, line);
            if (DISPCNT_BG1_EN(dispcnt)) render_text_bg(ppu, 1, line);
            if (DISPCNT_BG2_EN(dispcnt)) render_text_bg(ppu, 2, line);
            if (DISPCNT_BG3_EN(dispcnt)) render_text_bg(ppu, 3, line);
            break;

        case 1: /* 2 text + 1 affine */
            if (DISPCNT_BG0_EN(dispcnt)) render_text_bg(ppu, 0, line);
            if (DISPCNT_BG1_EN(dispcnt)) render_text_bg(ppu, 1, line);
            if (DISPCNT_BG2_EN(dispcnt)) render_affine_bg(ppu, 2, line);
            break;

        case 2: /* 2 affine */
            if (DISPCNT_BG2_EN(dispcnt)) render_affine_bg(ppu, 2, line);
            if (DISPCNT_BG3_EN(dispcnt)) render_affine_bg(ppu, 3, line);
            break;

        case 3: case 4: case 5: /* Bitmap modes */
            if (DISPCNT_BG2_EN(dispcnt)) render_bitmap_bg(ppu, line);
            break;
    }

    /* Sprites */
    if (DISPCNT_OBJ_EN(dispcnt)) {
        render_sprites(ppu, line);
    }

    /* Priority composition + blending */
    compose_scanline(ppu, line);
}

/* ---------- PPU step (called per CPU cycle batch) ---------- */

void gba_ppu_step(GbaPPU *ppu, uint32_t cycles) {
    GbaMemory *mem = ppu->mem;
    if (!mem) return;

    ppu->cycle += cycles;

    while (ppu->cycle >= GBA_LINE_CYCLES) {
        ppu->cycle -= GBA_LINE_CYCLES;

        /* Render visible line */
        if (ppu->line < GBA_SCREEN_H) {
            render_scanline(ppu);
        }

        ppu->line++;

        if (ppu->line == GBA_SCREEN_H) {
            /* Enter VBlank */
            ppu->in_vblank = true;
            uint16_t stat = get_dispstat(mem);
            set_dispstat(mem, stat | DISPSTAT_VBLANK);

            if (stat & DISPSTAT_VBLANK_IRQ) {
                gba_request_interrupt(mem, GBA_INT_VBLANK);
            }

            /* Trigger VBlank DMA */
            gba_dma_trigger(mem, DMA_TIMING_VBLANK);

            /* Latch affine reference points */
            ppu->bg2_ref_x = sign_extend_28(
                *(uint32_t *)&mem->io[0x028] & 0x0FFFFFFF);
            ppu->bg2_ref_y = sign_extend_28(
                *(uint32_t *)&mem->io[0x02C] & 0x0FFFFFFF);
            ppu->bg3_ref_x = sign_extend_28(
                *(uint32_t *)&mem->io[0x038] & 0x0FFFFFFF);
            ppu->bg3_ref_y = sign_extend_28(
                *(uint32_t *)&mem->io[0x03C] & 0x0FFFFFFF);
            ppu->bg2_x = ppu->bg2_ref_x;
            ppu->bg2_y = ppu->bg2_ref_y;
            ppu->bg3_x = ppu->bg3_ref_x;
            ppu->bg3_y = ppu->bg3_ref_y;

            ppu->frame_ready = true;
        }

        if (ppu->line >= GBA_SCANLINES) {
            ppu->line = 0;
            ppu->in_vblank = false;
            uint16_t stat = get_dispstat(mem);
            set_dispstat(mem, stat & ~DISPSTAT_VBLANK);
        }

        /* Update VCOUNT */
        set_vcount(mem, ppu->line);

        /* VCOUNT match check */
        uint16_t dispstat = get_dispstat(mem);
        uint8_t lyc = (dispstat >> 8) & 0xFF;
        if (ppu->line == lyc) {
            set_dispstat(mem, dispstat | DISPSTAT_VCOUNTER);
            if (dispstat & DISPSTAT_VCOUNTER_IRQ) {
                gba_request_interrupt(mem, GBA_INT_VCOUNT);
            }
        } else {
            set_dispstat(mem, dispstat & ~DISPSTAT_VCOUNTER);
        }

        /* HBlank occurs during visible lines */
        if (ppu->line < GBA_SCREEN_H) {
            uint16_t s = get_dispstat(mem);
            set_dispstat(mem, s | DISPSTAT_HBLANK);
            if (s & DISPSTAT_HBLANK_IRQ) {
                gba_request_interrupt(mem, GBA_INT_HBLANK);
            }
            gba_dma_trigger(mem, DMA_TIMING_HBLANK);
        }
    }
}

/* ---------- Serialization ---------- */

size_t gba_ppu_serialize(const GbaPPU *ppu, uint8_t *buf, size_t buf_size) {
    size_t needed = sizeof(ppu->cycle) + sizeof(ppu->line)
                  + sizeof(ppu->in_hblank) + sizeof(ppu->in_vblank)
                  + sizeof(ppu->bg2_ref_x) * 8 + sizeof(ppu->frame_ready) + 32;
    if (!buf || buf_size < needed) return needed;

    size_t pos = 0;
    #define S(f) memcpy(buf+pos, &ppu->f, sizeof(ppu->f)); pos += sizeof(ppu->f)
    S(cycle); S(line); S(in_hblank); S(in_vblank);
    S(bg2_ref_x); S(bg2_ref_y); S(bg3_ref_x); S(bg3_ref_y);
    S(bg2_x); S(bg2_y); S(bg3_x); S(bg3_y);
    S(frame_ready);
    #undef S
    return pos;
}

size_t gba_ppu_deserialize(GbaPPU *ppu, const uint8_t *buf, size_t buf_size) {
    size_t pos = 0;
    #define L(f) do { if(pos+sizeof(ppu->f)>buf_size) return 0; \
        memcpy(&ppu->f, buf+pos, sizeof(ppu->f)); pos += sizeof(ppu->f); } while(0)
    L(cycle); L(line); L(in_hblank); L(in_vblank);
    L(bg2_ref_x); L(bg2_ref_y); L(bg3_ref_x); L(bg3_ref_y);
    L(bg2_x); L(bg2_y); L(bg3_x); L(bg3_y);
    L(frame_ready);
    #undef L
    return pos;
}
