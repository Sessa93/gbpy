/*
 * Game Boy / Game Boy Color Audio Processing Unit
 *
 * Reference: Game Boy CPU Manual v1.01 (resources/gba-manual.md)
 * Section 2.11: Sound
 *
 * Four sound channels:
 *   Channel 1: Square wave with sweep and envelope (NR10-NR14)
 *   Channel 2: Square wave with envelope (NR21-NR24)
 *   Channel 3: Programmable wave (NR30-NR34, wave RAM FF30-FF3F)
 *   Channel 4: Noise (NR41-NR44)
 *
 * Master: NR50 (volume/Vin), NR51 (panning), NR52 (on/off)
 * Sample rate target: 44100 Hz
 * CPU clock: 4194304 Hz -> downsample ratio ~ 95.1
 */

#ifndef GBPY_APU_H
#define GBPY_APU_H

#include "types.h"

/* Audio buffer size: ~1 frame worth at 44100Hz (~735 samples at 60fps) */
#define APU_BUFFER_SIZE     2048
#define APU_SAMPLE_RATE     44100

/* Duty cycle patterns for square wave channels (Section 2.11.1)
 * "A square channel's frequency timer period is set to (2048-frequency)*4."
 * Duty patterns: 12.5%, 25%, 50%, 75%
 */

/* Envelope direction */
#define ENV_DECREASE 0
#define ENV_INCREASE 1

/* Sweep */
typedef struct {
    uint8_t period;     /* Sweep time (NR10 bits 6-4) */
    uint8_t shift;      /* Number of shift (NR10 bits 2-0) */
    uint8_t direction;  /* 0=increase, 1=decrease (NR10 bit 3) */
    bool enabled;
    uint16_t shadow;    /* Shadow frequency */
    int timer;
    bool negate_used;
} Sweep;

/* Volume envelope */
typedef struct {
    uint8_t initial;    /* Starting volume */
    uint8_t direction;  /* 0=decrease, 1=increase */
    uint8_t period;     /* Envelope period */
    uint8_t volume;     /* Current volume */
    int timer;
} Envelope;

/* Length counter */
typedef struct {
    int counter;
    bool enabled;
} LengthCounter;

/* Channel 1: Square + Sweep (Section 2.11.1) */
typedef struct {
    bool enabled;
    bool dac_enabled;
    Sweep sweep;
    Envelope envelope;
    LengthCounter length;
    uint8_t duty;            /* Duty pattern (bits 7-6 of NR11) */
    uint16_t frequency;      /* 11-bit frequency */
    int timer;               /* Frequency timer */
    uint8_t duty_pos;        /* Position in duty cycle (0-7) */
    uint8_t output;          /* Current output sample */
} Channel1;

/* Channel 2: Square (Section 2.11.2) */
typedef struct {
    bool enabled;
    bool dac_enabled;
    Envelope envelope;
    LengthCounter length;
    uint8_t duty;
    uint16_t frequency;
    int timer;
    uint8_t duty_pos;
    uint8_t output;
} Channel2;

/* Channel 3: Wave (Section 2.11.3) */
typedef struct {
    bool enabled;
    bool dac_enabled;
    LengthCounter length;
    uint8_t volume_code;     /* Output level (NR32 bits 6-5) */
    uint16_t frequency;
    int timer;
    uint8_t pos;             /* Position in wave RAM (0-31) */
    uint8_t sample;          /* Current sample */
    uint8_t output;
    uint8_t wave_ram[16];    /* 32 4-bit samples in 16 bytes (FF30-FF3F) */
} Channel3;

/* Channel 4: Noise (Section 2.11.4) */
typedef struct {
    bool enabled;
    bool dac_enabled;
    Envelope envelope;
    LengthCounter length;
    uint8_t clock_shift;     /* NR43 bits 7-4 */
    bool width_mode;         /* NR43 bit 3: 0=15bit, 1=7bit */
    uint8_t divisor_code;    /* NR43 bits 2-0 */
    int timer;
    uint16_t lfsr;           /* Linear Feedback Shift Register */
    uint8_t output;
} Channel4;

typedef struct APU {
    Channel1 ch1;
    Channel2 ch2;
    Channel3 ch3;
    Channel4 ch4;

    /* Master control */
    bool power;              /* NR52 bit 7 */
    uint8_t left_volume;     /* NR50 bits 6-4 */
    uint8_t right_volume;    /* NR50 bits 2-0 */
    uint8_t panning;         /* NR51 */

    /* Frame sequencer (512 Hz, 8192 CPU cycles per step) */
    uint32_t frame_seq_counter;
    uint8_t frame_seq_step;  /* 0-7 */

    /* Sample generation */
    uint32_t sample_counter;
    uint32_t sample_period;  /* CPU cycles per sample */

    /* Output buffer */
    GbpyAudioSample buffer[APU_BUFFER_SIZE];
    uint32_t buffer_pos;
    bool buffer_full;

    /* I/O register cache */
    uint8_t *io;             /* Pointer to MMU's I/O */
} APU;

void apu_init(APU *apu);
void apu_reset(APU *apu);
void apu_step(APU *apu, uint32_t cycles);
uint8_t apu_read(APU *apu, uint16_t addr);
void apu_write(APU *apu, uint16_t addr, uint8_t val);

/* Get audio buffer. Returns number of samples available. */
uint32_t apu_get_samples(APU *apu, GbpyAudioSample *out, uint32_t max_samples);

size_t apu_serialize(const APU *apu, uint8_t *buf, size_t buf_size);
size_t apu_deserialize(APU *apu, const uint8_t *buf, size_t buf_size);

#endif /* GBPY_APU_H */
