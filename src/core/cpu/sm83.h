/*
 * SM83 CPU - Game Boy / Game Boy Color Processor
 *
 * Reference: Game Boy CPU Manual v1.01 (resources/gba-manual.md)
 *
 * Section 2.4: "The GameBoy uses a computer chip similar to an Intel 8080.
 *              It contains all of the instructions of an 8080 except there
 *              are no exchange instructions."
 *
 * Section 3.2: CPU has registers A,B,C,D,E,F,H,L (8-bit) and SP,PC (16-bit).
 *              Registers can be paired: AF, BC, DE, HL.
 *
 * Section 3.2.2: Flag Register (F):
 *   Bit 7: Z (Zero), Bit 6: N (Subtract), Bit 5: H (Half-Carry), Bit 4: C (Carry)
 *
 * Section 2.3: Clock Speed: 4.194304 MHz (4.295454 SGB, 4.194/8.388 GBC)
 *              1 machine cycle = 4 clock cycles
 */

#ifndef GBPY_SM83_H
#define GBPY_SM83_H

#include "types.h"

/* Flag bit positions (Section 3.2.2) */
#define FLAG_Z  0x80   /* Zero Flag */
#define FLAG_N  0x40   /* Subtract Flag */
#define FLAG_H  0x20   /* Half Carry Flag */
#define FLAG_C  0x10   /* Carry Flag */

/* Interrupt vectors (Section 2.5.4 / 2.12.2) */
#define INT_VBLANK_ADDR   0x0040  /* V-Blank ~59.7 Hz */
#define INT_LCD_ADDR      0x0048  /* LCDC Status */
#define INT_TIMER_ADDR    0x0050  /* Timer Overflow */
#define INT_SERIAL_ADDR   0x0058  /* Serial Transfer Completion */
#define INT_JOYPAD_ADDR   0x0060  /* High-to-Low P10-P13 */

/* Interrupt flag bits (Section 2.13.1, register FF0F) */
#define INT_VBLANK_BIT    0x01
#define INT_LCD_BIT       0x02
#define INT_TIMER_BIT     0x04
#define INT_SERIAL_BIT    0x08
#define INT_JOYPAD_BIT    0x10

/* Forward declarations */
typedef struct SM83 SM83;

/* Memory read/write callbacks - allows the CPU to be independent of memory implementation */
typedef uint8_t (*sm83_read_fn)(void *ctx, uint16_t addr);
typedef void    (*sm83_write_fn)(void *ctx, uint16_t addr, uint8_t val);

/*
 * SM83 CPU State
 *
 * Register layout (Section 3.2.1):
 *   15..8  7..0
 *     A      F
 *     B      C
 *     D      E
 *     H      L
 *         SP
 *         PC
 */
struct SM83 {
    /* Registers - using a union for paired access */
    union {
        struct { uint8_t f, a; };  /* Note: little-endian order */
        uint16_t af;
    };
    union {
        struct { uint8_t c, b; };
        uint16_t bc;
    };
    union {
        struct { uint8_t e, d; };
        uint16_t de;
    };
    union {
        struct { uint8_t l, h; };
        uint16_t hl;
    };

    uint16_t sp;    /* Stack Pointer (Section 3.2.4) */
    uint16_t pc;    /* Program Counter (Section 3.2.3) */

    /* Interrupt Master Enable flag (Section 2.12.1) */
    bool ime;
    bool ime_pending;   /* EI enables after next instruction */

    /* HALT and STOP states (Sections 2.7.2, 2.7.3) */
    bool halted;
    bool stopped;
    bool halt_bug;      /* HALT bug when IME=0 (Section 2.7.3) */

    /* GBC double speed mode */
    bool double_speed;

    /* Cycle counter */
    uint32_t cycles;         /* Cycles executed this step */
    uint64_t total_cycles;   /* Total cycles since reset */

    /* Memory interface */
    sm83_read_fn  read;
    sm83_write_fn write;
    void *mem_ctx;
};

/* ---------- API ---------- */

/* Initialize CPU to power-up state (Section 2.7.1) */
void sm83_init(SM83 *cpu, sm83_read_fn read, sm83_write_fn write, void *ctx);

/* Reset CPU (sets registers per Section 2.7.1 Power Up Sequence) */
void sm83_reset(SM83 *cpu, GbpyMode mode);

/* Execute one instruction, returns cycles consumed */
uint32_t sm83_step(SM83 *cpu);

/* Request an interrupt (sets IF bit) */
void sm83_request_interrupt(SM83 *cpu, uint8_t interrupt);

/* Handle pending interrupts */
uint32_t sm83_handle_interrupts(SM83 *cpu);

/* Execute a CB-prefixed instruction */
uint32_t sm83_execute_cb(SM83 *cpu);

/* Get/set 16-bit register pairs for external access */
uint16_t sm83_get_af(const SM83 *cpu);
uint16_t sm83_get_bc(const SM83 *cpu);
uint16_t sm83_get_de(const SM83 *cpu);
uint16_t sm83_get_hl(const SM83 *cpu);

/* State serialization for save states */
size_t sm83_serialize(const SM83 *cpu, uint8_t *buf, size_t buf_size);
size_t sm83_deserialize(SM83 *cpu, const uint8_t *buf, size_t buf_size);

#endif /* GBPY_SM83_H */
