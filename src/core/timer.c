/*
 * Game Boy Timer Implementation
 * Section 2.10 of Game Boy CPU Manual
 */

#include "timer.h"

/* Timer clock select (TAC bits 1-0): which bit of div_counter to watch */
static const uint16_t TAC_BIT[4] = {
    1 << 9,   /* 4096 Hz   = bit 9 */
    1 << 3,   /* 262144 Hz = bit 3 */
    1 << 5,   /* 65536 Hz  = bit 5 */
    1 << 7,   /* 16384 Hz  = bit 7 */
};

/* Timer interrupt bit */
#define INT_TIMER 0x04

void timer_init(GbTimer *timer) {
    memset(timer, 0, sizeof(GbTimer));
    /* Set post-boot DIV value (DMG: DIV = 0xAB) */
    timer->div_counter = 0xABCC;
}

void timer_reset(GbTimer *timer) {
    /* After boot ROM on DMG, DIV = 0xAB, so div_counter >> 8 = 0xAB.
     * The exact lower byte depends on boot ROM timing (~0xCC). */
    timer->div_counter = 0xABCC;
    timer->tima = 0;
    timer->tma = 0;
    timer->tac = 0;
    timer->tima_overflow = false;
    timer->overflow_cycles = 0;
}

uint8_t timer_step(GbTimer *timer, uint32_t cycles) {
    uint8_t interrupts = 0;

    for (uint32_t i = 0; i < cycles; i++) {
        uint16_t old_div = timer->div_counter;
        timer->div_counter++;

        /* Handle pending TIMA overflow (1 M-cycle delay) */
        if (timer->tima_overflow) {
            timer->overflow_cycles++;
            if (timer->overflow_cycles >= 4) {
                timer->tima = timer->tma;
                interrupts |= INT_TIMER;
                timer->tima_overflow = false;
                timer->overflow_cycles = 0;
            }
        }

        /* Check if timer enabled (TAC bit 2) */
        if (!(timer->tac & 0x04)) continue;

        uint16_t bit = TAC_BIT[timer->tac & 0x03];

        /* Falling edge detection on the selected bit */
        if ((old_div & bit) && !(timer->div_counter & bit)) {
            timer->tima++;
            if (timer->tima == 0) {
                /* Overflow: set pending */
                timer->tima_overflow = true;
                timer->overflow_cycles = 0;
            }
        }
    }

    return interrupts;
}

uint8_t timer_read(const GbTimer *timer, uint16_t addr) {
    switch (addr & 0xFF) {
        case 0x04: return (uint8_t)(timer->div_counter >> 8);
        case 0x05: return timer->tima;
        case 0x06: return timer->tma;
        case 0x07: return timer->tac; /* Mask applied by MMU */
        default:   return 0xFF;
    }
}

void timer_write(GbTimer *timer, uint16_t addr, uint8_t val) {
    switch (addr & 0xFF) {
        case 0x04: {
            /* Writing any value to DIV resets the internal counter to 0.
             * If the timer is enabled and the currently selected bit was 1,
             * resetting to 0 creates a falling edge → increment TIMA. */
            if (timer->tac & 0x04) {
                uint16_t bit = TAC_BIT[timer->tac & 0x03];
                if (timer->div_counter & bit) {
                    timer->tima++;
                    if (timer->tima == 0) {
                        timer->tima_overflow = true;
                        timer->overflow_cycles = 0;
                    }
                }
            }
            timer->div_counter = 0;
            break;
        }
        case 0x05:
            /* Writing to TIMA during the overflow delay cycle has special behavior:
             * - During the 4-cycle delay before reload: write is accepted, cancels reload
             * - On the same cycle as reload: write is ignored (TMA wins) */
            if (timer->tima_overflow && timer->overflow_cycles < 4) {
                timer->tima = val;
                timer->tima_overflow = false;
            } else {
                timer->tima = val;
                timer->tima_overflow = false;
            }
            break;
        case 0x06:
            timer->tma = val;
            /* If TMA is written on the same cycle TIMA was reloaded from TMA,
             * the new TMA value is also loaded into TIMA */
            break;
        case 0x07: {
            /* Changing TAC can cause a spurious TIMA increment (the "glitch").
             * The internal signal is: timer_enabled AND selected_div_bit.
             * If this signal goes from 1→0 on a TAC write, TIMA increments. */
            uint8_t old_tac = timer->tac;
            uint8_t new_tac = val & 0x07;

            /* Old signal: was timer enabled AND selected bit high? */
            bool old_signal = (old_tac & 0x04) &&
                              (timer->div_counter & TAC_BIT[old_tac & 0x03]);
            /* New signal */
            bool new_signal = (new_tac & 0x04) &&
                              (timer->div_counter & TAC_BIT[new_tac & 0x03]);

            /* Falling edge of the AND signal → increment TIMA */
            if (old_signal && !new_signal) {
                timer->tima++;
                if (timer->tima == 0) {
                    timer->tima_overflow = true;
                    timer->overflow_cycles = 0;
                }
            }
            timer->tac = new_tac;
            break;
        }
    }
}

size_t timer_serialize(const GbTimer *timer, uint8_t *buf, size_t buf_size) {
    size_t needed = sizeof(GbTimer);
    if (!buf || buf_size < needed) return needed;
    memcpy(buf, timer, needed);
    return needed;
}

size_t timer_deserialize(GbTimer *timer, const uint8_t *buf, size_t buf_size) {
    if (buf_size < sizeof(GbTimer)) return 0;
    memcpy(timer, buf, sizeof(GbTimer));
    return sizeof(GbTimer);
}
