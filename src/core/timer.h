/*
 * Game Boy Timer
 *
 * Reference: Game Boy CPU Manual v1.01 (resources/gba-manual.md)
 * Section 2.10: Timer
 *
 * Registers:
 *   FF04 - DIV:  Divider Register (increments at 16384 Hz, resets on write)
 *   FF05 - TIMA: Timer counter (increments at TAC frequency)
 *   FF06 - TMA:  Timer modulo (TIMA reloads from this on overflow)
 *   FF07 - TAC:  Timer control
 *     Bit 2: Timer enable
 *     Bits 1-0: Clock select
 *       00 = 4096 Hz    (1024 cycles)
 *       01 = 262144 Hz  (16 cycles)
 *       10 = 65536 Hz   (64 cycles)
 *       11 = 16384 Hz   (256 cycles)
 *
 * Internal: DIV is actually a 16-bit counter incrementing every T-cycle.
 *   DIV register (FF04) = bits 15-8 of this counter.
 *   TIMA increments when a specific bit of the counter transitions 1->0.
 */

#ifndef GBPY_TIMER_H
#define GBPY_TIMER_H

#include "types.h"

typedef struct GbTimer {
    uint16_t div_counter;   /* Internal 16-bit counter */
    uint8_t tima;           /* FF05 - Timer counter */
    uint8_t tma;            /* FF06 - Timer modulo */
    uint8_t tac;            /* FF07 - Timer control */
    bool tima_overflow;     /* Pending overflow */
    uint8_t overflow_cycles;/* Delay before reload */
} GbTimer;

void timer_init(GbTimer *timer);
void timer_reset(GbTimer *timer);

/* Step timer by CPU cycles. Returns INT_TIMER bit if interrupt should fire. */
uint8_t timer_step(GbTimer *timer, uint32_t cycles);

/* Read/write timer registers */
uint8_t timer_read(const GbTimer *timer, uint16_t addr);
void timer_write(GbTimer *timer, uint16_t addr, uint8_t val);

size_t timer_serialize(const GbTimer *timer, uint8_t *buf, size_t buf_size);
size_t timer_deserialize(GbTimer *timer, const uint8_t *buf, size_t buf_size);

#endif /* GBPY_TIMER_H */
