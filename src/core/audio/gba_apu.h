/*
 * GBA APU Header
 *
 * The GBA audio system has:
 * - Legacy 4 GB channels (reused from DMG/CGB)
 * - 2 Direct Sound DMA channels (A and B) - 8-bit PCM at timer rate
 *
 * Sound output: stereo, mixed to 16-bit samples
 */

#ifndef GBPY_GBA_APU_H
#define GBPY_GBA_APU_H

#include "../types.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* Forward declarations */
typedef struct GbaMemory GbaMemory;

/* Audio buffer */
#define GBA_AUDIO_SAMPLE_RATE  32768
#define GBA_AUDIO_BUFFER_SIZE  4096

/* SOUNDCNT_H bits */
#define SOUNDCNT_H_DMG_VOL(v)    ((v) & 3)      /* 0=25%, 1=50%, 2=100%, 3=prohibited */
#define SOUNDCNT_H_DMA_A_VOL(v)  (((v) >> 2) & 1) /* 0=50%, 1=100% */
#define SOUNDCNT_H_DMA_B_VOL(v)  (((v) >> 3) & 1)
#define SOUNDCNT_H_DMA_A_RIGHT(v)(((v) >> 8) & 1)
#define SOUNDCNT_H_DMA_A_LEFT(v) (((v) >> 9) & 1)
#define SOUNDCNT_H_DMA_A_TIMER(v)(((v) >> 10) & 1) /* 0=Timer0, 1=Timer1 */
#define SOUNDCNT_H_DMA_A_RESET(v)(((v) >> 11) & 1)
#define SOUNDCNT_H_DMA_B_RIGHT(v)(((v) >> 12) & 1)
#define SOUNDCNT_H_DMA_B_LEFT(v) (((v) >> 13) & 1)
#define SOUNDCNT_H_DMA_B_TIMER(v)(((v) >> 14) & 1) /* 0=Timer0, 1=Timer1 */
#define SOUNDCNT_H_DMA_B_RESET(v)(((v) >> 15) & 1)

/* FIFO */
#define GBA_FIFO_SIZE 32

typedef struct {
    int8_t buffer[GBA_FIFO_SIZE];
    uint8_t read_pos;
    uint8_t write_pos;
    uint8_t count;
    int8_t current_sample; /* Last output sample */
} GbaFifo;

/* Legacy channel state (simplified GB channels in GBA context) */
typedef struct {
    /* Channel 1: Square + sweep */
    uint16_t ch1_sweep_pace;
    uint16_t ch1_sweep_dir;
    uint16_t ch1_sweep_step;
    uint16_t ch1_duty;
    uint16_t ch1_length;
    uint16_t ch1_env_vol;
    uint16_t ch1_env_dir;
    uint16_t ch1_env_pace;
    uint16_t ch1_freq;
    bool     ch1_length_en;
    bool     ch1_enabled;
    uint8_t  ch1_volume;
    uint16_t ch1_timer;
    uint8_t  ch1_duty_pos;
    uint16_t ch1_shadow_freq;
    uint16_t ch1_sweep_timer;
    uint8_t  ch1_env_timer;
    uint8_t  ch1_length_counter;

    /* Channel 2: Square */
    uint16_t ch2_duty;
    uint16_t ch2_length;
    uint16_t ch2_env_vol;
    uint16_t ch2_env_dir;
    uint16_t ch2_env_pace;
    uint16_t ch2_freq;
    bool     ch2_length_en;
    bool     ch2_enabled;
    uint8_t  ch2_volume;
    uint16_t ch2_timer;
    uint8_t  ch2_duty_pos;
    uint8_t  ch2_env_timer;
    uint8_t  ch2_length_counter;

    /* Channel 3: Wave */
    bool     ch3_dac_en;
    uint16_t ch3_length;
    uint8_t  ch3_volume_code; /* 0=0%, 1=100%, 2=50%, 3=25% */
    uint16_t ch3_freq;
    bool     ch3_length_en;
    bool     ch3_enabled;
    uint16_t ch3_timer;
    uint8_t  ch3_sample_pos;
    uint16_t ch3_length_counter;
    uint8_t  wave_ram[2][16]; /* Two banks */
    uint8_t  ch3_bank;       /* Current bank (GBA has 2 banks) */
    bool     ch3_dimension;  /* 0=one bank 32 samples, 1=two banks 64 samples */

    /* Channel 4: Noise */
    uint16_t ch4_length;
    uint16_t ch4_env_vol;
    uint16_t ch4_env_dir;
    uint16_t ch4_env_pace;
    uint16_t ch4_clock_shift;
    uint16_t ch4_width_mode;
    uint16_t ch4_div_ratio;
    bool     ch4_length_en;
    bool     ch4_enabled;
    uint8_t  ch4_volume;
    uint32_t ch4_timer;
    uint16_t ch4_lfsr;
    uint8_t  ch4_env_timer;
    uint8_t  ch4_length_counter;

    /* Frame sequencer */
    uint32_t frame_seq_timer;
    uint8_t  frame_seq_step;

    /* Master */
    bool     master_enable;
} GbaLegacyAPU;

typedef struct GbaAPU {
    /* DMA channels */
    GbaFifo fifo_a;
    GbaFifo fifo_b;

    /* Legacy channels */
    GbaLegacyAPU legacy;

    /* Output buffer */
    int16_t sample_buffer[GBA_AUDIO_BUFFER_SIZE * 2]; /* Interleaved L/R */
    uint32_t sample_count;
    bool buffer_full;

    /* Sample accumulator */
    uint32_t sample_clock;   /* Cycles until next sample output */
    uint32_t cycles_per_sample; /* CPU cycles per audio sample */

    /* Memory pointer */
    GbaMemory *mem;
} GbaAPU;

/* API */
void gba_apu_init(GbaAPU *apu);
void gba_apu_reset(GbaAPU *apu);
void gba_apu_step(GbaAPU *apu, uint32_t cycles);

/* Called when timer overflows (for DMA sound) */
void gba_apu_timer_overflow(GbaAPU *apu, int timer_id);

/* FIFO write (from DMA or CPU) */
void gba_fifo_write(GbaFifo *fifo, int8_t sample);
void gba_fifo_write32(GbaFifo *fifo, uint32_t data);
void gba_fifo_reset(GbaFifo *fifo);

/* Legacy channel triggers */
void gba_apu_write_reg(GbaAPU *apu, uint32_t addr, uint8_t val);

/* Serialization */
size_t gba_apu_serialize(const GbaAPU *apu, uint8_t *buf, size_t buf_size);
size_t gba_apu_deserialize(GbaAPU *apu, const uint8_t *buf, size_t buf_size);

#endif /* GBPY_GBA_APU_H */
