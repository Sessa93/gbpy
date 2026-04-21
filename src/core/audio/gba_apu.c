/*
 * GBA APU Implementation
 *
 * Two Direct Sound DMA FIFO channels (A, B) + legacy 4 GB channels.
 * DMA channels are timer-driven: when a timer overflows, a sample is
 * consumed from the FIFO and sent to the DAC.
 */

#include "gba_apu.h"
#include "../memory/gba_memory.h"

/* Duty cycle waveforms */
static const uint8_t duty_table[4][8] = {
    {0,0,0,0,0,0,0,1}, /* 12.5% */
    {1,0,0,0,0,0,0,1}, /* 25% */
    {1,0,0,0,0,1,1,1}, /* 50% */
    {0,1,1,1,1,1,1,0}, /* 75% */
};

/* ---------- FIFO ---------- */

void gba_fifo_reset(GbaFifo *fifo) {
    memset(fifo, 0, sizeof(GbaFifo));
}

void gba_fifo_write(GbaFifo *fifo, int8_t sample) {
    if (fifo->count >= GBA_FIFO_SIZE) return;
    fifo->buffer[fifo->write_pos] = sample;
    fifo->write_pos = (fifo->write_pos + 1) % GBA_FIFO_SIZE;
    fifo->count++;
}

void gba_fifo_write32(GbaFifo *fifo, uint32_t data) {
    gba_fifo_write(fifo, (int8_t)(data & 0xFF));
    gba_fifo_write(fifo, (int8_t)((data >> 8) & 0xFF));
    gba_fifo_write(fifo, (int8_t)((data >> 16) & 0xFF));
    gba_fifo_write(fifo, (int8_t)((data >> 24) & 0xFF));
}

static int8_t fifo_read(GbaFifo *fifo) {
    if (fifo->count == 0) return fifo->current_sample;
    int8_t sample = fifo->buffer[fifo->read_pos];
    fifo->read_pos = (fifo->read_pos + 1) % GBA_FIFO_SIZE;
    fifo->count--;
    fifo->current_sample = sample;
    return sample;
}

/* ---------- Init/Reset ---------- */

void gba_apu_init(GbaAPU *apu) {
    memset(apu, 0, sizeof(GbaAPU));
    /* ~16.78 MHz / 32768 Hz = ~512 cycles per sample */
    apu->cycles_per_sample = 16 * 1024 * 1024 / GBA_AUDIO_SAMPLE_RATE;
    apu->legacy.ch4_lfsr = 0x7FFF;
}

void gba_apu_reset(GbaAPU *apu) {
    GbaMemory *m = apu->mem;
    gba_apu_init(apu);
    apu->mem = m;
}

/* ---------- Legacy channel step helpers ---------- */

static void step_ch1_sweep(GbaLegacyAPU *leg) {
    if (leg->ch1_sweep_pace == 0) return;
    if (--leg->ch1_sweep_timer == 0) {
        leg->ch1_sweep_timer = leg->ch1_sweep_pace;
        uint16_t new_freq = leg->ch1_shadow_freq;
        uint16_t delta = new_freq >> leg->ch1_sweep_step;
        if (leg->ch1_sweep_dir) {
            new_freq -= delta;
        } else {
            new_freq += delta;
            if (new_freq > 2047) {
                leg->ch1_enabled = false;
                return;
            }
        }
        if (leg->ch1_sweep_step) {
            leg->ch1_shadow_freq = new_freq;
            leg->ch1_freq = new_freq;
        }
    }
}

static void step_length(bool *enabled, uint8_t *counter, bool length_en, uint8_t max) {
    if (!length_en) return;
    if (*counter > 0) {
        (*counter)--;
        if (*counter == 0) *enabled = false;
    }
}

static void step_envelope(uint8_t *volume, uint8_t *timer, uint16_t pace, uint16_t dir) {
    if (pace == 0) return;
    if (--(*timer) == 0) {
        *timer = pace;
        if (dir && *volume < 15) (*volume)++;
        else if (!dir && *volume > 0) (*volume)--;
    }
}

/* Step legacy channels by 1 sample period */
static void legacy_step_sample(GbaLegacyAPU *leg, int16_t *left, int16_t *right, uint16_t soundcnt_l) {
    /* Frame sequencer: runs at 512 Hz (every ~32768 CPU cycles at 16.78 MHz) */
    leg->frame_seq_timer++;
    if (leg->frame_seq_timer >= 64) { /* 32768/512 = 64 samples */
        leg->frame_seq_timer = 0;
        uint8_t step = leg->frame_seq_step;
        leg->frame_seq_step = (step + 1) % 8;

        /* Length: steps 0,2,4,6 */
        if ((step & 1) == 0) {
            step_length(&leg->ch1_enabled, &leg->ch1_length_counter, leg->ch1_length_en, 64);
            step_length(&leg->ch2_enabled, &leg->ch2_length_counter, leg->ch2_length_en, 64);
            step_length(&leg->ch3_enabled, (uint8_t*)&leg->ch3_length_counter, leg->ch3_length_en, 255);
            step_length(&leg->ch4_enabled, &leg->ch4_length_counter, leg->ch4_length_en, 64);
        }
        /* Sweep: steps 2,6 */
        if (step == 2 || step == 6) {
            step_ch1_sweep(leg);
        }
        /* Envelope: step 7 */
        if (step == 7) {
            step_envelope(&leg->ch1_volume, &leg->ch1_env_timer, leg->ch1_env_pace, leg->ch1_env_dir);
            step_envelope(&leg->ch2_volume, &leg->ch2_env_timer, leg->ch2_env_pace, leg->ch2_env_dir);
            step_envelope(&leg->ch4_volume, &leg->ch4_env_timer, leg->ch4_env_pace, leg->ch4_env_dir);
        }
    }

    /* Generate samples for each channel */
    int16_t ch_out[4] = {0, 0, 0, 0};

    /* CH1: Square + sweep */
    if (leg->ch1_enabled) {
        ch_out[0] = duty_table[leg->ch1_duty][leg->ch1_duty_pos] ? leg->ch1_volume : -leg->ch1_volume;
        leg->ch1_duty_pos = (leg->ch1_duty_pos + 1) & 7;
    }

    /* CH2: Square */
    if (leg->ch2_enabled) {
        ch_out[1] = duty_table[leg->ch2_duty][leg->ch2_duty_pos] ? leg->ch2_volume : -leg->ch2_volume;
        leg->ch2_duty_pos = (leg->ch2_duty_pos + 1) & 7;
    }

    /* CH3: Wave */
    if (leg->ch3_enabled && leg->ch3_dac_en) {
        uint8_t bank = leg->ch3_dimension ? 0 : leg->ch3_bank;
        uint8_t pos = leg->ch3_sample_pos;
        uint8_t byte = leg->wave_ram[bank][pos / 2];
        uint8_t sample = (pos & 1) ? (byte & 0xF) : (byte >> 4);
        int shift = 0;
        switch (leg->ch3_volume_code) {
            case 0: shift = 4; break; /* mute */
            case 1: shift = 0; break; /* 100% */
            case 2: shift = 1; break; /* 50% */
            case 3: shift = 2; break; /* 25% */
        }
        ch_out[2] = (int16_t)((sample >> shift) - 8);
        leg->ch3_sample_pos = (pos + 1) & 63;
    }

    /* CH4: Noise */
    if (leg->ch4_enabled) {
        uint8_t bit = leg->ch4_lfsr & 1;
        ch_out[3] = bit ? leg->ch4_volume : -leg->ch4_volume;
        /* Advance LFSR */
        uint16_t xor_bit = (leg->ch4_lfsr ^ (leg->ch4_lfsr >> 1)) & 1;
        leg->ch4_lfsr >>= 1;
        leg->ch4_lfsr |= xor_bit << 14;
        if (leg->ch4_width_mode) {
            leg->ch4_lfsr &= ~(1 << 6);
            leg->ch4_lfsr |= xor_bit << 6;
        }
    }

    /* Mixing: SOUNDCNT_L controls panning */
    uint8_t right_en = (soundcnt_l >> 8) & 0xF;
    uint8_t left_en = (soundcnt_l >> 12) & 0xF;
    uint8_t right_vol = soundcnt_l & 7;
    uint8_t left_vol = (soundcnt_l >> 4) & 7;

    int16_t l = 0, r = 0;
    for (int i = 0; i < 4; i++) {
        if (left_en & (1 << i)) l += ch_out[i];
        if (right_en & (1 << i)) r += ch_out[i];
    }

    *left = l * (left_vol + 1);
    *right = r * (right_vol + 1);
}

/* ---------- Timer overflow -> DMA sound ---------- */

/* Trigger only DMA1/2 for sound FIFO (SPECIAL timing on DMA1/2 = sound) */
static void dma_trigger_sound(GbaMemory *mem) {
    for (int ch = 1; ch <= 2; ch++) {
        GbaDMA *dma = &mem->dma[ch];
        if (!(dma->control & (1 << 15))) continue;
        uint8_t dma_timing = (dma->control >> 12) & 3;
        if (dma_timing == DMA_TIMING_SPECIAL) {
            dma->active = true;
            /* dma_run_channel will be called by gba_dma_step */
        }
    }
}

void gba_apu_timer_overflow(GbaAPU *apu, int timer_id) {
    GbaMemory *mem = apu->mem;
    if (!mem) return;
    uint16_t soundcnt_h = *(uint16_t *)&mem->io[0x082];

    /* Check FIFO A */
    if (SOUNDCNT_H_DMA_A_TIMER(soundcnt_h) == timer_id) {
        fifo_read(&apu->fifo_a);
        if (apu->fifo_a.count <= 16) {
            /* Request DMA refill for FIFO A (DMA1 or DMA2 only) */
            dma_trigger_sound(mem);
        }
    }

    /* Check FIFO B */
    if (SOUNDCNT_H_DMA_B_TIMER(soundcnt_h) == timer_id) {
        fifo_read(&apu->fifo_b);
        if (apu->fifo_b.count <= 16) {
            dma_trigger_sound(mem);
        }
    }
}

/* ---------- Register writes for legacy channels ---------- */

void gba_apu_write_reg(GbaAPU *apu, uint32_t addr, uint8_t val) {
    GbaLegacyAPU *leg = &apu->legacy;

    switch (addr) {
        /* NR10 - CH1 Sweep */
        case 0x060:
            leg->ch1_sweep_step = val & 7;
            leg->ch1_sweep_dir = (val >> 3) & 1;
            leg->ch1_sweep_pace = (val >> 4) & 7;
            break;

        /* NR11 - CH1 Duty/Length */
        case 0x062:
            leg->ch1_length = val & 0x3F;
            leg->ch1_duty = (val >> 6) & 3;
            leg->ch1_length_counter = 64 - leg->ch1_length;
            break;
        case 0x063:
            leg->ch1_env_pace = val & 7;
            leg->ch1_env_dir = (val >> 3) & 1;
            leg->ch1_env_vol = (val >> 4) & 0xF;
            break;

        /* NR13/14 - CH1 Freq */
        case 0x064:
            leg->ch1_freq = (leg->ch1_freq & 0x700) | val;
            break;
        case 0x065:
            leg->ch1_freq = (leg->ch1_freq & 0xFF) | ((val & 7) << 8);
            leg->ch1_length_en = (val >> 6) & 1;
            if (val & 0x80) { /* Trigger */
                leg->ch1_enabled = true;
                leg->ch1_volume = leg->ch1_env_vol;
                leg->ch1_env_timer = leg->ch1_env_pace;
                leg->ch1_shadow_freq = leg->ch1_freq;
                leg->ch1_sweep_timer = leg->ch1_sweep_pace;
                if (leg->ch1_length_counter == 0) leg->ch1_length_counter = 64;
            }
            break;

        /* NR21/22 - CH2 */
        case 0x068:
            leg->ch2_length = val & 0x3F;
            leg->ch2_duty = (val >> 6) & 3;
            leg->ch2_length_counter = 64 - leg->ch2_length;
            break;
        case 0x069:
            leg->ch2_env_pace = val & 7;
            leg->ch2_env_dir = (val >> 3) & 1;
            leg->ch2_env_vol = (val >> 4) & 0xF;
            break;
        case 0x06C:
            leg->ch2_freq = (leg->ch2_freq & 0x700) | val;
            break;
        case 0x06D:
            leg->ch2_freq = (leg->ch2_freq & 0xFF) | ((val & 7) << 8);
            leg->ch2_length_en = (val >> 6) & 1;
            if (val & 0x80) {
                leg->ch2_enabled = true;
                leg->ch2_volume = leg->ch2_env_vol;
                leg->ch2_env_timer = leg->ch2_env_pace;
                if (leg->ch2_length_counter == 0) leg->ch2_length_counter = 64;
            }
            break;

        /* NR30-34 - CH3 */
        case 0x070:
            leg->ch3_dimension = (val >> 5) & 1;
            leg->ch3_bank = (val >> 6) & 1;
            leg->ch3_dac_en = (val >> 7) & 1;
            if (!leg->ch3_dac_en) leg->ch3_enabled = false;
            break;
        case 0x072:
            leg->ch3_length = val;
            leg->ch3_length_counter = 256 - val;
            break;
        case 0x073:
            leg->ch3_volume_code = (val >> 5) & 3;
            break;
        case 0x074:
            leg->ch3_freq = (leg->ch3_freq & 0x700) | val;
            break;
        case 0x075:
            leg->ch3_freq = (leg->ch3_freq & 0xFF) | ((val & 7) << 8);
            leg->ch3_length_en = (val >> 6) & 1;
            if (val & 0x80) {
                leg->ch3_enabled = true;
                leg->ch3_sample_pos = 0;
                if (leg->ch3_length_counter == 0) leg->ch3_length_counter = 256;
            }
            break;

        /* NR41-44 - CH4 */
        case 0x078:
            leg->ch4_length = val & 0x3F;
            leg->ch4_length_counter = 64 - leg->ch4_length;
            break;
        case 0x079:
            leg->ch4_env_pace = val & 7;
            leg->ch4_env_dir = (val >> 3) & 1;
            leg->ch4_env_vol = (val >> 4) & 0xF;
            break;
        case 0x07C:
            leg->ch4_div_ratio = val & 7;
            leg->ch4_width_mode = (val >> 3) & 1;
            leg->ch4_clock_shift = (val >> 4) & 0xF;
            break;
        case 0x07D:
            leg->ch4_length_en = (val >> 6) & 1;
            if (val & 0x80) {
                leg->ch4_enabled = true;
                leg->ch4_volume = leg->ch4_env_vol;
                leg->ch4_env_timer = leg->ch4_env_pace;
                leg->ch4_lfsr = 0x7FFF;
                if (leg->ch4_length_counter == 0) leg->ch4_length_counter = 64;
            }
            break;

        /* NR52 - Master enable */
        case 0x084:
            leg->master_enable = (val >> 7) & 1;
            if (!leg->master_enable) {
                leg->ch1_enabled = false;
                leg->ch2_enabled = false;
                leg->ch3_enabled = false;
                leg->ch4_enabled = false;
            }
            break;

        /* Wave RAM: 0x090 - 0x09F */
        default:
            if (addr >= 0x090 && addr <= 0x09F) {
                uint8_t bank = leg->ch3_dimension ? (1 - leg->ch3_bank) : leg->ch3_bank;
                leg->wave_ram[bank][addr - 0x090] = val;
            }
            break;
    }
}

/* ---------- APU step (accumulates and outputs samples) ---------- */

void gba_apu_step(GbaAPU *apu, uint32_t cycles) {
    if (!apu->mem) return;
    GbaMemory *mem = apu->mem;

    if (!apu->legacy.master_enable) return;

    apu->sample_clock += cycles;

    while (apu->sample_clock >= apu->cycles_per_sample) {
        apu->sample_clock -= apu->cycles_per_sample;

        if (apu->buffer_full) continue;

        uint16_t soundcnt_h = *(uint16_t *)&mem->io[0x082];
        uint16_t soundcnt_l = *(uint16_t *)&mem->io[0x080];

        /* Legacy channel output */
        int16_t legacy_left = 0, legacy_right = 0;
        legacy_step_sample(&apu->legacy, &legacy_left, &legacy_right, soundcnt_l);

        /* Scale legacy by volume (SOUNDCNT_H bits 0-1) */
        uint8_t dmg_vol = SOUNDCNT_H_DMG_VOL(soundcnt_h);
        int shift = 2 - dmg_vol;
        if (shift < 0) shift = 0;
        legacy_left >>= shift;
        legacy_right >>= shift;

        /* DMA sound output */
        int16_t dma_a = apu->fifo_a.current_sample;
        int16_t dma_b = apu->fifo_b.current_sample;

        /* Volume: 0=50%, 1=100% */
        if (!SOUNDCNT_H_DMA_A_VOL(soundcnt_h)) dma_a >>= 1;
        if (!SOUNDCNT_H_DMA_B_VOL(soundcnt_h)) dma_b >>= 1;

        /* Scale to 16-bit range */
        dma_a <<= 2;
        dma_b <<= 2;

        /* Mix to stereo */
        int16_t left = legacy_left;
        int16_t right = legacy_right;

        if (SOUNDCNT_H_DMA_A_LEFT(soundcnt_h)) left += dma_a;
        if (SOUNDCNT_H_DMA_A_RIGHT(soundcnt_h)) right += dma_a;
        if (SOUNDCNT_H_DMA_B_LEFT(soundcnt_h)) left += dma_b;
        if (SOUNDCNT_H_DMA_B_RIGHT(soundcnt_h)) right += dma_b;

        /* Clamp */
        if (left > 32767) left = 32767;
        if (left < -32768) left = -32768;
        if (right > 32767) right = 32767;
        if (right < -32768) right = -32768;

        /* Store */
        uint32_t idx = apu->sample_count * 2;
        apu->sample_buffer[idx] = left;
        apu->sample_buffer[idx + 1] = right;
        apu->sample_count++;

        if (apu->sample_count >= GBA_AUDIO_BUFFER_SIZE) {
            apu->buffer_full = true;
        }
    }
}

/* ---------- Serialization ---------- */

size_t gba_apu_serialize(const GbaAPU *apu, uint8_t *buf, size_t buf_size) {
    size_t needed = sizeof(GbaFifo) * 2 + sizeof(GbaLegacyAPU) + 16;
    if (!buf || buf_size < needed) return needed;
    size_t pos = 0;
    memcpy(buf + pos, &apu->fifo_a, sizeof(GbaFifo)); pos += sizeof(GbaFifo);
    memcpy(buf + pos, &apu->fifo_b, sizeof(GbaFifo)); pos += sizeof(GbaFifo);
    memcpy(buf + pos, &apu->legacy, sizeof(GbaLegacyAPU)); pos += sizeof(GbaLegacyAPU);
    memcpy(buf + pos, &apu->sample_clock, sizeof(apu->sample_clock)); pos += sizeof(apu->sample_clock);
    return pos;
}

size_t gba_apu_deserialize(GbaAPU *apu, const uint8_t *buf, size_t buf_size) {
    size_t pos = 0;
    size_t needed = sizeof(GbaFifo) * 2 + sizeof(GbaLegacyAPU) + sizeof(apu->sample_clock);
    if (buf_size < needed) return 0;
    memcpy(&apu->fifo_a, buf + pos, sizeof(GbaFifo)); pos += sizeof(GbaFifo);
    memcpy(&apu->fifo_b, buf + pos, sizeof(GbaFifo)); pos += sizeof(GbaFifo);
    memcpy(&apu->legacy, buf + pos, sizeof(GbaLegacyAPU)); pos += sizeof(GbaLegacyAPU);
    memcpy(&apu->sample_clock, buf + pos, sizeof(apu->sample_clock)); pos += sizeof(apu->sample_clock);
    return pos;
}
