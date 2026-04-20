/*
 * Save State Implementation
 *
 * Format:
 *   [4 bytes] magic (STATE_MAGIC)
 *   [4 bytes] version (STATE_VERSION)
 *   [1 byte]  mode (GbpyMode)
 *   Repeated chunks:
 *     [1 byte]  chunk_id
 *     [4 bytes] chunk_size (little-endian)
 *     [N bytes] chunk_data
 *   [1 byte] CHUNK_END
 */

#include "state.h"
#include "emulator.h"
#include <stdio.h>
#include <string.h>

/* ---------- Helpers ---------- */

static void write_u32(uint8_t *buf, uint32_t val) {
    buf[0] = (uint8_t)(val);
    buf[1] = (uint8_t)(val >> 8);
    buf[2] = (uint8_t)(val >> 16);
    buf[3] = (uint8_t)(val >> 24);
}

static uint32_t read_u32(const uint8_t *buf) {
    return (uint32_t)buf[0] |
           ((uint32_t)buf[1] << 8) |
           ((uint32_t)buf[2] << 16) |
           ((uint32_t)buf[3] << 24);
}

/* Write a chunk: id + size + data. Returns bytes written, 0 on failure. */
static size_t write_chunk(uint8_t *buf, size_t buf_size, uint8_t chunk_id,
                          const uint8_t *data, size_t data_size) {
    size_t needed = 1 + 4 + data_size;  /* id + size + data */
    if (buf_size < needed) return 0;

    buf[0] = chunk_id;
    write_u32(buf + 1, (uint32_t)data_size);
    if (data_size > 0) {
        memcpy(buf + 5, data, data_size);
    }
    return needed;
}

/* ---------- Save to buffer ---------- */

size_t state_save_to_buffer(const Emulator *emu, uint8_t *buf, size_t buf_size) {
    /* Temporary chunk buffer (1MB should be enough for any subsystem) */
    uint8_t *chunk_buf = (uint8_t *)malloc(1024 * 1024);
    if (!chunk_buf) return 0;

    size_t pos = 0;

    /* Header: magic + version + mode */
    if (buf_size < 9) { free(chunk_buf); return 0; }
    write_u32(buf + pos, STATE_MAGIC); pos += 4;
    write_u32(buf + pos, STATE_VERSION); pos += 4;
    buf[pos++] = (uint8_t)emu->mode;

    /* Serialize each subsystem as a chunk */
    size_t chunk_size;
    size_t written;

#define SAVE_CHUNK(id, serialize_fn, obj) \
    chunk_size = serialize_fn(obj, chunk_buf, 1024 * 1024); \
    if (chunk_size > 0) { \
        written = write_chunk(buf + pos, buf_size - pos, id, chunk_buf, chunk_size); \
        if (written == 0) { free(chunk_buf); return 0; } \
        pos += written; \
    }

    /* Cartridge */
    SAVE_CHUNK(CHUNK_CART, cartridge_serialize, &emu->cart);

    switch (emu->mode) {
        case GBPY_MODE_GB:
        case GBPY_MODE_GBC:
            SAVE_CHUNK(CHUNK_SM83,   sm83_serialize,    &emu->cpu_sm83);
            SAVE_CHUNK(CHUNK_MMU,    mmu_serialize,     &emu->mmu);
            SAVE_CHUNK(CHUNK_MBC,    mbc_serialize,     &emu->mbc);
            SAVE_CHUNK(CHUNK_GB_PPU, gb_ppu_serialize,  &emu->gb_ppu);
            SAVE_CHUNK(CHUNK_APU,    apu_serialize,     &emu->gb_apu);
            SAVE_CHUNK(CHUNK_TIMER,  timer_serialize,   &emu->gb_timer);
            break;
        case GBPY_MODE_GBA:
            SAVE_CHUNK(CHUNK_ARM7,    arm7_serialize,      &emu->cpu_arm7);
            SAVE_CHUNK(CHUNK_GBA_MEM, gba_mem_serialize,   &emu->gba_mem);
            SAVE_CHUNK(CHUNK_GBA_PPU, gba_ppu_serialize,   &emu->gba_ppu);
            SAVE_CHUNK(CHUNK_GBA_APU, gba_apu_serialize,   &emu->gba_apu);
            break;
    }

#undef SAVE_CHUNK

    /* End marker */
    if (pos < buf_size) {
        buf[pos++] = CHUNK_END;
    }

    free(chunk_buf);
    return pos;
}

/* ---------- Load from buffer ---------- */

int state_load_from_buffer(Emulator *emu, const uint8_t *buf, size_t size) {
    if (size < 9) return GBPY_ERR_IO;

    size_t pos = 0;

    /* Verify magic */
    uint32_t magic = read_u32(buf + pos); pos += 4;
    if (magic != STATE_MAGIC) return GBPY_ERR_IO;

    /* Verify version */
    uint32_t version = read_u32(buf + pos); pos += 4;
    if (version != STATE_VERSION) return GBPY_ERR_IO;

    /* Mode */
    GbpyMode mode = (GbpyMode)buf[pos++];
    emu->mode = mode;

    /* Read chunks */
    while (pos < size) {
        uint8_t chunk_id = buf[pos++];
        if (chunk_id == CHUNK_END) break;
        if (pos + 4 > size) return GBPY_ERR_IO;

        uint32_t chunk_size = read_u32(buf + pos); pos += 4;
        if (pos + chunk_size > size) return GBPY_ERR_IO;

        const uint8_t *chunk_data = buf + pos;

        switch (chunk_id) {
            case CHUNK_CART:
                cartridge_deserialize(&emu->cart, chunk_data, chunk_size);
                break;
            case CHUNK_SM83:
                sm83_deserialize(&emu->cpu_sm83, chunk_data, chunk_size);
                break;
            case CHUNK_MMU:
                mmu_deserialize(&emu->mmu, chunk_data, chunk_size);
                break;
            case CHUNK_MBC:
                mbc_deserialize(&emu->mbc, chunk_data, chunk_size);
                break;
            case CHUNK_GB_PPU:
                gb_ppu_deserialize(&emu->gb_ppu, chunk_data, chunk_size);
                break;
            case CHUNK_APU:
                apu_deserialize(&emu->gb_apu, chunk_data, chunk_size);
                break;
            case CHUNK_TIMER:
                timer_deserialize(&emu->gb_timer, chunk_data, chunk_size);
                break;
            case CHUNK_ARM7:
                arm7_deserialize(&emu->cpu_arm7, chunk_data, chunk_size);
                break;
            case CHUNK_GBA_MEM:
                gba_mem_deserialize(&emu->gba_mem, chunk_data, chunk_size);
                break;
            case CHUNK_GBA_PPU:
                gba_ppu_deserialize(&emu->gba_ppu, chunk_data, chunk_size);
                break;
            case CHUNK_GBA_APU:
                gba_apu_deserialize(&emu->gba_apu, chunk_data, chunk_size);
                break;
            default:
                /* Unknown chunk — skip */
                break;
        }

        pos += chunk_size;
    }

    /* Re-wire subsystem pointers after deserialization */
    switch (emu->mode) {
        case GBPY_MODE_GB:
        case GBPY_MODE_GBC:
            emu->mmu.mbc = &emu->mbc;
            emu->mmu.ppu = &emu->gb_ppu;
            emu->mmu.apu = &emu->gb_apu;
            emu->mmu.timer = &emu->gb_timer;
            emu->mmu.input = &emu->gb_input;
            emu->gb_ppu.vram = emu->mmu.vram;
            emu->gb_ppu.oam  = emu->mmu.oam;
            emu->gb_ppu.io   = emu->mmu.io;
            emu->gb_apu.io   = emu->mmu.io;
            emu->cpu_sm83.read = mmu_read;
            emu->cpu_sm83.write = mmu_write;
            emu->cpu_sm83.mem_ctx = &emu->mmu;
            break;
        case GBPY_MODE_GBA:
            emu->gba_mem.cpu = &emu->cpu_arm7;
            emu->gba_mem.ppu = &emu->gba_ppu;
            emu->gba_mem.apu = &emu->gba_apu;
            emu->gba_ppu.mem = &emu->gba_mem;
            emu->gba_apu.mem = &emu->gba_mem;
            emu->cpu_arm7.read8  = gba_mem_read8;
            emu->cpu_arm7.read16 = gba_mem_read16;
            emu->cpu_arm7.read32 = gba_mem_read32;
            emu->cpu_arm7.write8  = gba_mem_write8;
            emu->cpu_arm7.write16 = gba_mem_write16;
            emu->cpu_arm7.write32 = gba_mem_write32;
            emu->cpu_arm7.mem_ctx = &emu->gba_mem;
            break;
    }

    emu->running = true;
    return GBPY_OK;
}

/* ---------- File save/load ---------- */

int state_save(const Emulator *emu, const char *path) {
    /* Allocate a generous buffer (4MB) */
    size_t buf_size = 4 * 1024 * 1024;
    uint8_t *buf = (uint8_t *)malloc(buf_size);
    if (!buf) return GBPY_ERR_IO;

    size_t written = state_save_to_buffer(emu, buf, buf_size);
    if (written == 0) {
        free(buf);
        return GBPY_ERR_IO;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        free(buf);
        return GBPY_ERR_IO;
    }

    size_t n = fwrite(buf, 1, written, f);
    fclose(f);
    free(buf);

    return (n == written) ? GBPY_OK : GBPY_ERR_IO;
}

int state_load(Emulator *emu, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return GBPY_ERR_IO;

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size <= 0 || file_size > 16 * 1024 * 1024) {
        fclose(f);
        return GBPY_ERR_IO;
    }

    uint8_t *buf = (uint8_t *)malloc((size_t)file_size);
    if (!buf) {
        fclose(f);
        return GBPY_ERR_IO;
    }

    size_t n = fread(buf, 1, (size_t)file_size, f);
    fclose(f);

    if (n != (size_t)file_size) {
        free(buf);
        return GBPY_ERR_IO;
    }

    int result = state_load_from_buffer(emu, buf, n);
    free(buf);
    return result;
}
