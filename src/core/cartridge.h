/*
 * Cartridge Header - ROM loader for GB/GBC and GBA
 *
 * Handles header parsing, ROM type detection, and MBC selection.
 */

#ifndef GBPY_CARTRIDGE_H
#define GBPY_CARTRIDGE_H

#include "types.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ROM type detection */
typedef enum {
    CART_TYPE_UNKNOWN = 0,
    CART_TYPE_GB,
    CART_TYPE_GBC,
    CART_TYPE_GBA
} CartType;

/* GB/GBC cartridge header (at 0x100-0x14F) */
typedef struct {
    char title[17];         /* 0x134-0x143 (null-terminated) */
    uint8_t cgb_flag;       /* 0x143 */
    uint16_t new_licensee;  /* 0x144-0x145 */
    uint8_t sgb_flag;       /* 0x146 */
    uint8_t cart_type;      /* 0x147: MBC type */
    uint8_t rom_size_code;  /* 0x148 */
    uint8_t ram_size_code;  /* 0x149 */
    uint8_t dest_code;      /* 0x14A */
    uint8_t old_licensee;   /* 0x14B */
    uint8_t version;        /* 0x14C */
    uint8_t header_checksum;/* 0x14D */
    uint16_t global_checksum; /* 0x14E-0x14F */
    uint32_t rom_size;      /* Calculated */
    uint32_t ram_size;      /* Calculated */
} GbCartHeader;

/* GBA cartridge header (at 0x000-0x0BF) */
typedef struct {
    uint32_t entry_point;   /* 0x000: ARM branch */
    uint8_t  logo[156];     /* 0x004-0x09F: Nintendo logo */
    char     title[13];     /* 0x0A0-0x0AB (null-terminated) */
    char     game_code[5];  /* 0x0AC-0x0AF */
    char     maker_code[3]; /* 0x0B0-0x0B1 */
    uint8_t  fixed_val;     /* 0x0B2: must be 0x96 */
    uint8_t  unit_code;     /* 0x0B3 */
    uint8_t  device_type;   /* 0x0B4 */
    uint8_t  version;       /* 0x0BC */
    uint8_t  complement;    /* 0x0BD */
    uint32_t rom_size;      /* Calculated from file size */
} GbaCartHeader;

/* GBA save type detection */
typedef enum {
    GBA_SAVE_NONE = 0,
    GBA_SAVE_SRAM,
    GBA_SAVE_FLASH_64K,
    GBA_SAVE_FLASH_128K,
    GBA_SAVE_EEPROM
} GbaSaveType;

/* Unified cartridge */
typedef struct {
    CartType type;
    uint8_t *rom;
    size_t rom_size;
    uint8_t *ram;       /* External RAM (SRAM/battery backed) */
    size_t ram_size;

    union {
        GbCartHeader gb;
        GbaCartHeader gba;
    } header;

    /* Save file path (for battery backed RAM) */
    char save_path[512];
    bool has_battery;
    bool has_rtc;
    GbaSaveType gba_save_type;
} Cartridge;

/* API */
int cartridge_load(Cartridge *cart, const uint8_t *data, size_t size);
int cartridge_load_file(Cartridge *cart, const char *path);
void cartridge_free(Cartridge *cart);

/* Save RAM */
int cartridge_load_save(Cartridge *cart, const char *path);
int cartridge_save_ram(const Cartridge *cart, const char *path);

/* Detect type from data */
CartType cartridge_detect_type(const uint8_t *data, size_t size);

/* Serialization */
size_t cartridge_serialize(const Cartridge *cart, uint8_t *buf, size_t buf_size);
size_t cartridge_deserialize(Cartridge *cart, const uint8_t *buf, size_t buf_size);

#endif /* GBPY_CARTRIDGE_H */
