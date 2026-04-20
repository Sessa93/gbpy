/*
 * Save State Management
 *
 * Aggregates serialization from all subsystems into a single save state.
 * Format: Magic + Version + [subsystem chunks with size headers]
 */

#ifndef GBPY_STATE_H
#define GBPY_STATE_H

#include "types.h"
#include <stdint.h>
#include <stddef.h>

/* Forward declarations */
typedef struct Emulator Emulator;

#define STATE_MAGIC  0x47425059  /* "GBPY" */
#define STATE_VERSION 1

/* Chunk IDs */
#define CHUNK_SM83    0x01
#define CHUNK_MMU     0x02
#define CHUNK_MBC     0x03
#define CHUNK_GB_PPU  0x04
#define CHUNK_APU     0x05
#define CHUNK_TIMER   0x06
#define CHUNK_INPUT   0x07
#define CHUNK_ARM7    0x08
#define CHUNK_GBA_MEM 0x09
#define CHUNK_GBA_PPU 0x0A
#define CHUNK_GBA_APU 0x0B
#define CHUNK_CART    0x0C
#define CHUNK_END     0xFF

/* API */
int state_save(const Emulator *emu, const char *path);
int state_load(Emulator *emu, const char *path);

/* In-memory save/load */
size_t state_save_to_buffer(const Emulator *emu, uint8_t *buf, size_t buf_size);
int state_load_from_buffer(Emulator *emu, const uint8_t *buf, size_t size);

#endif /* GBPY_STATE_H */
