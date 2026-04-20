/*
 * Cartridge Implementation
 *
 * Handles loading and parsing GB/GBC and GBA ROMs,
 * detecting ROM type, extracting headers, and managing save RAM.
 */

#include "cartridge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- ROM size tables ---------- */

static const uint32_t gb_rom_sizes[] = {
    32*1024, 64*1024, 128*1024, 256*1024, 512*1024,
    1024*1024, 2048*1024, 4096*1024, 8192*1024
};

static const uint32_t gb_ram_sizes[] = {
    0, 0, 8*1024, 32*1024, 128*1024, 64*1024
};

/* MBC types with battery */
static bool gb_has_battery(uint8_t cart_type) {
    switch (cart_type) {
        case 0x03: /* MBC1+RAM+BATTERY */
        case 0x06: /* MBC2+BATTERY */
        case 0x09: /* ROM+RAM+BATTERY */
        case 0x0D: /* MMM01+RAM+BATTERY */
        case 0x0F: /* MBC3+TIMER+BATTERY */
        case 0x10: /* MBC3+TIMER+RAM+BATTERY */
        case 0x13: /* MBC3+RAM+BATTERY */
        case 0x1B: /* MBC5+RAM+BATTERY */
        case 0x1E: /* MBC5+RUMBLE+RAM+BATTERY */
        case 0xFF: /* HuC1+RAM+BATTERY */
            return true;
        default:
            return false;
    }
}

static bool gb_has_rtc(uint8_t cart_type) {
    return cart_type == 0x0F || cart_type == 0x10;
}

/* ---------- Type detection ---------- */

CartType cartridge_detect_type(const uint8_t *data, size_t size) {
    if (!data || size < 0x150) {
        if (size >= 0xC0) {
            /* Could be GBA - check for fixed value at 0xB2 */
            if (data[0xB2] == 0x96) return CART_TYPE_GBA;
        }
        return CART_TYPE_UNKNOWN;
    }

    /* Check GBA: fixed value 0x96 at offset 0xB2 */
    if (size >= 0xC0 && data[0xB2] == 0x96) {
        /* Additional check: entry point should be ARM branch (0xEA000000-ish) */
        uint32_t entry = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
        if ((entry & 0xFF000000) == 0xEA000000) {
            return CART_TYPE_GBA;
        }
        /* Also accept thumb/other entry patterns - the 0x96 byte is definitive */
        return CART_TYPE_GBA;
    }

    /* Check GB/GBC: Nintendo logo at 0x104-0x133 */
    static const uint8_t nintendo_logo_start[] = {
        0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B
    };
    if (memcmp(&data[0x104], nintendo_logo_start, 8) == 0) {
        /* CGB flag */
        uint8_t cgb = data[0x143];
        if (cgb == 0x80 || cgb == 0xC0) {
            return CART_TYPE_GBC;
        }
        return CART_TYPE_GB;
    }

    return CART_TYPE_UNKNOWN;
}

/* ---------- Parse GB/GBC header ---------- */

static void parse_gb_header(Cartridge *cart) {
    const uint8_t *rom = cart->rom;
    GbCartHeader *hdr = &cart->header.gb;

    /* Title (up to 16 bytes, but CGB uses some for other fields) */
    memcpy(hdr->title, &rom[0x134], 16);
    hdr->title[16] = '\0';

    hdr->cgb_flag = rom[0x143];
    hdr->new_licensee = rom[0x144] | (rom[0x145] << 8);
    hdr->sgb_flag = rom[0x146];
    hdr->cart_type = rom[0x147];
    hdr->rom_size_code = rom[0x148];
    hdr->ram_size_code = rom[0x149];
    hdr->dest_code = rom[0x14A];
    hdr->old_licensee = rom[0x14B];
    hdr->version = rom[0x14C];
    hdr->header_checksum = rom[0x14D];
    hdr->global_checksum = rom[0x14E] | (rom[0x14F] << 8);

    /* Calculate sizes */
    if (hdr->rom_size_code < sizeof(gb_rom_sizes) / sizeof(gb_rom_sizes[0])) {
        hdr->rom_size = gb_rom_sizes[hdr->rom_size_code];
    } else {
        hdr->rom_size = (uint32_t)cart->rom_size;
    }

    if (hdr->ram_size_code < sizeof(gb_ram_sizes) / sizeof(gb_ram_sizes[0])) {
        hdr->ram_size = gb_ram_sizes[hdr->ram_size_code];
    } else {
        hdr->ram_size = 0;
    }

    /* MBC2 always has 512 half-bytes of RAM */
    if (hdr->cart_type == 0x05 || hdr->cart_type == 0x06) {
        hdr->ram_size = 512;
    }

    cart->ram_size = hdr->ram_size;
    cart->has_battery = gb_has_battery(hdr->cart_type);
    cart->has_rtc = gb_has_rtc(hdr->cart_type);

    /* Allocate external RAM */
    if (cart->ram_size > 0) {
        cart->ram = (uint8_t *)calloc(1, cart->ram_size);
    }
}

/* ---------- Parse GBA header ---------- */

static void parse_gba_header(Cartridge *cart) {
    const uint8_t *rom = cart->rom;
    GbaCartHeader *hdr = &cart->header.gba;

    hdr->entry_point = rom[0] | (rom[1] << 8) | (rom[2] << 16) | (rom[3] << 24);
    memcpy(hdr->logo, &rom[4], 156);
    memcpy(hdr->title, &rom[0xA0], 12);
    hdr->title[12] = '\0';
    memcpy(hdr->game_code, &rom[0xAC], 4);
    hdr->game_code[4] = '\0';
    memcpy(hdr->maker_code, &rom[0xB0], 2);
    hdr->maker_code[2] = '\0';
    hdr->fixed_val = rom[0xB2];
    hdr->unit_code = rom[0xB3];
    hdr->device_type = rom[0xB4];
    hdr->version = rom[0xBC];
    hdr->complement = rom[0xBD];
    hdr->rom_size = (uint32_t)cart->rom_size;

    /* GBA SRAM/Flash: typically 32KB or 64KB */
    /* Detect by searching for strings in ROM */
    cart->has_battery = false;
    cart->ram_size = 0;

    /* Search for save type strings in the ROM */
    const char *sram_str = "SRAM_V";
    const char *flash_str = "FLASH_V";
    const char *flash512_str = "FLASH512_V";
    const char *flash1m_str = "FLASH1M_V";
    const char *eeprom_str = "EEPROM_V";

    for (size_t i = 0; i + 10 < cart->rom_size; i += 4) {
        if (memcmp(&rom[i], flash1m_str, 9) == 0) {
            cart->ram_size = 128 * 1024;
            cart->has_battery = true;
            break;
        }
        if (memcmp(&rom[i], flash512_str, 10) == 0) {
            cart->ram_size = 64 * 1024;
            cart->has_battery = true;
            break;
        }
        if (memcmp(&rom[i], flash_str, 7) == 0) {
            cart->ram_size = 64 * 1024;
            cart->has_battery = true;
            break;
        }
        if (memcmp(&rom[i], sram_str, 6) == 0) {
            cart->ram_size = 32 * 1024;
            cart->has_battery = true;
            break;
        }
        if (memcmp(&rom[i], eeprom_str, 8) == 0) {
            cart->ram_size = 8 * 1024; /* Default to 8KB EEPROM */
            cart->has_battery = true;
            break;
        }
    }

    if (cart->ram_size > 0) {
        cart->ram = (uint8_t *)calloc(1, cart->ram_size);
    }
}

/* ---------- Load ROM from data ---------- */

int cartridge_load(Cartridge *cart, const uint8_t *data, size_t size) {
    if (!cart || !data || size == 0) return GBPY_ERR_INVALID_ROM;

    memset(cart, 0, sizeof(Cartridge));

    cart->type = cartridge_detect_type(data, size);
    if (cart->type == CART_TYPE_UNKNOWN) return GBPY_ERR_INVALID_ROM;

    /* Copy ROM data */
    cart->rom = (uint8_t *)malloc(size);
    if (!cart->rom) return GBPY_ERR_INVALID_ROM;
    memcpy(cart->rom, data, size);
    cart->rom_size = size;

    /* Parse header based on type */
    switch (cart->type) {
        case CART_TYPE_GB:
        case CART_TYPE_GBC:
            parse_gb_header(cart);
            break;
        case CART_TYPE_GBA:
            parse_gba_header(cart);
            break;
        default:
            break;
    }

    return GBPY_OK;
}

/* ---------- Load ROM from file ---------- */

int cartridge_load_file(Cartridge *cart, const char *path) {
    if (!cart || !path) return GBPY_ERR_INVALID_ROM;

    FILE *f = fopen(path, "rb");
    if (!f) return GBPY_ERR_INVALID_ROM;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > 32 * 1024 * 1024) { /* Max 32MB */
        fclose(f);
        return GBPY_ERR_INVALID_ROM;
    }

    uint8_t *data = (uint8_t *)malloc(size);
    if (!data) {
        fclose(f);
        return GBPY_ERR_INVALID_ROM;
    }

    if (fread(data, 1, size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return GBPY_ERR_INVALID_ROM;
    }
    fclose(f);

    int result = cartridge_load(cart, data, size);
    free(data);

    if (result == GBPY_OK) {
        /* Build save path: replace extension with .sav */
        strncpy(cart->save_path, path, sizeof(cart->save_path) - 5);
        cart->save_path[sizeof(cart->save_path) - 5] = '\0';
        char *dot = strrchr(cart->save_path, '.');
        if (dot) {
            strcpy(dot, ".sav");
        } else {
            strcat(cart->save_path, ".sav");
        }
    }

    return result;
}

/* ---------- Free ---------- */

void cartridge_free(Cartridge *cart) {
    if (!cart) return;
    free(cart->rom);
    free(cart->ram);
    memset(cart, 0, sizeof(Cartridge));
}

/* ---------- Save/Load RAM ---------- */

int cartridge_save_ram(const Cartridge *cart, const char *path) {
    if (!cart || !cart->ram || cart->ram_size == 0) return GBPY_OK;
    const char *p = path ? path : cart->save_path;
    if (!p[0]) return GBPY_ERR_INVALID_ROM;

    FILE *f = fopen(p, "wb");
    if (!f) return GBPY_ERR_INVALID_ROM;
    fwrite(cart->ram, 1, cart->ram_size, f);
    fclose(f);
    return GBPY_OK;
}

int cartridge_load_save(Cartridge *cart, const char *path) {
    if (!cart || !cart->ram || cart->ram_size == 0) return GBPY_OK;
    const char *p = path ? path : cart->save_path;
    if (!p[0]) return GBPY_OK;

    FILE *f = fopen(p, "rb");
    if (!f) return GBPY_OK; /* No save file is not an error */
    fread(cart->ram, 1, cart->ram_size, f);
    fclose(f);
    return GBPY_OK;
}

/* ---------- Serialization ---------- */

size_t cartridge_serialize(const Cartridge *cart, uint8_t *buf, size_t buf_size) {
    size_t needed = sizeof(cart->type) + sizeof(cart->ram_size) + cart->ram_size + 4;
    if (!buf || buf_size < needed) return needed;

    size_t pos = 0;
    memcpy(buf + pos, &cart->type, sizeof(cart->type)); pos += sizeof(cart->type);

    uint32_t rs = (uint32_t)cart->ram_size;
    memcpy(buf + pos, &rs, 4); pos += 4;

    if (cart->ram && cart->ram_size > 0) {
        memcpy(buf + pos, cart->ram, cart->ram_size);
        pos += cart->ram_size;
    }
    return pos;
}

size_t cartridge_deserialize(Cartridge *cart, const uint8_t *buf, size_t buf_size) {
    if (!cart || !buf) return 0;
    size_t pos = 0;

    if (pos + sizeof(cart->type) > buf_size) return 0;
    memcpy(&cart->type, buf + pos, sizeof(cart->type)); pos += sizeof(cart->type);

    uint32_t rs;
    if (pos + 4 > buf_size) return 0;
    memcpy(&rs, buf + pos, 4); pos += 4;

    if (cart->ram && rs == (uint32_t)cart->ram_size && rs > 0) {
        if (pos + rs > buf_size) return 0;
        memcpy(cart->ram, buf + pos, rs);
        pos += rs;
    }
    return pos;
}
