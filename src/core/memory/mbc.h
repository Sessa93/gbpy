/*
 * Memory Bank Controllers (MBC) - Game Boy Cartridge Types
 *
 * Reference: Game Boy CPU Manual v1.01 (resources/gba-manual.md)
 * Section 2.6: Cartridge Types
 *
 * Supported controllers:
 *   - ROM Only (no banking)
 *   - MBC1: 16Mbit ROM/8KB RAM or 4Mbit ROM/32KB RAM (Section 2.6)
 *   - MBC2: Up to 2Mbit ROM, 512x4 bits internal RAM (Section 2.6)
 *   - MBC3: Up to 16Mbit ROM, optional RTC (Section 2.6)
 *   - MBC5: Up to 64Mbit ROM, 1Mbit RAM (Section 2.6)
 */

#ifndef GBPY_MBC_H
#define GBPY_MBC_H

#include "types.h"

/* Cartridge type codes (Section 2.5.4, address 0x0147) */
typedef enum {
    CART_ROM_ONLY        = 0x00,
    CART_MBC1            = 0x01,
    CART_MBC1_RAM        = 0x02,
    CART_MBC1_RAM_BATT   = 0x03,
    CART_MBC2            = 0x05,
    CART_MBC2_BATT       = 0x06,
    CART_ROM_RAM         = 0x08,
    CART_ROM_RAM_BATT    = 0x09,
    CART_MBC3_TIMER_BATT = 0x0F,
    CART_MBC3_TIMER_RAM_BATT = 0x10,
    CART_MBC3            = 0x11,
    CART_MBC3_RAM        = 0x12,
    CART_MBC3_RAM_BATT   = 0x13,
    CART_MBC5            = 0x19,
    CART_MBC5_RAM        = 0x1A,
    CART_MBC5_RAM_BATT   = 0x1B,
    CART_MBC5_RUMBLE     = 0x1C,
    CART_MBC5_RUMBLE_RAM = 0x1D,
    CART_MBC5_RUMBLE_RAM_BATT = 0x1E,
} CartridgeType;

typedef enum {
    MBC_TYPE_NONE,
    MBC_TYPE_MBC1,
    MBC_TYPE_MBC2,
    MBC_TYPE_MBC3,
    MBC_TYPE_MBC5,
} MBCType;

/* MBC state */
typedef struct MBC {
    MBCType type;
    CartridgeType cart_type;

    /* ROM banking */
    const uint8_t *rom;
    size_t rom_size;
    uint16_t rom_bank;       /* Current ROM bank (mapped to 4000-7FFF) */
    uint16_t num_rom_banks;

    /* RAM banking */
    uint8_t *ram;
    size_t ram_size;
    uint8_t ram_bank;
    bool ram_enabled;

    /* MBC1-specific (Section 2.6): memory model select */
    bool mbc1_mode;   /* 0 = 16Mbit ROM/8KB RAM, 1 = 4Mbit ROM/32KB RAM */
    uint8_t mbc1_bank_lo;  /* Lower 5 bits of ROM bank */
    uint8_t mbc1_bank_hi;  /* Upper 2 bits */

    /* MBC3-specific: RTC registers */
    bool has_rtc;
    uint8_t rtc_regs[5];    /* S, M, H, DL, DH */
    uint8_t rtc_latched[5];
    bool rtc_latching;
    uint64_t rtc_timestamp;

    /* MBC5-specific */
    uint16_t mbc5_rom_bank; /* 9-bit ROM bank number */

    /* Battery-backed flag */
    bool has_battery;
} MBC;

/* Initialize MBC based on cartridge header */
GbpyResult mbc_init(MBC *mbc, const uint8_t *rom, size_t rom_size);
void mbc_destroy(MBC *mbc);

/* ROM read (0000-7FFF) */
uint8_t mbc_read_rom(MBC *mbc, uint16_t addr);

/* External RAM access (A000-BFFF) */
uint8_t mbc_read_ram(MBC *mbc, uint16_t addr);
void mbc_write_ram(MBC *mbc, uint16_t addr, uint8_t val);

/* ROM area writes (register control) */
void mbc_write(MBC *mbc, uint16_t addr, uint8_t val);

/* State serialization */
size_t mbc_serialize(const MBC *mbc, uint8_t *buf, size_t buf_size);
size_t mbc_deserialize(MBC *mbc, const uint8_t *buf, size_t buf_size);

#endif /* GBPY_MBC_H */
