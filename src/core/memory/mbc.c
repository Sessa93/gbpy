/*
 * Memory Bank Controller Implementation
 *
 * Reference: Game Boy CPU Manual v1.01 (resources/gba-manual.md)
 * Section 2.6: Cartridge Types
 *
 * Section 2.5.4 (address 0x0147): Cartridge type byte
 * Section 2.5.4 (address 0x0148): ROM size
 * Section 2.5.4 (address 0x0149): RAM size
 */

#include "mbc.h"

/* ROM size lookup table (Section 2.5.4, address 0x0148) */
static size_t rom_size_from_code(uint8_t code) {
    switch (code) {
        case 0: return 32 * 1024;     /* 32KB = 2 banks */
        case 1: return 64 * 1024;     /* 64KB = 4 banks */
        case 2: return 128 * 1024;    /* 128KB = 8 banks */
        case 3: return 256 * 1024;    /* 256KB = 16 banks */
        case 4: return 512 * 1024;    /* 512KB = 32 banks */
        case 5: return 1024 * 1024;   /* 1MB = 64 banks */
        case 6: return 2048 * 1024;   /* 2MB = 128 banks */
        case 0x52: return 1152 * 1024; /* 1.1MB = 72 banks */
        case 0x53: return 1280 * 1024; /* 1.2MB = 80 banks */
        case 0x54: return 1536 * 1024; /* 1.5MB = 96 banks */
        default: return 32 * 1024;
    }
}

/* RAM size lookup table (Section 2.5.4, address 0x0149) */
static size_t ram_size_from_code(uint8_t code) {
    switch (code) {
        case 0: return 0;
        case 1: return 2 * 1024;     /* 2KB */
        case 2: return 8 * 1024;     /* 8KB */
        case 3: return 32 * 1024;    /* 32KB = 4 banks */
        case 4: return 128 * 1024;   /* 128KB = 16 banks */
        default: return 0;
    }
}

static MBCType detect_mbc_type(uint8_t cart_type) {
    switch (cart_type) {
        case CART_ROM_ONLY:
        case CART_ROM_RAM:
        case CART_ROM_RAM_BATT:
            return MBC_TYPE_NONE;
        case CART_MBC1:
        case CART_MBC1_RAM:
        case CART_MBC1_RAM_BATT:
            return MBC_TYPE_MBC1;
        case CART_MBC2:
        case CART_MBC2_BATT:
            return MBC_TYPE_MBC2;
        case CART_MBC3_TIMER_BATT:
        case CART_MBC3_TIMER_RAM_BATT:
        case CART_MBC3:
        case CART_MBC3_RAM:
        case CART_MBC3_RAM_BATT:
            return MBC_TYPE_MBC3;
        case CART_MBC5:
        case CART_MBC5_RAM:
        case CART_MBC5_RAM_BATT:
        case CART_MBC5_RUMBLE:
        case CART_MBC5_RUMBLE_RAM:
        case CART_MBC5_RUMBLE_RAM_BATT:
            return MBC_TYPE_MBC5;
        default:
            return MBC_TYPE_NONE;
    }
}

static bool has_battery(uint8_t cart_type) {
    switch (cart_type) {
        case CART_MBC1_RAM_BATT:
        case CART_MBC2_BATT:
        case CART_ROM_RAM_BATT:
        case CART_MBC3_TIMER_BATT:
        case CART_MBC3_TIMER_RAM_BATT:
        case CART_MBC3_RAM_BATT:
        case CART_MBC5_RAM_BATT:
        case CART_MBC5_RUMBLE_RAM_BATT:
            return true;
        default:
            return false;
    }
}

static bool has_rtc_feature(uint8_t cart_type) {
    return cart_type == CART_MBC3_TIMER_BATT ||
           cart_type == CART_MBC3_TIMER_RAM_BATT;
}

GbpyResult mbc_init(MBC *mbc, const uint8_t *rom, size_t rom_size_bytes) {
    memset(mbc, 0, sizeof(MBC));

    if (!rom || rom_size_bytes < 0x150) {
        return GBPY_ERR_INVALID_ROM;
    }

    mbc->rom = rom;
    mbc->rom_size = rom_size_bytes;
    mbc->cart_type = (CartridgeType)rom[0x0147];
    mbc->type = detect_mbc_type(rom[0x0147]);
    mbc->has_battery = has_battery(rom[0x0147]);
    mbc->has_rtc = has_rtc_feature(rom[0x0147]);

    /* Calculate number of ROM banks */
    size_t expected_rom = rom_size_from_code(rom[0x0148]);
    (void)expected_rom;
    mbc->num_rom_banks = (uint16_t)(rom_size_bytes / 0x4000);
    if (mbc->num_rom_banks == 0) mbc->num_rom_banks = 2;

    /* Allocate external RAM */
    if (mbc->type == MBC_TYPE_MBC2) {
        /* MBC2 has 512x4 bits of internal RAM (Section 2.6) */
        mbc->ram_size = 512;
        mbc->ram = calloc(1, mbc->ram_size);
    } else {
        mbc->ram_size = ram_size_from_code(rom[0x0149]);
        if (mbc->ram_size > 0) {
            mbc->ram = calloc(1, mbc->ram_size);
        }
    }

    /* Default state */
    mbc->rom_bank = 1;
    mbc->ram_bank = 0;
    mbc->ram_enabled = false;
    mbc->mbc1_mode = false;
    mbc->mbc1_bank_lo = 1;
    mbc->mbc1_bank_hi = 0;
    mbc->mbc5_rom_bank = 1;

    return GBPY_OK;
}

void mbc_destroy(MBC *mbc) {
    if (mbc->ram) {
        free(mbc->ram);
        mbc->ram = NULL;
    }
}

/* ---------- ROM read ---------- */

uint8_t mbc_read_rom(MBC *mbc, uint16_t addr) {
    if (addr < 0x4000) {
        /* Bank 0 (always mapped) */
        if (mbc->type == MBC_TYPE_MBC1 && mbc->mbc1_mode) {
            /* In 4/32 mode, bank 0 area uses upper bits (Section 2.6) */
            uint32_t bank0 = (uint32_t)(mbc->mbc1_bank_hi << 5);
            uint32_t offset = (bank0 * 0x4000) + addr;
            return mbc->rom[offset % mbc->rom_size];
        }
        if (addr < mbc->rom_size) {
            return mbc->rom[addr];
        }
        return 0xFF;
    }

    /* Switchable bank (4000-7FFF) */
    uint32_t bank = mbc->rom_bank;
    uint32_t offset = (bank * 0x4000) + (addr - 0x4000);
    if (offset < mbc->rom_size) {
        return mbc->rom[offset];
    }
    return 0xFF;
}

/* ---------- External RAM access ---------- */

uint8_t mbc_read_ram(MBC *mbc, uint16_t addr) {
    if (!mbc->ram_enabled || !mbc->ram) return 0xFF;

    if (mbc->type == MBC_TYPE_MBC3 && mbc->has_rtc && mbc->ram_bank >= 0x08) {
        /* RTC register read */
        uint8_t reg = mbc->ram_bank - 0x08;
        if (reg < 5) return mbc->rtc_latched[reg];
        return 0xFF;
    }

    if (mbc->type == MBC_TYPE_MBC2) {
        /* MBC2: only lower 4 bits, 512 bytes */
        uint16_t ram_addr = (addr - 0xA000) & 0x1FF;
        return mbc->ram[ram_addr] | 0xF0;
    }

    uint32_t ram_addr = ((uint32_t)mbc->ram_bank * 0x2000) + (addr - 0xA000);
    if (ram_addr < mbc->ram_size) {
        return mbc->ram[ram_addr];
    }
    return 0xFF;
}

void mbc_write_ram(MBC *mbc, uint16_t addr, uint8_t val) {
    if (!mbc->ram_enabled) return;

    if (mbc->type == MBC_TYPE_MBC3 && mbc->has_rtc && mbc->ram_bank >= 0x08) {
        /* RTC register write */
        uint8_t reg = mbc->ram_bank - 0x08;
        if (reg < 5) mbc->rtc_regs[reg] = val;
        return;
    }

    if (!mbc->ram) return;

    if (mbc->type == MBC_TYPE_MBC2) {
        uint16_t ram_addr = (addr - 0xA000) & 0x1FF;
        mbc->ram[ram_addr] = val & 0x0F;
        return;
    }

    uint32_t ram_addr = ((uint32_t)mbc->ram_bank * 0x2000) + (addr - 0xA000);
    if (ram_addr < mbc->ram_size) {
        mbc->ram[ram_addr] = val;
    }
}

/* ---------- MBC register writes ---------- */

/*
 * MBC1 (Section 2.6):
 * 0000-1FFF: RAM Enable (write XXXX1010 to enable)
 * 2000-3FFF: ROM Bank Number (lower 5 bits)
 * 4000-5FFF: RAM Bank / Upper ROM bits
 * 6000-7FFF: Mode Select (0=16/8, 1=4/32)
 */
static void mbc1_write(MBC *mbc, uint16_t addr, uint8_t val) {
    if (addr < 0x2000) {
        /* RAM Enable: "writing a XXXX1010 into 0000-1FFF" */
        mbc->ram_enabled = ((val & 0x0F) == 0x0A);
    } else if (addr < 0x4000) {
        /* ROM Bank Number (lower 5 bits) */
        /* "Values of 0 and 1 do the same thing and point to ROM bank 1" */
        mbc->mbc1_bank_lo = val & 0x1F;
        if (mbc->mbc1_bank_lo == 0) mbc->mbc1_bank_lo = 1;
        mbc->rom_bank = (mbc->mbc1_bank_hi << 5) | mbc->mbc1_bank_lo;
        mbc->rom_bank %= mbc->num_rom_banks;
    } else if (addr < 0x6000) {
        /* RAM Bank or upper ROM bits */
        mbc->mbc1_bank_hi = val & 0x03;
        if (mbc->mbc1_mode) {
            mbc->ram_bank = mbc->mbc1_bank_hi;
        }
        mbc->rom_bank = (mbc->mbc1_bank_hi << 5) | mbc->mbc1_bank_lo;
        mbc->rom_bank %= mbc->num_rom_banks;
    } else {
        /* Mode Select */
        mbc->mbc1_mode = (val & 0x01) != 0;
        if (!mbc->mbc1_mode) {
            mbc->ram_bank = 0;
        }
    }
}

/*
 * MBC2 (Section 2.6):
 * Bit 0 of upper address byte selects RAM enable vs ROM bank
 */
static void mbc2_write(MBC *mbc, uint16_t addr, uint8_t val) {
    if (addr < 0x4000) {
        if (addr & 0x0100) {
            /* ROM bank select */
            mbc->rom_bank = val & 0x0F;
            if (mbc->rom_bank == 0) mbc->rom_bank = 1;
            mbc->rom_bank %= mbc->num_rom_banks;
        } else {
            /* RAM enable */
            mbc->ram_enabled = ((val & 0x0F) == 0x0A);
        }
    }
}

/*
 * MBC3 (Section 2.6):
 * 0000-1FFF: RAM/RTC Enable
 * 2000-3FFF: ROM Bank (7 bits, accesses all 16Mbit)
 * 4000-5FFF: RAM Bank / RTC Register Select
 * 6000-7FFF: Latch Clock Data
 */
static void mbc3_write(MBC *mbc, uint16_t addr, uint8_t val) {
    if (addr < 0x2000) {
        mbc->ram_enabled = ((val & 0x0F) == 0x0A);
    } else if (addr < 0x4000) {
        mbc->rom_bank = val & 0x7F;
        if (mbc->rom_bank == 0) mbc->rom_bank = 1;
        mbc->rom_bank %= mbc->num_rom_banks;
    } else if (addr < 0x6000) {
        if (val <= 0x03) {
            mbc->ram_bank = val;
        } else if (val >= 0x08 && val <= 0x0C) {
            mbc->ram_bank = val; /* RTC register select */
        }
    } else {
        /* Latch Clock Data */
        if (val == 0x01 && mbc->rtc_latching) {
            memcpy(mbc->rtc_latched, mbc->rtc_regs, 5);
        }
        mbc->rtc_latching = (val == 0x00);
    }
}

/*
 * MBC5 (Section 2.6):
 * 0000-1FFF: RAM Enable
 * 2000-2FFF: Low 8 bits of ROM Bank
 * 3000-3FFF: High bit of ROM Bank (bit 8)
 * 4000-5FFF: RAM Bank (0-15)
 *
 * "This is the first MBC that allows rom bank 0 to appear in 4000-7FFF"
 */
static void mbc5_write(MBC *mbc, uint16_t addr, uint8_t val) {
    if (addr < 0x2000) {
        mbc->ram_enabled = ((val & 0x0F) == 0x0A);
    } else if (addr < 0x3000) {
        mbc->mbc5_rom_bank = (mbc->mbc5_rom_bank & 0x100) | val;
        mbc->rom_bank = mbc->mbc5_rom_bank % mbc->num_rom_banks;
    } else if (addr < 0x4000) {
        mbc->mbc5_rom_bank = (mbc->mbc5_rom_bank & 0xFF) | ((val & 0x01) << 8);
        mbc->rom_bank = mbc->mbc5_rom_bank % mbc->num_rom_banks;
    } else if (addr < 0x6000) {
        mbc->ram_bank = val & 0x0F;
    }
}

void mbc_write(MBC *mbc, uint16_t addr, uint8_t val) {
    switch (mbc->type) {
        case MBC_TYPE_NONE: break;
        case MBC_TYPE_MBC1: mbc1_write(mbc, addr, val); break;
        case MBC_TYPE_MBC2: mbc2_write(mbc, addr, val); break;
        case MBC_TYPE_MBC3: mbc3_write(mbc, addr, val); break;
        case MBC_TYPE_MBC5: mbc5_write(mbc, addr, val); break;
    }
}

/* ---------- Serialization ---------- */

size_t mbc_serialize(const MBC *mbc, uint8_t *buf, size_t buf_size) {
    size_t needed = 32 + mbc->ram_size;
    if (!buf || buf_size < needed) return needed;

    size_t pos = 0;
    memcpy(buf + pos, &mbc->type, sizeof(mbc->type)); pos += sizeof(mbc->type);
    memcpy(buf + pos, &mbc->rom_bank, sizeof(mbc->rom_bank)); pos += sizeof(mbc->rom_bank);
    memcpy(buf + pos, &mbc->ram_bank, sizeof(mbc->ram_bank)); pos += sizeof(mbc->ram_bank);
    memcpy(buf + pos, &mbc->ram_enabled, sizeof(mbc->ram_enabled)); pos += sizeof(mbc->ram_enabled);
    memcpy(buf + pos, &mbc->mbc1_mode, sizeof(mbc->mbc1_mode)); pos += sizeof(mbc->mbc1_mode);
    memcpy(buf + pos, &mbc->mbc1_bank_lo, sizeof(mbc->mbc1_bank_lo)); pos += sizeof(mbc->mbc1_bank_lo);
    memcpy(buf + pos, &mbc->mbc1_bank_hi, sizeof(mbc->mbc1_bank_hi)); pos += sizeof(mbc->mbc1_bank_hi);
    memcpy(buf + pos, &mbc->mbc5_rom_bank, sizeof(mbc->mbc5_rom_bank)); pos += sizeof(mbc->mbc5_rom_bank);
    memcpy(buf + pos, mbc->rtc_regs, 5); pos += 5;
    memcpy(buf + pos, mbc->rtc_latched, 5); pos += 5;

    /* Save external RAM */
    if (mbc->ram && mbc->ram_size > 0) {
        memcpy(buf + pos, mbc->ram, mbc->ram_size);
        pos += mbc->ram_size;
    }

    return pos;
}

size_t mbc_deserialize(MBC *mbc, const uint8_t *buf, size_t buf_size) {
    size_t pos = 0;
    if (buf_size < 32) return 0;

    memcpy(&mbc->type, buf + pos, sizeof(mbc->type)); pos += sizeof(mbc->type);
    memcpy(&mbc->rom_bank, buf + pos, sizeof(mbc->rom_bank)); pos += sizeof(mbc->rom_bank);
    memcpy(&mbc->ram_bank, buf + pos, sizeof(mbc->ram_bank)); pos += sizeof(mbc->ram_bank);
    memcpy(&mbc->ram_enabled, buf + pos, sizeof(mbc->ram_enabled)); pos += sizeof(mbc->ram_enabled);
    memcpy(&mbc->mbc1_mode, buf + pos, sizeof(mbc->mbc1_mode)); pos += sizeof(mbc->mbc1_mode);
    memcpy(&mbc->mbc1_bank_lo, buf + pos, sizeof(mbc->mbc1_bank_lo)); pos += sizeof(mbc->mbc1_bank_lo);
    memcpy(&mbc->mbc1_bank_hi, buf + pos, sizeof(mbc->mbc1_bank_hi)); pos += sizeof(mbc->mbc1_bank_hi);
    memcpy(&mbc->mbc5_rom_bank, buf + pos, sizeof(mbc->mbc5_rom_bank)); pos += sizeof(mbc->mbc5_rom_bank);
    memcpy(mbc->rtc_regs, buf + pos, 5); pos += 5;
    memcpy(mbc->rtc_latched, buf + pos, 5); pos += 5;

    if (mbc->ram && mbc->ram_size > 0 && pos + mbc->ram_size <= buf_size) {
        memcpy(mbc->ram, buf + pos, mbc->ram_size);
        pos += mbc->ram_size;
    }

    return pos;
}
