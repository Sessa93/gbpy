/*
 * ARM7TDMI CPU for Game Boy Advance
 *
 * 32-bit RISC processor running at 16.78 MHz
 * Two instruction sets:
 *   ARM mode:   32-bit instructions
 *   Thumb mode: 16-bit instructions (subset, more compact)
 *
 * 16 general-purpose registers (R0-R15)
 *   R13 = SP (Stack Pointer)
 *   R14 = LR (Link Register)
 *   R15 = PC (Program Counter)
 *
 * CPSR: Current Program Status Register
 *   Bits 31-28: N Z C V (condition flags)
 *   Bits 7-0:   I F T M4-M0 (control bits)
 *     I: IRQ disable, F: FIQ disable, T: Thumb state
 *     M: processor mode (USR/FIQ/IRQ/SVC/ABT/UND/SYS)
 *
 * 7 processor modes, each with banked registers:
 *   USR (0x10): Normal execution
 *   FIQ (0x11): Fast interrupt (R8-R14 banked)
 *   IRQ (0x12): Normal interrupt (R13-R14 banked)
 *   SVC (0x13): Software interrupt (R13-R14 banked)
 *   ABT (0x17): Abort (R13-R14 banked)
 *   UND (0x1B): Undefined (R13-R14 banked)
 *   SYS (0x1F): Privileged user (shares USR regs)
 */

#ifndef GBPY_ARM7TDMI_H
#define GBPY_ARM7TDMI_H

#include "types.h"

/* Processor modes */
#define ARM_MODE_USR  0x10
#define ARM_MODE_FIQ  0x11
#define ARM_MODE_IRQ  0x12
#define ARM_MODE_SVC  0x13
#define ARM_MODE_ABT  0x17
#define ARM_MODE_UND  0x1B
#define ARM_MODE_SYS  0x1F

/* CPSR flag bits */
#define ARM_FLAG_N    (1U << 31)  /* Negative */
#define ARM_FLAG_Z    (1U << 30)  /* Zero */
#define ARM_FLAG_C    (1U << 29)  /* Carry */
#define ARM_FLAG_V    (1U << 28)  /* Overflow */
#define ARM_FLAG_I    (1U << 7)   /* IRQ disable */
#define ARM_FLAG_F    (1U << 6)   /* FIQ disable */
#define ARM_FLAG_T    (1U << 5)   /* Thumb state */
#define ARM_MODE_MASK 0x1F

/* GBA interrupt flags (at 0x04000200/0x04000202) */
#define GBA_INT_VBLANK   (1 << 0)
#define GBA_INT_HBLANK   (1 << 1)
#define GBA_INT_VCOUNT   (1 << 2)
#define GBA_INT_TIMER0   (1 << 3)
#define GBA_INT_TIMER1   (1 << 4)
#define GBA_INT_TIMER2   (1 << 5)
#define GBA_INT_TIMER3   (1 << 6)
#define GBA_INT_SERIAL   (1 << 7)
#define GBA_INT_DMA0     (1 << 8)
#define GBA_INT_DMA1     (1 << 9)
#define GBA_INT_DMA2     (1 << 10)
#define GBA_INT_DMA3     (1 << 11)
#define GBA_INT_KEYPAD   (1 << 12)
#define GBA_INT_GAMEPAK  (1 << 13)

/* Memory access callbacks */
typedef uint8_t  (*arm7_read8_fn)(void *ctx, uint32_t addr);
typedef uint16_t (*arm7_read16_fn)(void *ctx, uint32_t addr);
typedef uint32_t (*arm7_read32_fn)(void *ctx, uint32_t addr);
typedef void (*arm7_write8_fn)(void *ctx, uint32_t addr, uint8_t val);
typedef void (*arm7_write16_fn)(void *ctx, uint32_t addr, uint16_t val);
typedef void (*arm7_write32_fn)(void *ctx, uint32_t addr, uint32_t val);

typedef struct ARM7TDMI {
    /* Registers: R0-R15 for current mode */
    uint32_t r[16];

    /* CPSR and SPSR */
    uint32_t cpsr;
    uint32_t spsr;

    /* Banked registers per mode */
    /* FIQ: R8-R14, SPSR */
    uint32_t fiq_r8_r12[5];  /* R8_fiq - R12_fiq */
    uint32_t fiq_r13, fiq_r14, fiq_spsr;
    /* Save USR R8-R12 when in FIQ */
    uint32_t usr_r8_r12[5];

    /* IRQ: R13, R14, SPSR */
    uint32_t irq_r13, irq_r14, irq_spsr;

    /* SVC: R13, R14, SPSR */
    uint32_t svc_r13, svc_r14, svc_spsr;

    /* ABT: R13, R14, SPSR */
    uint32_t abt_r13, abt_r14, abt_spsr;

    /* UND: R13, R14, SPSR */
    uint32_t und_r13, und_r14, und_spsr;

    /* USR/SYS: R13, R14 (shared) */
    uint32_t usr_r13, usr_r14;

    /* Pipeline */
    uint32_t pipeline[2];  /* Prefetch pipeline */
    bool pipeline_valid;

    /* State */
    bool halted;
    uint64_t cycles;
    uint64_t total_cycles;

    /* IRQ/FIQ pending */
    bool irq_pending;
    bool fiq_pending;

    /* Memory callbacks */
    arm7_read8_fn read8;
    arm7_read16_fn read16;
    arm7_read32_fn read32;
    arm7_write8_fn write8;
    arm7_write16_fn write16;
    arm7_write32_fn write32;
    void *mem_ctx;
} ARM7TDMI;

void arm7_init(ARM7TDMI *cpu);
void arm7_reset(ARM7TDMI *cpu);

/* Execute one instruction. Returns cycles consumed. */
uint32_t arm7_step(ARM7TDMI *cpu);

/* Request interrupt */
void arm7_request_irq(ARM7TDMI *cpu);
void arm7_request_fiq(ARM7TDMI *cpu);

/* Handle pending interrupts */
void arm7_check_interrupts(ARM7TDMI *cpu);

/* Mode switching */
void arm7_switch_mode(ARM7TDMI *cpu, uint8_t new_mode);

/* Serialization */
size_t arm7_serialize(const ARM7TDMI *cpu, uint8_t *buf, size_t buf_size);
size_t arm7_deserialize(ARM7TDMI *cpu, const uint8_t *buf, size_t buf_size);

#endif /* GBPY_ARM7TDMI_H */
