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
}

void timer_reset(GbTimer *timer) {
    timer->div_counter = 0;
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
        case 0x07: return timer->tac | 0xF8; /* Upper bits read as 1 */
        default:   return 0xFF;
    }
}

void timer_write(GbTimer *timer, uint16_t addr, uint8_t val) {
    switch (addr & 0xFF) {
        case 0x04:
            /* Writing any value to DIV resets it to 0 */
            timer->div_counter = 0;
            break;
        case 0x05:
            /* Writing to TIMA cancels pending overflow */
            timer->tima = val;
            timer->tima_overflow = false;
            break;
        case 0x06:
            timer->tma = val;
            break;
        case 0x07:
            timer->tac = val & 0x07;
            break;
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
