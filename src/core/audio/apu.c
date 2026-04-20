/*
 * Game Boy / Game Boy Color APU Implementation
 *
 * Reference: Game Boy CPU Manual v1.01 (resources/gba-manual.md)
 * Section 2.11: Sound Controller
 *
 * Channel timing:
 *   Frame sequencer: 512 Hz (8192 T-cycles)
 *     Step 0: Length
 *     Step 1: -
 *     Step 2: Length, Sweep
 *     Step 3: -
 *     Step 4: Length
 *     Step 5: -
 *     Step 6: Length, Sweep
 *     Step 7: Volume Envelope
 *
 * Frequency formula (Section 2.11.1):
 *   Square: freq_timer = (2048 - frequency) * 4
 *   Wave:   freq_timer = (2048 - frequency) * 2
 *   Noise:  freq_timer = divisor[code] << shift
 */

#include "apu.h"

/* I/O register offsets */
#define IO_NR10  0x10
#define IO_NR11  0x11
#define IO_NR12  0x12
#define IO_NR13  0x13
#define IO_NR14  0x14
#define IO_NR21  0x16
#define IO_NR22  0x17
#define IO_NR23  0x18
#define IO_NR24  0x19
#define IO_NR30  0x1A
#define IO_NR31  0x1B
#define IO_NR32  0x1C
#define IO_NR33  0x1D
#define IO_NR34  0x1E
#define IO_NR41  0x20
#define IO_NR42  0x21
#define IO_NR43  0x22
#define IO_NR44  0x23
#define IO_NR50  0x24
#define IO_NR51  0x25
#define IO_NR52  0x26

/* Duty cycle waveforms (Section 2.11.1)
 * 12.5%: 00000001
 * 25%:   10000001
 * 50%:   10000111
 * 75%:   01111110
 */
static const uint8_t DUTY_TABLE[4][8] = {
    {0, 0, 0, 0, 0, 0, 0, 1}, /* 12.5% */
    {1, 0, 0, 0, 0, 0, 0, 1}, /* 25%   */
    {1, 0, 0, 0, 0, 1, 1, 1}, /* 50%   */
    {0, 1, 1, 1, 1, 1, 1, 0}, /* 75%   */
};

/* Noise divisor table (Section 2.11.4) */
static const int NOISE_DIVISOR[8] = {8, 16, 32, 48, 64, 80, 96, 112};

#define FRAME_SEQ_PERIOD 8192

void apu_init(APU *apu) {
    memset(apu, 0, sizeof(APU));
    apu->sample_period = GB_CLOCK_SPEED / APU_SAMPLE_RATE;
    apu->ch4.lfsr = 0x7FFF;
}

void apu_reset(APU *apu) {
    uint8_t *io = apu->io;
    memset(apu, 0, sizeof(APU));
    apu->io = io;
    apu->sample_period = GB_CLOCK_SPEED / APU_SAMPLE_RATE;
    apu->ch4.lfsr = 0x7FFF;
}

/* ---------- Sweep (Channel 1, Section 2.11.1) ---------- */

static uint16_t sweep_calculate(Channel1 *ch) {
    uint16_t new_freq = ch->sweep.shadow >> ch->sweep.shift;
    if (ch->sweep.direction) {
        new_freq = ch->sweep.shadow - new_freq;
        ch->sweep.negate_used = true;
    } else {
        new_freq = ch->sweep.shadow + new_freq;
    }
    return new_freq;
}

static void sweep_clock(Channel1 *ch) {
    if (!ch->sweep.enabled || ch->sweep.period == 0) return;

    ch->sweep.timer--;
    if (ch->sweep.timer <= 0) {
        ch->sweep.timer = ch->sweep.period ? ch->sweep.period : 8;

        uint16_t new_freq = sweep_calculate(ch);
        if (new_freq > 2047) {
            ch->enabled = false;
        } else if (ch->sweep.shift > 0) {
            ch->sweep.shadow = new_freq;
            ch->frequency = new_freq;
            /* Overflow check again */
            if (sweep_calculate(ch) > 2047) {
                ch->enabled = false;
            }
        }
    }
}

/* ---------- Envelope ---------- */

static void envelope_clock(Envelope *env) {
    if (env->period == 0) return;

    env->timer--;
    if (env->timer <= 0) {
        env->timer = env->period ? env->period : 8;

        if (env->direction == ENV_INCREASE && env->volume < 15) {
            env->volume++;
        } else if (env->direction == ENV_DECREASE && env->volume > 0) {
            env->volume--;
        }
    }
}

/* ---------- Length counter ---------- */

static void length_clock(LengthCounter *len, bool *enabled) {
    if (!len->enabled) return;
    if (len->counter > 0) {
        len->counter--;
        if (len->counter == 0) {
            *enabled = false;
        }
    }
}

/* ---------- Channel step functions ---------- */

static void ch1_step(Channel1 *ch, uint32_t cycles) {
    ch->timer -= (int)cycles;
    while (ch->timer <= 0) {
        ch->timer += (2048 - ch->frequency) * 4;
        ch->duty_pos = (ch->duty_pos + 1) & 7;
    }
    ch->output = (ch->enabled && ch->dac_enabled)
                 ? DUTY_TABLE[ch->duty][ch->duty_pos] * ch->envelope.volume
                 : 0;
}

static void ch2_step(Channel2 *ch, uint32_t cycles) {
    ch->timer -= (int)cycles;
    while (ch->timer <= 0) {
        ch->timer += (2048 - ch->frequency) * 4;
        ch->duty_pos = (ch->duty_pos + 1) & 7;
    }
    ch->output = (ch->enabled && ch->dac_enabled)
                 ? DUTY_TABLE[ch->duty][ch->duty_pos] * ch->envelope.volume
                 : 0;
}

static void ch3_step(Channel3 *ch, uint32_t cycles) {
    ch->timer -= (int)cycles;
    while (ch->timer <= 0) {
        ch->timer += (2048 - ch->frequency) * 2;
        ch->pos = (ch->pos + 1) & 31;
        /* Each byte holds 2 samples (4 bits each) */
        uint8_t raw = ch->wave_ram[ch->pos >> 1];
        ch->sample = (ch->pos & 1) ? (raw & 0x0F) : (raw >> 4);
    }

    if (!ch->enabled || !ch->dac_enabled) {
        ch->output = 0;
        return;
    }

    /* Volume shift (NR32 bits 6-5): 0=mute, 1=100%, 2=50%, 3=25% */
    static const uint8_t shift[4] = {4, 0, 1, 2};
    ch->output = ch->sample >> shift[ch->volume_code];
}

static void ch4_step(Channel4 *ch, uint32_t cycles) {
    ch->timer -= (int)cycles;
    while (ch->timer <= 0) {
        ch->timer += NOISE_DIVISOR[ch->divisor_code] << ch->clock_shift;

        /* LFSR tick (Section 2.11.4) */
        uint8_t xor_bit = (ch->lfsr & 0x01) ^ ((ch->lfsr >> 1) & 0x01);
        ch->lfsr = (ch->lfsr >> 1) | (xor_bit << 14);
        if (ch->width_mode) {
            ch->lfsr = (ch->lfsr & ~(1 << 6)) | (xor_bit << 6);
        }
    }

    ch->output = (ch->enabled && ch->dac_enabled && !(ch->lfsr & 0x01))
                 ? ch->envelope.volume
                 : 0;
}

/* ---------- Frame sequencer ---------- */

static void frame_sequencer_step(APU *apu) {
    switch (apu->frame_seq_step) {
        case 0:
            length_clock(&apu->ch1.length, &apu->ch1.enabled);
            length_clock(&apu->ch2.length, &apu->ch2.enabled);
            length_clock(&apu->ch3.length, &apu->ch3.enabled);
            length_clock(&apu->ch4.length, &apu->ch4.enabled);
            break;
        case 2:
            length_clock(&apu->ch1.length, &apu->ch1.enabled);
            length_clock(&apu->ch2.length, &apu->ch2.enabled);
            length_clock(&apu->ch3.length, &apu->ch3.enabled);
            length_clock(&apu->ch4.length, &apu->ch4.enabled);
            sweep_clock(&apu->ch1);
            break;
        case 4:
            length_clock(&apu->ch1.length, &apu->ch1.enabled);
            length_clock(&apu->ch2.length, &apu->ch2.enabled);
            length_clock(&apu->ch3.length, &apu->ch3.enabled);
            length_clock(&apu->ch4.length, &apu->ch4.enabled);
            break;
        case 6:
            length_clock(&apu->ch1.length, &apu->ch1.enabled);
            length_clock(&apu->ch2.length, &apu->ch2.enabled);
            length_clock(&apu->ch3.length, &apu->ch3.enabled);
            length_clock(&apu->ch4.length, &apu->ch4.enabled);
            sweep_clock(&apu->ch1);
            break;
        case 7:
            envelope_clock(&apu->ch1.envelope);
            envelope_clock(&apu->ch2.envelope);
            envelope_clock(&apu->ch4.envelope);
            break;
    }
    apu->frame_seq_step = (apu->frame_seq_step + 1) & 7;
}

/* ---------- Mix channels ---------- */

static void apu_mix_sample(APU *apu) {
    if (!apu->power || apu->buffer_full) return;

    int16_t left = 0, right = 0;
    uint8_t pan = apu->panning;

    /* Channel outputs are 0-15, mix and apply master volume */
    int16_t samples[4] = {
        (int16_t)apu->ch1.output,
        (int16_t)apu->ch2.output,
        (int16_t)apu->ch3.output,
        (int16_t)apu->ch4.output,
    };

    for (int i = 0; i < 4; i++) {
        if (pan & (1 << i))       right += samples[i];
        if (pan & (1 << (i + 4))) left += samples[i];
    }

    /* Scale: each channel 0-15, 4 channels max = 60; master volume 0-7 */
    /* Normalize to ~int16 range: 60 * 8 = 480, scale by ~68 to reach ~32767 */
    left  = (int16_t)(left  * (apu->left_volume + 1) * 68);
    right = (int16_t)(right * (apu->right_volume + 1) * 68);

    apu->buffer[apu->buffer_pos].left = left;
    apu->buffer[apu->buffer_pos].right = right;
    apu->buffer_pos++;

    if (apu->buffer_pos >= APU_BUFFER_SIZE) {
        apu->buffer_full = true;
        apu->buffer_pos = 0;
    }
}

/* ---------- Main step ---------- */

void apu_step(APU *apu, uint32_t cycles) {
    if (!apu->power) return;

    /* Step channels */
    ch1_step(&apu->ch1, cycles);
    ch2_step(&apu->ch2, cycles);
    ch3_step(&apu->ch3, cycles);
    ch4_step(&apu->ch4, cycles);

    /* Frame sequencer */
    apu->frame_seq_counter += cycles;
    while (apu->frame_seq_counter >= FRAME_SEQ_PERIOD) {
        apu->frame_seq_counter -= FRAME_SEQ_PERIOD;
        frame_sequencer_step(apu);
    }

    /* Downsample to output rate */
    apu->sample_counter += cycles;
    while (apu->sample_counter >= apu->sample_period) {
        apu->sample_counter -= apu->sample_period;
        apu_mix_sample(apu);
    }
}

/* ---------- Register access ---------- */

/* Read masks: bits that always read as 1 */
static const uint8_t NR_READ_MASKS[] = {
    /* NR10-NR14 */ 0x80, 0x3F, 0x00, 0xFF, 0xBF,
    /* gap */       0xFF,
    /* NR21-NR24 */ 0x3F, 0x00, 0xFF, 0xBF,
    /* NR30-NR34 */ 0x7F, 0xFF, 0x9F, 0xFF, 0xBF,
    /* gap */       0xFF,
    /* NR41-NR44 */ 0xFF, 0x00, 0x00, 0xBF,
    /* NR50-NR52 */ 0x00, 0x00, 0x70,
};

uint8_t apu_read(APU *apu, uint16_t addr) {
    uint8_t reg = addr & 0xFF;

    /* Wave RAM: FF30-FF3F */
    if (reg >= 0x30 && reg <= 0x3F) {
        return apu->ch3.wave_ram[reg - 0x30];
    }

    if (reg < 0x10 || reg > 0x26) return 0xFF;

    uint8_t idx = reg - 0x10;
    uint8_t val = apu->io ? apu->io[reg] : 0;

    /* NR52: power + channel status */
    if (reg == 0x26) {
        val = 0x70;
        if (apu->power) val |= 0x80;
        if (apu->ch1.enabled) val |= 0x01;
        if (apu->ch2.enabled) val |= 0x02;
        if (apu->ch3.enabled) val |= 0x04;
        if (apu->ch4.enabled) val |= 0x08;
        return val;
    }

    if (idx < sizeof(NR_READ_MASKS)) {
        return val | NR_READ_MASKS[idx];
    }
    return val;
}

static void trigger_ch1(APU *apu) {
    Channel1 *ch = &apu->ch1;
    ch->enabled = true;
    if (ch->length.counter == 0) ch->length.counter = 64;
    ch->timer = (2048 - ch->frequency) * 4;
    ch->envelope.volume = ch->envelope.initial;
    ch->envelope.timer = ch->envelope.period ? ch->envelope.period : 8;
    ch->sweep.shadow = ch->frequency;
    ch->sweep.timer = ch->sweep.period ? ch->sweep.period : 8;
    ch->sweep.negate_used = false;
    ch->sweep.enabled = (ch->sweep.period > 0 || ch->sweep.shift > 0);
    if (ch->sweep.shift > 0) {
        if (sweep_calculate(ch) > 2047) ch->enabled = false;
    }
    if (!ch->dac_enabled) ch->enabled = false;
}

static void trigger_ch2(APU *apu) {
    Channel2 *ch = &apu->ch2;
    ch->enabled = true;
    if (ch->length.counter == 0) ch->length.counter = 64;
    ch->timer = (2048 - ch->frequency) * 4;
    ch->envelope.volume = ch->envelope.initial;
    ch->envelope.timer = ch->envelope.period ? ch->envelope.period : 8;
    if (!ch->dac_enabled) ch->enabled = false;
}

static void trigger_ch3(APU *apu) {
    Channel3 *ch = &apu->ch3;
    ch->enabled = true;
    if (ch->length.counter == 0) ch->length.counter = 256;
    ch->timer = (2048 - ch->frequency) * 2;
    ch->pos = 0;
    if (!ch->dac_enabled) ch->enabled = false;
}

static void trigger_ch4(APU *apu) {
    Channel4 *ch = &apu->ch4;
    ch->enabled = true;
    if (ch->length.counter == 0) ch->length.counter = 64;
    ch->timer = NOISE_DIVISOR[ch->divisor_code] << ch->clock_shift;
    ch->envelope.volume = ch->envelope.initial;
    ch->envelope.timer = ch->envelope.period ? ch->envelope.period : 8;
    ch->lfsr = 0x7FFF;
    if (!ch->dac_enabled) ch->enabled = false;
}

void apu_write(APU *apu, uint16_t addr, uint8_t val) {
    uint8_t reg = addr & 0xFF;

    /* Wave RAM */
    if (reg >= 0x30 && reg <= 0x3F) {
        apu->ch3.wave_ram[reg - 0x30] = val;
        return;
    }

    if (reg < 0x10 || reg > 0x26) return;

    /* NR52: only bit 7 writable */
    if (reg == 0x26) {
        bool new_power = (val & 0x80) != 0;
        if (!new_power && apu->power) {
            /* Power off: clear all registers */
            for (int i = 0x10; i <= 0x25; i++) {
                if (apu->io) apu->io[i] = 0;
            }
            memset(&apu->ch1, 0, sizeof(apu->ch1));
            memset(&apu->ch2, 0, sizeof(apu->ch2));
            /* Preserve wave RAM */
            uint8_t wave_bak[16];
            memcpy(wave_bak, apu->ch3.wave_ram, 16);
            memset(&apu->ch3, 0, sizeof(apu->ch3));
            memcpy(apu->ch3.wave_ram, wave_bak, 16);
            memset(&apu->ch4, 0, sizeof(apu->ch4));
            apu->ch4.lfsr = 0x7FFF;
        }
        apu->power = new_power;
        return;
    }

    if (!apu->power) return; /* Registers read-only when powered off */

    /* Store in I/O */
    if (apu->io) apu->io[reg] = val;

    switch (reg) {
        /* ---- Channel 1 ---- */
        case 0x10: /* NR10 */
            apu->ch1.sweep.period = (val >> 4) & 0x07;
            apu->ch1.sweep.direction = (val >> 3) & 0x01;
            apu->ch1.sweep.shift = val & 0x07;
            if (apu->ch1.sweep.direction == 0 && apu->ch1.sweep.negate_used) {
                apu->ch1.enabled = false;
            }
            break;
        case 0x11: /* NR11 */
            apu->ch1.duty = (val >> 6) & 0x03;
            apu->ch1.length.counter = 64 - (val & 0x3F);
            break;
        case 0x12: /* NR12 */
            apu->ch1.envelope.initial = (val >> 4) & 0x0F;
            apu->ch1.envelope.direction = (val >> 3) & 0x01;
            apu->ch1.envelope.period = val & 0x07;
            apu->ch1.dac_enabled = (val & 0xF8) != 0;
            if (!apu->ch1.dac_enabled) apu->ch1.enabled = false;
            break;
        case 0x13: /* NR13 */
            apu->ch1.frequency = (apu->ch1.frequency & 0x700) | val;
            break;
        case 0x14: /* NR14 */
            apu->ch1.frequency = (apu->ch1.frequency & 0xFF) | ((uint16_t)(val & 0x07) << 8);
            apu->ch1.length.enabled = (val & 0x40) != 0;
            if (val & 0x80) trigger_ch1(apu);
            break;

        /* ---- Channel 2 ---- */
        case 0x16: /* NR21 */
            apu->ch2.duty = (val >> 6) & 0x03;
            apu->ch2.length.counter = 64 - (val & 0x3F);
            break;
        case 0x17: /* NR22 */
            apu->ch2.envelope.initial = (val >> 4) & 0x0F;
            apu->ch2.envelope.direction = (val >> 3) & 0x01;
            apu->ch2.envelope.period = val & 0x07;
            apu->ch2.dac_enabled = (val & 0xF8) != 0;
            if (!apu->ch2.dac_enabled) apu->ch2.enabled = false;
            break;
        case 0x18: /* NR23 */
            apu->ch2.frequency = (apu->ch2.frequency & 0x700) | val;
            break;
        case 0x19: /* NR24 */
            apu->ch2.frequency = (apu->ch2.frequency & 0xFF) | ((uint16_t)(val & 0x07) << 8);
            apu->ch2.length.enabled = (val & 0x40) != 0;
            if (val & 0x80) trigger_ch2(apu);
            break;

        /* ---- Channel 3 ---- */
        case 0x1A: /* NR30 */
            apu->ch3.dac_enabled = (val & 0x80) != 0;
            if (!apu->ch3.dac_enabled) apu->ch3.enabled = false;
            break;
        case 0x1B: /* NR31 */
            apu->ch3.length.counter = 256 - val;
            break;
        case 0x1C: /* NR32 */
            apu->ch3.volume_code = (val >> 5) & 0x03;
            break;
        case 0x1D: /* NR33 */
            apu->ch3.frequency = (apu->ch3.frequency & 0x700) | val;
            break;
        case 0x1E: /* NR34 */
            apu->ch3.frequency = (apu->ch3.frequency & 0xFF) | ((uint16_t)(val & 0x07) << 8);
            apu->ch3.length.enabled = (val & 0x40) != 0;
            if (val & 0x80) trigger_ch3(apu);
            break;

        /* ---- Channel 4 ---- */
        case 0x20: /* NR41 */
            apu->ch4.length.counter = 64 - (val & 0x3F);
            break;
        case 0x21: /* NR42 */
            apu->ch4.envelope.initial = (val >> 4) & 0x0F;
            apu->ch4.envelope.direction = (val >> 3) & 0x01;
            apu->ch4.envelope.period = val & 0x07;
            apu->ch4.dac_enabled = (val & 0xF8) != 0;
            if (!apu->ch4.dac_enabled) apu->ch4.enabled = false;
            break;
        case 0x22: /* NR43 */
            apu->ch4.clock_shift = (val >> 4) & 0x0F;
            apu->ch4.width_mode = (val & 0x08) != 0;
            apu->ch4.divisor_code = val & 0x07;
            break;
        case 0x23: /* NR44 */
            apu->ch4.length.enabled = (val & 0x40) != 0;
            if (val & 0x80) trigger_ch4(apu);
            break;

        /* ---- Master ---- */
        case 0x24: /* NR50 */
            apu->left_volume = (val >> 4) & 0x07;
            apu->right_volume = val & 0x07;
            break;
        case 0x25: /* NR51 */
            apu->panning = val;
            break;
    }
}

uint32_t apu_get_samples(APU *apu, GbpyAudioSample *out, uint32_t max_samples) {
    uint32_t avail = apu->buffer_full ? APU_BUFFER_SIZE : apu->buffer_pos;
    uint32_t count = avail < max_samples ? avail : max_samples;

    if (count > 0 && out) {
        memcpy(out, apu->buffer, count * sizeof(GbpyAudioSample));
        /* Shift remaining samples */
        if (count < avail) {
            memmove(apu->buffer, apu->buffer + count,
                    (avail - count) * sizeof(GbpyAudioSample));
        }
        apu->buffer_pos = avail - count;
        apu->buffer_full = false;
    }

    return count;
}

/* Serialization */
size_t apu_serialize(const APU *apu, uint8_t *buf, size_t buf_size) {
    size_t needed = sizeof(Channel1) + sizeof(Channel2) + sizeof(Channel3)
                  + sizeof(Channel4) + 32;
    if (!buf || buf_size < needed) return needed;

    size_t pos = 0;
    #define S(f) memcpy(buf+pos, &apu->f, sizeof(apu->f)); pos += sizeof(apu->f)
    S(ch1); S(ch2); S(ch3); S(ch4);
    S(power); S(left_volume); S(right_volume); S(panning);
    S(frame_seq_counter); S(frame_seq_step);
    S(sample_counter);
    #undef S
    return pos;
}

size_t apu_deserialize(APU *apu, const uint8_t *buf, size_t buf_size) {
    size_t pos = 0;
    #define L(f) do { if(pos+sizeof(apu->f)>buf_size) return 0; \
        memcpy(&apu->f, buf+pos, sizeof(apu->f)); pos += sizeof(apu->f); } while(0)
    L(ch1); L(ch2); L(ch3); L(ch4);
    L(power); L(left_volume); L(right_volume); L(panning);
    L(frame_seq_counter); L(frame_seq_step);
    L(sample_counter);
    #undef L
    return pos;
}
