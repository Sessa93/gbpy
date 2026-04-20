/*
 * gbpy - GBA/GBC Emulator Core
 * Common types and definitions shared across all subsystems.
 *
 * Reference: Game Boy CPU Manual v1.01 (resources/gba-manual.md)
 */

#ifndef GBPY_TYPES_H
#define GBPY_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

/* ---------- Platform detection ---------- */
#ifdef _MSC_VER
  #define GBPY_INLINE __forceinline
#else
  #define GBPY_INLINE static inline __attribute__((always_inline))
#endif

/* ---------- Emulator mode ---------- */
typedef enum {
    GBPY_MODE_GB  = 0,   /* Original Game Boy */
    GBPY_MODE_GBC = 1,   /* Game Boy Color */
    GBPY_MODE_GBA = 2,   /* Game Boy Advance */
} GbpyMode;

/* ---------- Screen dimensions ---------- */
/* GB/GBC: 160x144, GBA: 240x160 */
#define GB_SCREEN_WIDTH   160
#define GB_SCREEN_HEIGHT  144
#define GBA_SCREEN_WIDTH  240
#define GBA_SCREEN_HEIGHT 160

/* ---------- Clock speeds (Hz) ---------- */
/* Manual section 2.3: Clock Speed: 4.194304 MHz */
#define GB_CLOCK_SPEED    4194304
#define GBC_CLOCK_SPEED   8388608   /* Double speed mode */
#define GBA_CLOCK_SPEED   16777216  /* 16.78 MHz ARM7TDMI */

/* ---------- Memory sizes ---------- */
/* Manual section 2.3: Main RAM: 8K Byte, Video RAM: 8K Byte */
#define GB_RAM_SIZE       0x2000    /* 8KB internal RAM */
#define GB_VRAM_SIZE      0x2000    /* 8KB video RAM */
#define GBC_RAM_SIZE      0x8000    /* 32KB (8 banks of 4KB) */
#define GBC_VRAM_SIZE     0x4000    /* 16KB (2 banks of 8KB) */
#define GB_OAM_SIZE       0xA0      /* 160 bytes sprite attribute memory */
#define GB_HRAM_SIZE      0x7F      /* High RAM FF80-FFFE */
#define GB_IO_SIZE        0x80      /* I/O registers FF00-FF7F */

/* GBA memory sizes */
#define GBA_BIOS_SIZE     0x4000    /* 16KB */
#define GBA_EWRAM_SIZE    0x40000   /* 256KB external work RAM */
#define GBA_IWRAM_SIZE    0x8000    /* 32KB internal work RAM */
#define GBA_PALETTE_SIZE  0x400     /* 1KB */
#define GBA_VRAM_SIZE     0x18000   /* 96KB */
#define GBA_OAM_SIZE      0x400     /* 1KB */
#define GBA_IO_SIZE       0x400     /* 1KB I/O registers */

/* ---------- Common result codes ---------- */
typedef enum {
    GBPY_OK = 0,
    GBPY_ERR_INVALID_ROM,
    GBPY_ERR_UNSUPPORTED_MBC,
    GBPY_ERR_OUT_OF_MEMORY,
    GBPY_ERR_INVALID_STATE,
    GBPY_ERR_IO,
} GbpyResult;

/* ---------- Color type ---------- */
typedef struct {
    uint8_t r, g, b, a;
} GbpyColor;

/* ---------- Audio sample ---------- */
typedef struct {
    int16_t left;
    int16_t right;
} GbpyAudioSample;

#define GBPY_AUDIO_SAMPLE_RATE 44100
#define GBPY_AUDIO_BUFFER_SIZE 2048

#endif /* GBPY_TYPES_H */
