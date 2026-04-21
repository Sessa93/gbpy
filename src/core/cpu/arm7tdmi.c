/*
 * ARM7TDMI CPU Implementation for Game Boy Advance
 *
 * ARM instruction set: 32-bit, condition-code based execution
 * Thumb instruction set: 16-bit subset for code density
 *
 * This implements all ARM and Thumb instructions needed for GBA emulation.
 */

#include "arm7tdmi.h"
#include "../memory/gba_memory.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* ---------- Helpers ---------- */

#define PC    cpu->r[15]
#define LR    cpu->r[14]
#define SP    cpu->r[13]

#define GET_N()  ((cpu->cpsr >> 31) & 1)
#define GET_Z()  ((cpu->cpsr >> 30) & 1)
#define GET_C()  ((cpu->cpsr >> 29) & 1)
#define GET_V()  ((cpu->cpsr >> 28) & 1)

#define SET_N(v) cpu->cpsr = (cpu->cpsr & ~ARM_FLAG_N) | ((v) ? ARM_FLAG_N : 0)
#define SET_Z(v) cpu->cpsr = (cpu->cpsr & ~ARM_FLAG_Z) | ((v) ? ARM_FLAG_Z : 0)
#define SET_C(v) cpu->cpsr = (cpu->cpsr & ~ARM_FLAG_C) | ((v) ? ARM_FLAG_C : 0)
#define SET_V(v) cpu->cpsr = (cpu->cpsr & ~ARM_FLAG_V) | ((v) ? ARM_FLAG_V : 0)

#define IS_THUMB() ((cpu->cpsr & ARM_FLAG_T) != 0)

#define READ8(addr)      cpu->read8(cpu->mem_ctx, addr)
#define READ16(addr)     cpu->read16(cpu->mem_ctx, (addr) & ~1u)
#define READ32(addr)     cpu->read32(cpu->mem_ctx, (addr) & ~3u)
#define WRITE8(addr,v)   cpu->write8(cpu->mem_ctx, addr, v)
#define WRITE16(addr,v)  cpu->write16(cpu->mem_ctx, (addr) & ~1u, v)
#define WRITE32(addr,v)  cpu->write32(cpu->mem_ctx, (addr) & ~3u, v)

void arm7_init(ARM7TDMI *cpu) {
    memset(cpu, 0, sizeof(ARM7TDMI));
    cpu->cpsr = ARM_MODE_SVC | ARM_FLAG_I | ARM_FLAG_F;
}

void arm7_reset(ARM7TDMI *cpu) {
    arm7_read8_fn r8 = cpu->read8;
    arm7_read16_fn r16 = cpu->read16;
    arm7_read32_fn r32 = cpu->read32;
    arm7_write8_fn w8 = cpu->write8;
    arm7_write16_fn w16 = cpu->write16;
    arm7_write32_fn w32 = cpu->write32;
    void *ctx = cpu->mem_ctx;

    memset(cpu, 0, sizeof(ARM7TDMI));
    cpu->read8 = r8; cpu->read16 = r16; cpu->read32 = r32;
    cpu->write8 = w8; cpu->write16 = w16; cpu->write32 = w32;
    cpu->mem_ctx = ctx;

    cpu->cpsr = ARM_MODE_SVC | ARM_FLAG_I | ARM_FLAG_F;
    PC = 0x08000000; /* GBA ROM entry point */
    cpu->pipeline_valid = false;
}

/* ---------- Mode switching ---------- */

static uint8_t current_mode(ARM7TDMI *cpu) {
    return cpu->cpsr & ARM_MODE_MASK;
}

void arm7_switch_mode(ARM7TDMI *cpu, uint8_t new_mode) {
    uint8_t old_mode = current_mode(cpu);
    if (old_mode == new_mode) return;

    /* Save current banked registers */
    switch (old_mode) {
        case ARM_MODE_USR:
        case ARM_MODE_SYS:
            cpu->usr_r13 = cpu->r[13];
            cpu->usr_r14 = cpu->r[14];
            break;
        case ARM_MODE_FIQ:
            for (int i = 0; i < 5; i++) cpu->fiq_r8_r12[i] = cpu->r[8 + i];
            cpu->fiq_r13 = cpu->r[13];
            cpu->fiq_r14 = cpu->r[14];
            cpu->fiq_spsr = cpu->spsr;
            break;
        case ARM_MODE_IRQ:
            cpu->irq_r13 = cpu->r[13];
            cpu->irq_r14 = cpu->r[14];
            cpu->irq_spsr = cpu->spsr;
            break;
        case ARM_MODE_SVC:
            cpu->svc_r13 = cpu->r[13];
            cpu->svc_r14 = cpu->r[14];
            cpu->svc_spsr = cpu->spsr;
            break;
        case ARM_MODE_ABT:
            cpu->abt_r13 = cpu->r[13];
            cpu->abt_r14 = cpu->r[14];
            cpu->abt_spsr = cpu->spsr;
            break;
        case ARM_MODE_UND:
            cpu->und_r13 = cpu->r[13];
            cpu->und_r14 = cpu->r[14];
            cpu->und_spsr = cpu->spsr;
            break;
    }

    /* Save/restore R8-R12 for FIQ */
    if (old_mode == ARM_MODE_FIQ && new_mode != ARM_MODE_FIQ) {
        for (int i = 0; i < 5; i++) {
            cpu->fiq_r8_r12[i] = cpu->r[8 + i];
            cpu->r[8 + i] = cpu->usr_r8_r12[i];
        }
    } else if (old_mode != ARM_MODE_FIQ && new_mode == ARM_MODE_FIQ) {
        for (int i = 0; i < 5; i++) {
            cpu->usr_r8_r12[i] = cpu->r[8 + i];
            cpu->r[8 + i] = cpu->fiq_r8_r12[i];
        }
    }

    /* Load new mode's banked registers */
    switch (new_mode) {
        case ARM_MODE_USR:
        case ARM_MODE_SYS:
            cpu->r[13] = cpu->usr_r13;
            cpu->r[14] = cpu->usr_r14;
            break;
        case ARM_MODE_FIQ:
            cpu->r[13] = cpu->fiq_r13;
            cpu->r[14] = cpu->fiq_r14;
            cpu->spsr = cpu->fiq_spsr;
            break;
        case ARM_MODE_IRQ:
            cpu->r[13] = cpu->irq_r13;
            cpu->r[14] = cpu->irq_r14;
            cpu->spsr = cpu->irq_spsr;
            break;
        case ARM_MODE_SVC:
            cpu->r[13] = cpu->svc_r13;
            cpu->r[14] = cpu->svc_r14;
            cpu->spsr = cpu->svc_spsr;
            break;
        case ARM_MODE_ABT:
            cpu->r[13] = cpu->abt_r13;
            cpu->r[14] = cpu->abt_r14;
            cpu->spsr = cpu->abt_spsr;
            break;
        case ARM_MODE_UND:
            cpu->r[13] = cpu->und_r13;
            cpu->r[14] = cpu->und_r14;
            cpu->spsr = cpu->und_spsr;
            break;
    }

    cpu->cpsr = (cpu->cpsr & ~ARM_MODE_MASK) | (new_mode & ARM_MODE_MASK);
}

/* ---------- Interrupts ---------- */

void arm7_request_irq(ARM7TDMI *cpu) {
    cpu->irq_pending = true;
}

void arm7_request_fiq(ARM7TDMI *cpu) {
    cpu->fiq_pending = true;
}

void arm7_check_interrupts(ARM7TDMI *cpu) {
    /* FIQ */
    if (cpu->fiq_pending && !(cpu->cpsr & ARM_FLAG_F)) {
        cpu->fiq_pending = false;
        cpu->halted = false;
        uint32_t return_addr = PC + 4;
        uint32_t old_cpsr = cpu->cpsr;
        arm7_switch_mode(cpu, ARM_MODE_FIQ);
        cpu->spsr = old_cpsr;
        LR = return_addr;
        cpu->cpsr |= ARM_FLAG_I | ARM_FLAG_F;
        cpu->cpsr &= ~ARM_FLAG_T;
        PC = 0x1C;
        cpu->pipeline_valid = false;
    }

    /* IRQ */
    if (cpu->irq_pending && !(cpu->cpsr & ARM_FLAG_I)) {
        cpu->irq_pending = false;
        cpu->halted = false;
        uint32_t return_addr = PC + 4;
        uint32_t old_cpsr = cpu->cpsr;
        arm7_switch_mode(cpu, ARM_MODE_IRQ);
        cpu->spsr = old_cpsr;
        LR = return_addr;
        cpu->cpsr |= ARM_FLAG_I;
        cpu->cpsr &= ~ARM_FLAG_T;
        PC = 0x18;
        cpu->pipeline_valid = false;
    }
}

/* ---------- Condition evaluation ---------- */

static bool check_condition(ARM7TDMI *cpu, uint8_t cond) {
    switch (cond) {
        case 0x0: return GET_Z();                        /* EQ */
        case 0x1: return !GET_Z();                       /* NE */
        case 0x2: return GET_C();                        /* CS/HS */
        case 0x3: return !GET_C();                       /* CC/LO */
        case 0x4: return GET_N();                        /* MI */
        case 0x5: return !GET_N();                       /* PL */
        case 0x6: return GET_V();                        /* VS */
        case 0x7: return !GET_V();                       /* VC */
        case 0x8: return GET_C() && !GET_Z();            /* HI */
        case 0x9: return !GET_C() || GET_Z();            /* LS */
        case 0xA: return GET_N() == GET_V();             /* GE */
        case 0xB: return GET_N() != GET_V();             /* LT */
        case 0xC: return !GET_Z() && (GET_N() == GET_V()); /* GT */
        case 0xD: return GET_Z() || (GET_N() != GET_V());  /* LE */
        case 0xE: return true;                           /* AL */
        case 0xF: return true;                           /* NV (unconditional) */
        default:  return false;
    }
}

/* ---------- Barrel shifter ---------- */

static uint32_t barrel_shift(ARM7TDMI *cpu, uint32_t val, uint8_t type,
                              uint8_t amount, bool *carry_out, bool reg_shift) {
    /* type: 0=LSL, 1=LSR, 2=ASR, 3=ROR */
    if (amount == 0 && !reg_shift) {
        /* Special cases for immediate shift of 0 */
        switch (type) {
            case 0: /* LSL #0: no change */
                *carry_out = GET_C();
                return val;
            case 1: /* LSR #0 -> LSR #32 */
                *carry_out = (val >> 31) & 1;
                return 0;
            case 2: /* ASR #0 -> ASR #32 */
                *carry_out = (val >> 31) & 1;
                return (int32_t)val >> 31;
            case 3: /* ROR #0 -> RRX (rotate right extended) */
                *carry_out = val & 1;
                return (GET_C() << 31) | (val >> 1);
        }
    }

    if (amount == 0) {
        *carry_out = GET_C();
        return val;
    }

    switch (type) {
        case 0: /* LSL */
            if (amount >= 32) {
                *carry_out = (amount == 32) ? (val & 1) : 0;
                return 0;
            }
            *carry_out = (val >> (32 - amount)) & 1;
            return val << amount;

        case 1: /* LSR */
            if (amount >= 32) {
                *carry_out = (amount == 32) ? ((val >> 31) & 1) : 0;
                return 0;
            }
            *carry_out = (val >> (amount - 1)) & 1;
            return val >> amount;

        case 2: /* ASR */
            if (amount >= 32) {
                *carry_out = (val >> 31) & 1;
                return (int32_t)val >> 31;
            }
            *carry_out = (val >> (amount - 1)) & 1;
            return (uint32_t)((int32_t)val >> amount);

        case 3: /* ROR */
            amount &= 31;
            if (amount == 0) {
                *carry_out = (val >> 31) & 1;
                return val;
            }
            *carry_out = (val >> (amount - 1)) & 1;
            return (val >> amount) | (val << (32 - amount));
    }

    *carry_out = GET_C();
    return val;
}

/* ---------- ARM ALU ---------- */

static void set_nz(ARM7TDMI *cpu, uint32_t result) {
    SET_N(result & 0x80000000);
    SET_Z(result == 0);
}

static uint32_t add_with_carry(ARM7TDMI *cpu, uint32_t a, uint32_t b,
                                bool carry_in, bool set_flags) {
    uint64_t result = (uint64_t)a + b + carry_in;
    uint32_t r = (uint32_t)result;
    if (set_flags) {
        set_nz(cpu, r);
        SET_C(result > 0xFFFFFFFF);
        SET_V(((a ^ r) & (b ^ r)) >> 31);
    }
    return r;
}

static uint32_t sub_with_carry(ARM7TDMI *cpu, uint32_t a, uint32_t b,
                                bool carry_in, bool set_flags) {
    return add_with_carry(cpu, a, ~b, carry_in, set_flags);
}

/* ---------- ARM instruction decode/execute ---------- */

static void flush_pipeline(ARM7TDMI *cpu) {
    cpu->pipeline_valid = false;
}

static uint32_t arm_fetch(ARM7TDMI *cpu) {
    uint32_t instr = READ32(PC);
    PC += 4;
    return instr;
}

/* ARM Data Processing */
static uint32_t arm_data_processing(ARM7TDMI *cpu, uint32_t instr) {
    uint8_t opcode = (instr >> 21) & 0xF;
    bool S = (instr >> 20) & 1;
    uint8_t rn = (instr >> 16) & 0xF;
    uint8_t rd = (instr >> 12) & 0xF;
    bool carry_out = GET_C();

    uint32_t op2;
    if (instr & (1 << 25)) {
        /* Immediate: 8-bit value rotated by 2*rot */
        uint8_t imm = instr & 0xFF;
        uint8_t rot = ((instr >> 8) & 0xF) * 2;
        if (rot == 0) {
            op2 = imm;
        } else {
            op2 = (imm >> rot) | (imm << (32 - rot));
        }
        if (rot > 0) carry_out = (op2 >> 31) & 1;
    } else {
        /* Register */
        uint8_t rm = instr & 0xF;
        uint32_t val = cpu->r[rm];
        if (rm == 15) val += 4; /* Pipeline offset */

        uint8_t shift_type = (instr >> 5) & 3;
        uint8_t shift_amount;
        bool reg_shift = (instr >> 4) & 1;

        if (reg_shift) {
            uint8_t rs = (instr >> 8) & 0xF;
            shift_amount = cpu->r[rs] & 0xFF;
        } else {
            shift_amount = (instr >> 7) & 0x1F;
        }
        op2 = barrel_shift(cpu, val, shift_type, shift_amount, &carry_out, reg_shift);
    }

    uint32_t rn_val = cpu->r[rn];
    if (rn == 15) rn_val += 4; /* Pipeline */

    uint32_t result = 0;
    bool write_result = true;

    switch (opcode) {
        case 0x0: /* AND */
            result = rn_val & op2;
            if (S) { set_nz(cpu, result); SET_C(carry_out); }
            break;
        case 0x1: /* EOR */
            result = rn_val ^ op2;
            if (S) { set_nz(cpu, result); SET_C(carry_out); }
            break;
        case 0x2: /* SUB */
            result = sub_with_carry(cpu, rn_val, op2, true, S);
            break;
        case 0x3: /* RSB */
            result = sub_with_carry(cpu, op2, rn_val, true, S);
            break;
        case 0x4: /* ADD */
            result = add_with_carry(cpu, rn_val, op2, false, S);
            break;
        case 0x5: /* ADC */
            result = add_with_carry(cpu, rn_val, op2, GET_C(), S);
            break;
        case 0x6: /* SBC */
            result = sub_with_carry(cpu, rn_val, op2, GET_C(), S);
            break;
        case 0x7: /* RSC */
            result = sub_with_carry(cpu, op2, rn_val, GET_C(), S);
            break;
        case 0x8: /* TST */
            set_nz(cpu, rn_val & op2);
            SET_C(carry_out);
            write_result = false;
            break;
        case 0x9: /* TEQ */
            set_nz(cpu, rn_val ^ op2);
            SET_C(carry_out);
            write_result = false;
            break;
        case 0xA: /* CMP */
            sub_with_carry(cpu, rn_val, op2, true, true);
            write_result = false;
            break;
        case 0xB: /* CMN */
            add_with_carry(cpu, rn_val, op2, false, true);
            write_result = false;
            break;
        case 0xC: /* ORR */
            result = rn_val | op2;
            if (S) { set_nz(cpu, result); SET_C(carry_out); }
            break;
        case 0xD: /* MOV */
            result = op2;
            if (S) { set_nz(cpu, result); SET_C(carry_out); }
            break;
        case 0xE: /* BIC */
            result = rn_val & ~op2;
            if (S) { set_nz(cpu, result); SET_C(carry_out); }
            break;
        case 0xF: /* MVN */
            result = ~op2;
            if (S) { set_nz(cpu, result); SET_C(carry_out); }
            break;
    }

    if (write_result) {
        if (rd == 15) {
            uint32_t new_pc = result;
            if (S) {
                /* SUBS PC, ... / MOVS PC, ... restores CPSR from SPSR */
                uint32_t new_cpsr = cpu->spsr;
                arm7_switch_mode(cpu, new_cpsr & ARM_MODE_MASK);
                cpu->cpsr = new_cpsr;
            }

            new_pc &= IS_THUMB() ? ~1u : ~3u;
            PC = new_pc;
            flush_pipeline(cpu);
        } else {
            cpu->r[rd] = result;
        }
    }

    return 1; /* Simplified cycle count */
}

/* ARM Multiply */
static uint32_t arm_multiply(ARM7TDMI *cpu, uint32_t instr) {
    uint8_t rd = (instr >> 16) & 0xF;
    uint8_t rn = (instr >> 12) & 0xF;
    uint8_t rs = (instr >> 8) & 0xF;
    uint8_t rm = instr & 0xF;
    bool A = (instr >> 21) & 1;
    bool S = (instr >> 20) & 1;

    uint32_t result = cpu->r[rm] * cpu->r[rs];
    if (A) result += cpu->r[rn]; /* MLA */

    cpu->r[rd] = result;
    if (S) set_nz(cpu, result);

    return A ? 3 : 2;
}

/* ARM Multiply Long */
static uint32_t arm_multiply_long(ARM7TDMI *cpu, uint32_t instr) {
    uint8_t rdhi = (instr >> 16) & 0xF;
    uint8_t rdlo = (instr >> 12) & 0xF;
    uint8_t rs = (instr >> 8) & 0xF;
    uint8_t rm = instr & 0xF;
    bool U = (instr >> 22) & 1;  /* 0=unsigned, 1=signed */
    bool A = (instr >> 21) & 1;
    bool S = (instr >> 20) & 1;

    uint64_t result;
    if (U) {
        result = (int64_t)(int32_t)cpu->r[rm] * (int64_t)(int32_t)cpu->r[rs];
    } else {
        result = (uint64_t)cpu->r[rm] * (uint64_t)cpu->r[rs];
    }

    if (A) {
        result += ((uint64_t)cpu->r[rdhi] << 32) | cpu->r[rdlo];
    }

    cpu->r[rdhi] = (uint32_t)(result >> 32);
    cpu->r[rdlo] = (uint32_t)result;

    if (S) {
        SET_N((result >> 63) & 1);
        SET_Z(result == 0);
    }

    return A ? 4 : 3;
}

/* ARM Single Data Transfer (LDR/STR) */
static uint32_t arm_single_transfer(ARM7TDMI *cpu, uint32_t instr) {
    uint8_t rn = (instr >> 16) & 0xF;
    uint8_t rd = (instr >> 12) & 0xF;
    bool I = (instr >> 25) & 1;  /* 0=immediate offset, 1=register */
    bool P = (instr >> 24) & 1;  /* Pre/Post indexing */
    bool U = (instr >> 23) & 1;  /* Up/Down */
    bool B = (instr >> 22) & 1;  /* Byte/Word */
    bool W = (instr >> 21) & 1;  /* Write-back */
    bool L = (instr >> 20) & 1;  /* Load/Store */

    uint32_t base = cpu->r[rn];
    if (rn == 15) base += 4;

    uint32_t offset;
    if (!I) {
        offset = instr & 0xFFF;
    } else {
        uint8_t rm = instr & 0xF;
        uint32_t val = cpu->r[rm];
        uint8_t shift_type = (instr >> 5) & 3;
        uint8_t shift_amount = (instr >> 7) & 0x1F;
        bool dummy;
        offset = barrel_shift(cpu, val, shift_type, shift_amount, &dummy, false);
    }

    uint32_t addr = P ? (U ? base + offset : base - offset) : base;

    if (L) {
        if (B) {
            cpu->r[rd] = READ8(addr);
        } else {
            uint32_t val = READ32(addr);
            /* Unaligned word: rotate into register */
            uint8_t misalign = addr & 3;
            if (misalign) {
                val = (val >> (misalign * 8)) | (val << (32 - misalign * 8));
            }
            cpu->r[rd] = val;
        }
        if (rd == 15) {
            PC &= ~3u;
            flush_pipeline(cpu);
        }
    } else {
        uint32_t val = cpu->r[rd];
        if (rd == 15) val += 4; /* Pipeline */
        if (B) {
            WRITE8(addr, (uint8_t)val);
        } else {
            WRITE32(addr, val);
        }
    }

    /* Post-index or write-back */
    if (!P) {
        uint32_t wb = U ? base + offset : base - offset;
        cpu->r[rn] = wb;
    } else if (W) {
        cpu->r[rn] = addr;
    }

    return L ? 3 : 2;
}

/* ARM Halfword/Signed Data Transfer (LDRH/STRH/LDRSB/LDRSH) */
static uint32_t arm_halfword_transfer(ARM7TDMI *cpu, uint32_t instr) {
    uint8_t rn = (instr >> 16) & 0xF;
    uint8_t rd = (instr >> 12) & 0xF;
    bool P = (instr >> 24) & 1;
    bool U = (instr >> 23) & 1;
    bool I = (instr >> 22) & 1; /* Immediate offset */
    bool W = (instr >> 21) & 1;
    bool L = (instr >> 20) & 1;
    uint8_t sh = (instr >> 5) & 3;

    uint32_t base = cpu->r[rn];
    if (rn == 15) base += 4;

    uint32_t offset;
    if (I) {
        offset = ((instr >> 4) & 0xF0) | (instr & 0xF);
    } else {
        offset = cpu->r[instr & 0xF];
    }

    uint32_t addr = P ? (U ? base + offset : base - offset) : base;

    if (L) {
        switch (sh) {
            case 1: /* LDRH */
                cpu->r[rd] = READ16(addr);
                break;
            case 2: /* LDRSB */
                cpu->r[rd] = (uint32_t)(int32_t)(int8_t)READ8(addr);
                break;
            case 3: /* LDRSH */
                cpu->r[rd] = (uint32_t)(int32_t)(int16_t)READ16(addr);
                break;
        }
        if (rd == 15) {
            PC &= ~3u;
            flush_pipeline(cpu);
        }
    } else {
        if (sh == 1) { /* STRH */
            WRITE16(addr, (uint16_t)cpu->r[rd]);
        }
    }

    if (!P) {
        cpu->r[rn] = U ? base + offset : base - offset;
    } else if (W) {
        cpu->r[rn] = addr;
    }

    return L ? 3 : 2;
}

/* ARM Block Data Transfer (LDM/STM) */
static uint32_t arm_block_transfer(ARM7TDMI *cpu, uint32_t instr) {
    uint8_t rn = (instr >> 16) & 0xF;
    bool P = (instr >> 24) & 1;
    bool U = (instr >> 23) & 1;
    bool S = (instr >> 22) & 1; /* User bank / CPSR restore */
    bool W = (instr >> 21) & 1;
    bool L = (instr >> 20) & 1;
    uint16_t rlist = instr & 0xFFFF;

    uint32_t base = cpu->r[rn];
    int count = 0;
    for (int i = 0; i < 16; i++) {
        if (rlist & (1 << i)) count++;
    }
    if (count == 0) count = 16; /* Empty rlist: add 0x40 */

    uint32_t start_addr;
    if (U) {
        start_addr = P ? base + 4 : base;
    } else {
        start_addr = P ? base - count * 4 : base - count * 4 + 4;
    }

    /* Determine if we need user-bank access (S=1 and NOT LDM with R15) */
    bool user_bank = S && !(L && (rlist & (1 << 15)));
    uint8_t cur_mode = current_mode(cpu);
    bool is_priv = cur_mode != ARM_MODE_USR && cur_mode != ARM_MODE_SYS;

    uint32_t addr = start_addr;
    for (int i = 0; i < 16; i++) {
        if (!(rlist & (1 << i))) continue;

        if (user_bank && is_priv && i >= 8 && i <= 14) {
            /* Access user-mode banked registers */
            if (L) {
                uint32_t val = READ32(addr);
                if (i >= 8 && i <= 12) {
                    if (cur_mode == ARM_MODE_FIQ)
                        cpu->usr_r8_r12[i - 8] = val;
                    else
                        cpu->r[i] = val;
                } else if (i == 13) {
                    if (is_priv) cpu->usr_r13 = val; else cpu->r[13] = val;
                } else if (i == 14) {
                    if (is_priv) cpu->usr_r14 = val; else cpu->r[14] = val;
                }
            } else {
                uint32_t val;
                if (i >= 8 && i <= 12) {
                    val = (cur_mode == ARM_MODE_FIQ) ? cpu->usr_r8_r12[i - 8] : cpu->r[i];
                } else if (i == 13) {
                    val = is_priv ? cpu->usr_r13 : cpu->r[13];
                } else { /* i == 14 */
                    val = is_priv ? cpu->usr_r14 : cpu->r[14];
                }
                WRITE32(addr, val);
            }
        } else {
            /* Normal register access */
            if (L) {
                cpu->r[i] = READ32(addr);
                if (i == 15) {
                    /* S=1 with R15 in LDM: restore CPSR from SPSR */
                    if (S && is_priv) {
                        uint32_t new_cpsr = cpu->spsr;
                        arm7_switch_mode(cpu, new_cpsr & ARM_MODE_MASK);
                        cpu->cpsr = new_cpsr;
                    }
                    PC &= IS_THUMB() ? ~1u : ~3u;
                    flush_pipeline(cpu);
                }
            } else {
                uint32_t val = cpu->r[i];
                if (i == 15) val += 4;
                WRITE32(addr, val);
            }
        }
        addr += 4;
    }

    /* Writeback (not performed when S=1 user-bank and W=1 in privileged mode,
       but many emulators just do it regardless — the ARM ARM says "unpredictable") */
    if (W && !user_bank) {
        cpu->r[rn] = U ? base + count * 4 : base - count * 4;
    }

    return count + (L ? 2 : 1);
}

/* ARM Branch / Branch with Link */
static uint32_t arm_branch(ARM7TDMI *cpu, uint32_t instr) {
    bool L = (instr >> 24) & 1;
    int32_t offset = (int32_t)(instr << 8) >> 6; /* Sign-extend 24-bit, *4 */

    if (L) {
        LR = PC; /* Return address = next instruction (PC is already inst+4) */
    }

    PC += offset + 4; /* +4 for pipeline: real PC = inst+8, ours = inst+4 */
    flush_pipeline(cpu);
    return 3;
}

/* ARM Branch and Exchange (BX) */
static uint32_t arm_branch_exchange(ARM7TDMI *cpu, uint32_t instr) {
    uint8_t rn = instr & 0xF;
    uint32_t addr = cpu->r[rn];

    if (addr & 1) {
        cpu->cpsr |= ARM_FLAG_T;  /* Switch to Thumb */
        PC = addr & ~1u;
    } else {
        cpu->cpsr &= ~ARM_FLAG_T; /* Stay in ARM */
        PC = addr & ~3u;
    }
    flush_pipeline(cpu);
    return 3;
}

/* ---------- HLE BIOS SWI ---------- */

/*
 * High-Level Emulation of GBA BIOS software interrupts.
 * Instead of jumping to a real BIOS ROM at 0x08, we handle common
 * SWI functions directly in C.  The SWI number is extracted by the
 * caller: bits [23:16] for ARM, bits [7:0] for Thumb.
 *
 * After execution, return to the address in LR_svc (already saved by
 * the ARM/Thumb SWI entry code) and restore CPSR from SPSR_svc.
 */

static void swi_return(ARM7TDMI *cpu) {
    /* Restore CPSR from SPSR, return to LR */
    uint32_t ret = LR;
    uint32_t saved_cpsr = cpu->spsr;
    arm7_switch_mode(cpu, saved_cpsr & ARM_MODE_MASK);
    cpu->cpsr = saved_cpsr;
    PC = ret;
    flush_pipeline(cpu);
}

/* Trace state for post-VBL execution logging */
static int vbl_swi_done = 0;      /* incremented in hle_swi after SWI 0x05 */
static int post_vbl_traced = 0;   /* instructions traced after 3rd VBL wake */

static void hle_swi(ARM7TDMI *cpu, uint8_t num) {
    static int swi_trace = 0;
    static uint64_t last_vblank_cycles = 0;
    bool do_return = true;
    if (swi_trace < 30) {
        fprintf(stderr, "[SWI] num=0x%02X R0=0x%08X R1=0x%08X R2=0x%08X PC=0x%08X LR=0x%08X\n",
            num, cpu->r[0], cpu->r[1], cpu->r[2], PC, LR);
        swi_trace++;
    }
    if (num == 0x05 && last_vblank_cycles > 0) {
        static int vbl_count = 0;
        uint64_t delta = cpu->total_cycles - last_vblank_cycles;
        if (vbl_count < 10) {
            fprintf(stderr, "[VBL] cycles_between=%llu\n", (unsigned long long)delta);
            vbl_count++;
        }
    }
    if (num == 0x05) last_vblank_cycles = cpu->total_cycles;
    switch (num) {
        case 0x00: { /* SoftReset */
            /* Clear 0x03007E00-0x03007FFF, reset stack pointers, jump to ROM */
            for (uint32_t a = 0x03007E00; a < 0x03008000; a++)
                WRITE8(a, 0);
            cpu->svc_r13 = 0x03007FE0;
            cpu->irq_r13 = 0x03007FA0;
            arm7_switch_mode(cpu, ARM_MODE_SYS);
            cpu->r[13] = 0x03007F00;
            cpu->cpsr = ARM_MODE_SYS;
            PC = 0x08000000;
            flush_pipeline(cpu);
            return; /* Don't do normal swi_return */
        }

        case 0x01: { /* RegisterRamReset (flags in R0) */
            uint8_t flags = cpu->r[0] & 0xFF;
            GbaMemory *rst_mem = (GbaMemory *)cpu->mem_ctx;
            if (flags & 0x01) memset(rst_mem->ewram, 0, sizeof(rst_mem->ewram));
            if (flags & 0x02) memset(rst_mem->iwram, 0, 0x7E00); /* Keep last 0x200 for IRQ stack */
            if (flags & 0x04) memset(rst_mem->palette, 0, sizeof(rst_mem->palette));
            if (flags & 0x08) memset(rst_mem->vram, 0, sizeof(rst_mem->vram));
            if (flags & 0x10) memset(rst_mem->oam, 0, sizeof(rst_mem->oam));
            if (flags & 0x20) { /* Reset SIO: clear 0x04000120-0x0400015A */ }
            if (flags & 0x40) {
                /* Reset Sound registers */
                for (uint32_t off = 0x060; off < 0x0A8; off += 2)
                    *(uint16_t *)&rst_mem->io[off] = 0;
            }
            if (flags & 0x80) {
                /* Reset other IO registers */
                for (uint32_t off = 0; off < 0x060; off += 2)
                    *(uint16_t *)&rst_mem->io[off] = 0;
            }
            break;
        }

        case 0x02: /* Halt */
            cpu->halted = true;
            break;

        case 0x03: /* Stop */
            cpu->halted = true;
            break;

        case 0x04: { /* IntrWait (R0=discard_old, R1=flags) */
            uint16_t wait_flags = cpu->r[1] & 0xFFFF;
            if (cpu->r[0]) {
                /* Discard old: clear requested flags in BIOS IF (0x03007FF8) */
                uint16_t bios_if = READ16(0x03007FF8);
                bios_if &= ~wait_flags;
                WRITE16(0x03007FF8, bios_if);
            }

            /* Force IME=1 as per GBATEK */
            WRITE16(0x04000208, 1);

            /* BIOS blocks inside SWI until requested IRQ flags are latched. */
            uint16_t bios_if = READ16(0x03007FF8);
            if ((bios_if & wait_flags) == 0) {
                cpu->intrwait_active = true;
                cpu->intrwait_flags = wait_flags;
                cpu->cpsr &= ~ARM_FLAG_I;
                cpu->halted = true;
                do_return = false;
            }
            break;
        }

        case 0x05: { /* VBlankIntrWait — equivalent to IntrWait(1, 1) */
            uint16_t bios_if = READ16(0x03007FF8);
            bios_if &= ~1u;
            WRITE16(0x03007FF8, bios_if);

            /* Force IME=1 as per GBATEK */
            WRITE16(0x04000208, 1);

            cpu->intrwait_active = true;
            cpu->intrwait_flags = 1;
            cpu->cpsr &= ~ARM_FLAG_I;
            cpu->halted = true;
            do_return = false;
            vbl_swi_done++;
            break;
        }

        case 0x06: { /* Div: R0=num, R1=den -> R0=quo, R1=rem, R3=abs(quo) */
            int32_t num = (int32_t)cpu->r[0];
            int32_t den = (int32_t)cpu->r[1];
            if (den == 0) { break; }
            cpu->r[0] = (uint32_t)(num / den);
            cpu->r[1] = (uint32_t)(num % den);
            cpu->r[3] = (uint32_t)(num / den < 0 ? -(num / den) : num / den);
            break;
        }

        case 0x07: { /* DivArm: R0=den, R1=num (swapped args) */
            int32_t num = (int32_t)cpu->r[1];
            int32_t den = (int32_t)cpu->r[0];
            if (den == 0) { break; }
            cpu->r[0] = (uint32_t)(num / den);
            cpu->r[1] = (uint32_t)(num % den);
            cpu->r[3] = (uint32_t)(num / den < 0 ? -(num / den) : num / den);
            break;
        }

        case 0x08: { /* Sqrt: R0=val -> R0=sqrt(val) */
            uint32_t val = cpu->r[0];
            uint32_t result = 0;
            uint32_t bit = 1u << 30;
            while (bit > val) bit >>= 2;
            while (bit != 0) {
                if (val >= result + bit) {
                    val -= result + bit;
                    result = (result >> 1) + bit;
                } else {
                    result >>= 1;
                }
                bit >>= 2;
            }
            cpu->r[0] = result;
            break;
        }

        case 0x09: { /* ArcTan: R0=tan (s16.14) -> R0=angle (0..pi/2 as u16) */
            /* Simplified: use floating point approximation */
            int16_t t = (int16_t)(cpu->r[0] & 0xFFFF);
            double tan_val = t / 16384.0;
            double angle = atan(tan_val);
            int16_t result = (int16_t)(angle * (32768.0 / 3.14159265358979));
            cpu->r[0] = (uint32_t)(uint16_t)result;
            break;
        }

        case 0x0A: { /* ArcTan2: R0=x, R1=y -> R0=angle (0..2*pi as u16) */
            int16_t x = (int16_t)(cpu->r[0] & 0xFFFF);
            int16_t y = (int16_t)(cpu->r[1] & 0xFFFF);
            double angle = atan2((double)y, (double)x);
            if (angle < 0) angle += 2.0 * 3.14159265358979;
            uint16_t result = (uint16_t)(angle * (32768.0 / 3.14159265358979));
            cpu->r[0] = result;
            break;
        }

        case 0x0B: { /* CpuSet: R0=src, R1=dst, R2=len_mode */
            uint32_t src = cpu->r[0];
            uint32_t dst = cpu->r[1];
            uint32_t ctrl = cpu->r[2];
            uint32_t count = ctrl & 0x1FFFFF;
            bool fixed = (ctrl >> 24) & 1;
            bool word = (ctrl >> 26) & 1;

            if (word) {
                src &= ~3u; dst &= ~3u;
                uint32_t fill = READ32(src);
                for (uint32_t i = 0; i < count; i++) {
                    WRITE32(dst, fixed ? fill : READ32(src));
                    dst += 4;
                    if (!fixed) src += 4;
                }
            } else {
                src &= ~1u; dst &= ~1u;
                uint16_t fill = READ16(src);
                for (uint32_t i = 0; i < count; i++) {
                    WRITE16(dst, fixed ? fill : READ16(src));
                    dst += 2;
                    if (!fixed) src += 2;
                }
            }
            break;
        }

        case 0x0C: { /* CpuFastSet: R0=src, R1=dst, R2=len_mode (words, 32-byte aligned) */
            uint32_t src = cpu->r[0] & ~3u;
            uint32_t dst = cpu->r[1] & ~3u;
            uint32_t ctrl = cpu->r[2];
            uint32_t count = ctrl & 0x1FFFFF;
            bool fixed = (ctrl >> 24) & 1;

            /* Round up to multiple of 8 words */
            count = (count + 7) & ~7u;
            uint32_t fill = READ32(src);
            for (uint32_t i = 0; i < count; i++) {
                WRITE32(dst, fixed ? fill : READ32(src));
                dst += 4;
                if (!fixed) src += 4;
            }
            break;
        }

        case 0x0E: { /* BgAffineSet */
            uint32_t src = cpu->r[0];
            uint32_t dst = cpu->r[1];
            uint32_t count = cpu->r[2];

            for (uint32_t i = 0; i < count; i++) {
                int32_t cx = (int32_t)READ32(src);      src += 4;
                int32_t cy = (int32_t)READ32(src);      src += 4;
                int16_t dx = (int16_t)READ16(src);      src += 2;
                int16_t dy = (int16_t)READ16(src);      src += 2;
                int16_t sx = (int16_t)READ16(src);      src += 2;
                int16_t sy = (int16_t)READ16(src);      src += 2;
                uint16_t angle = READ16(src);            src += 2;
                src += 2; /* padding */

                double a = (double)angle / 65536.0 * 2.0 * 3.14159265358979;
                double cos_a = cos(a), sin_a = sin(a);
                int16_t pa = (int16_t)( cos_a * (256.0 / sx * 256.0));
                int16_t pb = (int16_t)(-sin_a * (256.0 / sx * 256.0));
                int16_t pc = (int16_t)( sin_a * (256.0 / sy * 256.0));
                int16_t pd = (int16_t)( cos_a * (256.0 / sy * 256.0));
                int32_t refx = cx - (int32_t)((int64_t)pa * dx + (int64_t)pb * dy);
                int32_t refy = cy - (int32_t)((int64_t)pc * dx + (int64_t)pd * dy);

                WRITE16(dst, (uint16_t)pa); dst += 2;
                WRITE16(dst, (uint16_t)pb); dst += 2;
                WRITE16(dst, (uint16_t)pc); dst += 2;
                WRITE16(dst, (uint16_t)pd); dst += 2;
                WRITE32(dst, (uint32_t)refx); dst += 4;
                WRITE32(dst, (uint32_t)refy); dst += 4;
            }
            break;
        }

        case 0x0F: { /* ObjAffineSet */
            uint32_t src = cpu->r[0];
            uint32_t dst = cpu->r[1];
            uint32_t count = cpu->r[2];
            uint32_t offset = cpu->r[3]; /* stride in dst (typically 2 or 8) */

            for (uint32_t i = 0; i < count; i++) {
                int16_t sx = (int16_t)READ16(src); src += 2;
                int16_t sy = (int16_t)READ16(src); src += 2;
                uint16_t angle = READ16(src);       src += 2;
                src += 2; /* padding */

                double a = (double)angle / 65536.0 * 2.0 * 3.14159265358979;
                double cos_a = cos(a), sin_a = sin(a);
                int16_t pa = (int16_t)( cos_a * (256.0 * 256.0 / sx));
                int16_t pb = (int16_t)(-sin_a * (256.0 * 256.0 / sx));
                int16_t pc = (int16_t)( sin_a * (256.0 * 256.0 / sy));
                int16_t pd = (int16_t)( cos_a * (256.0 * 256.0 / sy));

                WRITE16(dst, (uint16_t)pa); dst += offset;
                WRITE16(dst, (uint16_t)pb); dst += offset;
                WRITE16(dst, (uint16_t)pc); dst += offset;
                WRITE16(dst, (uint16_t)pd); dst += offset;
            }
            break;
        }

        case 0x10: { /* BitUnPack: R0=src, R1=dst, R2=info_ptr */
            uint32_t src = cpu->r[0];
            uint32_t dst = cpu->r[1];
            uint32_t info = cpu->r[2];
            uint16_t src_len = READ16(info);
            uint8_t src_width = READ8(info + 2);
            uint8_t dst_width = READ8(info + 3);
            uint32_t data_offset = READ32(info + 4);
            bool zero_flag = (data_offset >> 31) & 1;
            data_offset &= 0x7FFFFFFF;

            if (src_width == 0 || dst_width == 0 || dst_width > 32) break;

            uint32_t out_word = 0;
            int out_bits = 0;
            uint32_t src_mask = (1u << src_width) - 1;

            uint32_t src_byte = 0;
            int src_bits_left = 0;

            for (uint16_t i = 0; i < src_len; ) {
                if (src_bits_left == 0) {
                    src_byte = READ8(src + i);
                    i++;
                    src_bits_left = 8;
                }
                uint32_t val = src_byte & src_mask;
                src_byte >>= src_width;
                src_bits_left -= src_width;

                /* Apply offset: if zero_flag, add to all; else add only to non-zero */
                if (val != 0 || zero_flag) {
                    val += data_offset;
                }

                out_word |= (val & ((1u << dst_width) - 1)) << out_bits;
                out_bits += dst_width;
                if (out_bits >= 32) {
                    WRITE32(dst, out_word);
                    dst += 4;
                    out_word = 0;
                    out_bits = 0;
                }
            }
            /* Flush remaining bits */
            if (out_bits > 0) {
                WRITE32(dst, out_word);
            }
            break;
        }

        case 0x11: { /* LZ77UnCompWram */
            uint32_t src = cpu->r[0];
            uint32_t dst = cpu->r[1];
            uint32_t header = READ32(src); src += 4;
            uint32_t decomp_len = header >> 8;
            uint32_t written = 0;

            while (written < decomp_len) {
                uint8_t flags = READ8(src++);
                for (int bit = 7; bit >= 0 && written < decomp_len; bit--) {
                    if (flags & (1 << bit)) {
                        uint8_t b1 = READ8(src++);
                        uint8_t b2 = READ8(src++);
                        uint32_t len = ((b1 >> 4) & 0xF) + 3;
                        uint32_t disp = ((b1 & 0xF) << 8) | b2;
                        uint32_t ref = dst - disp - 1;
                        for (uint32_t j = 0; j < len && written < decomp_len; j++, written++) {
                            WRITE8(dst++, READ8(ref + j));
                        }
                    } else {
                        WRITE8(dst++, READ8(src++));
                        written++;
                    }
                }
            }
            break;
        }

        case 0x12: { /* LZ77UnCompVram — must write 16-bit to VRAM */
            uint32_t src = cpu->r[0];
            uint32_t dst = cpu->r[1];
            uint32_t header = READ32(src); src += 4;
            uint32_t decomp_len = header >> 8;
            if (decomp_len == 0 || decomp_len > 0x40000) break; /* sanity */
            uint8_t *tmp = (uint8_t *)malloc(decomp_len);
            if (!tmp) break;
            uint32_t written = 0;

            while (written < decomp_len) {
                uint8_t flags = READ8(src++);
                for (int bit = 7; bit >= 0 && written < decomp_len; bit--) {
                    if (flags & (1 << bit)) {
                        uint8_t b1 = READ8(src++);
                        uint8_t b2 = READ8(src++);
                        uint32_t len = ((b1 >> 4) & 0xF) + 3;
                        uint32_t disp = ((b1 & 0xF) << 8) | b2;
                        for (uint32_t j = 0; j < len && written < decomp_len; j++, written++) {
                            tmp[written] = tmp[written - disp - 1];
                        }
                    } else {
                        tmp[written++] = READ8(src++);
                    }
                }
            }
            /* Write to VRAM in 16-bit units */
            for (uint32_t i = 0; i + 1 < decomp_len; i += 2) {
                WRITE16(dst + i, tmp[i] | ((uint16_t)tmp[i + 1] << 8));
            }
            if (decomp_len & 1) {
                WRITE16(dst + decomp_len - 1, tmp[decomp_len - 1]);
            }
            free(tmp);
            break;
        }

        case 0x14: { /* RLUnCompWram */
            uint32_t src = cpu->r[0];
            uint32_t dst = cpu->r[1];
            uint32_t header = READ32(src); src += 4;
            uint32_t decomp_len = header >> 8;
            uint32_t written = 0;

            while (written < decomp_len) {
                uint8_t flag = READ8(src++);
                if (flag & 0x80) {
                    uint32_t len = (flag & 0x7F) + 3;
                    uint8_t data = READ8(src++);
                    for (uint32_t j = 0; j < len && written < decomp_len; j++, written++)
                        WRITE8(dst++, data);
                } else {
                    uint32_t len = (flag & 0x7F) + 1;
                    for (uint32_t j = 0; j < len && written < decomp_len; j++, written++)
                        WRITE8(dst++, READ8(src++));
                }
            }
            break;
        }

        case 0x15: { /* RLUnCompVram — must write 16-bit to VRAM */
            uint32_t src = cpu->r[0];
            uint32_t dst = cpu->r[1];
            uint32_t header = READ32(src); src += 4;
            uint32_t decomp_len = header >> 8;
            if (decomp_len == 0 || decomp_len > 0x40000) break;
            uint8_t *tmp = (uint8_t *)malloc(decomp_len);
            if (!tmp) break;
            uint32_t written = 0;

            while (written < decomp_len) {
                uint8_t flag = READ8(src++);
                if (flag & 0x80) {
                    uint32_t len = (flag & 0x7F) + 3;
                    uint8_t data = READ8(src++);
                    for (uint32_t j = 0; j < len && written < decomp_len; j++, written++)
                        tmp[written] = data;
                } else {
                    uint32_t len = (flag & 0x7F) + 1;
                    for (uint32_t j = 0; j < len && written < decomp_len; j++, written++)
                        tmp[written] = READ8(src++);
                }
            }
            /* Write to VRAM in 16-bit units */
            for (uint32_t i = 0; i + 1 < decomp_len; i += 2) {
                WRITE16(dst + i, tmp[i] | ((uint16_t)tmp[i + 1] << 8));
            }
            if (decomp_len & 1) {
                WRITE16(dst + decomp_len - 1, tmp[decomp_len - 1]);
            }
            free(tmp);
            break;
        }

        default:
            /* Unimplemented SWI: silently return */
            break;
    }

    if (do_return) {
        swi_return(cpu);
    }
}

/* ARM Software Interrupt */
static uint32_t arm_swi(ARM7TDMI *cpu, uint32_t instr) {
    uint8_t swi_num = (instr >> 16) & 0xFF;
    uint32_t return_addr = PC; /* Next instruction (PC = inst+4 after fetch) */
    uint32_t old_cpsr = cpu->cpsr;
    arm7_switch_mode(cpu, ARM_MODE_SVC);
    cpu->spsr = old_cpsr;
    LR = return_addr;
    cpu->cpsr |= ARM_FLAG_I;
    cpu->cpsr &= ~ARM_FLAG_T;
    hle_swi(cpu, swi_num);
    return 3;
}

/* ARM MRS (Read PSR) */
static uint32_t arm_mrs(ARM7TDMI *cpu, uint32_t instr) {
    uint8_t rd = (instr >> 12) & 0xF;
    bool spsr = (instr >> 22) & 1;
    cpu->r[rd] = spsr ? cpu->spsr : cpu->cpsr;
    return 1;
}

/* ARM MSR (Write PSR) */
static uint32_t arm_msr(ARM7TDMI *cpu, uint32_t instr) {
    bool spsr = (instr >> 22) & 1;
    bool I = (instr >> 25) & 1;

    uint32_t val;
    if (I) {
        uint8_t imm = instr & 0xFF;
        uint8_t rot = ((instr >> 8) & 0xF) * 2;
        if (rot == 0) {
            val = imm;
        } else {
            val = (imm >> rot) | (imm << (32 - rot));
        }
    } else {
        val = cpu->r[instr & 0xF];
    }

    /* Field mask: f=flags, s=status, x=extension, c=control */
    uint32_t mask = 0;
    if (instr & (1 << 19)) mask |= 0xFF000000; /* f */
    if (instr & (1 << 18)) mask |= 0x00FF0000; /* s */
    if (instr & (1 << 17)) mask |= 0x0000FF00; /* x */
    if (instr & (1 << 16)) mask |= 0x000000FF; /* c */

    /* User mode can only modify flags */
    if (current_mode(cpu) == ARM_MODE_USR) {
        mask &= 0xFF000000;
    }

    if (spsr) {
        cpu->spsr = (cpu->spsr & ~mask) | (val & mask);
    } else {
        uint32_t old_cpsr = cpu->cpsr;
        uint32_t new_cpsr = (cpu->cpsr & ~mask) | (val & mask);
        if ((old_cpsr & ARM_MODE_MASK) != (new_cpsr & ARM_MODE_MASK)) {
            arm7_switch_mode(cpu, new_cpsr & ARM_MODE_MASK);
        }
        cpu->cpsr = new_cpsr;
    }

    return 1;
}

/* ARM Single Data Swap (SWP) */
static uint32_t arm_swap(ARM7TDMI *cpu, uint32_t instr) {
    uint8_t rn = (instr >> 16) & 0xF;
    uint8_t rd = (instr >> 12) & 0xF;
    uint8_t rm = instr & 0xF;
    bool B = (instr >> 22) & 1;

    uint32_t addr = cpu->r[rn];

    if (B) {
        uint8_t tmp = READ8(addr);
        WRITE8(addr, (uint8_t)cpu->r[rm]);
        cpu->r[rd] = tmp;
    } else {
        uint32_t tmp = READ32(addr);
        WRITE32(addr, cpu->r[rm]);
        cpu->r[rd] = tmp;
    }

    return 4;
}

/* ARM BLX (Branch Link Exchange - ARMv5 but used by GBA BIOS) */
static uint32_t arm_blx(ARM7TDMI *cpu, uint32_t instr) {
    int32_t offset = (int32_t)(instr << 8) >> 6;
    uint32_t h = ((instr >> 24) & 1) << 1;
    LR = PC; /* Return address = next instruction */
    PC += offset + h + 4; /* +4 for pipeline offset */
    cpu->cpsr |= ARM_FLAG_T;
    flush_pipeline(cpu);
    return 3;
}

/* ---------- Execute one ARM instruction ---------- */

static uint32_t arm_execute(ARM7TDMI *cpu) {
    uint32_t instr = arm_fetch(cpu);
    uint8_t cond = (instr >> 28) & 0xF;

    /* Special: cond=0xF is unconditional for BLX */
    if (cond == 0xF) {
        return arm_blx(cpu, instr);
    }

    if (!check_condition(cpu, cond)) {
        return 1; /* Skipped */
    }

    /* Decode by bits 27-20 and 7-4 */
    uint8_t bits27_20 = (instr >> 20) & 0xFF;
    uint8_t bits7_4 = (instr >> 4) & 0xF;

    /* Branch */
    if ((bits27_20 & 0xE0) == 0xA0) {
        return arm_branch(cpu, instr);
    }

    /* Branch and Exchange */
    if ((instr & 0x0FFFFFF0) == 0x012FFF10) {
        return arm_branch_exchange(cpu, instr);
    }

    /* Software Interrupt */
    if ((bits27_20 & 0xF0) == 0xF0) {
        return arm_swi(cpu, instr);
    }

    /* MRS */
    if ((instr & 0x0FBF0FFF) == 0x010F0000) {
        return arm_mrs(cpu, instr);
    }

    /* MSR */
    if ((instr & 0x0DB0F000) == 0x0120F000) {
        return arm_msr(cpu, instr);
    }

    /* SWP/SWPB */
    if ((instr & 0x0FB00FF0) == 0x01000090) {
        return arm_swap(cpu, instr);
    }

    /* Multiply Long */
    if ((instr & 0x0F8000F0) == 0x00800090) {
        return arm_multiply_long(cpu, instr);
    }

    /* Multiply */
    if ((instr & 0x0FC000F0) == 0x00000090) {
        return arm_multiply(cpu, instr);
    }

    /* Halfword/Signed transfer */
    if ((bits7_4 & 0x9) == 0x9 && (bits27_20 & 0xE0) == 0x00
        && ((instr >> 5) & 3) != 0) {
        return arm_halfword_transfer(cpu, instr);
    }

    /* Block Data Transfer */
    if ((bits27_20 & 0xE0) == 0x80) {
        return arm_block_transfer(cpu, instr);
    }

    /* Single Data Transfer */
    if ((bits27_20 & 0xC0) == 0x40) {
        return arm_single_transfer(cpu, instr);
    }

    /* Data Processing */
    if ((bits27_20 & 0xC0) == 0x00) {
        return arm_data_processing(cpu, instr);
    }

    /* Coprocessor / undefined: treat as NOP */
    return 1;
}

/* ---------- Thumb instruction decode/execute ---------- */

static uint16_t thumb_fetch(ARM7TDMI *cpu) {
    uint16_t instr = READ16(PC);
    PC += 2;
    return instr;
}

static uint32_t thumb_execute(ARM7TDMI *cpu) {
    uint16_t instr = thumb_fetch(cpu);
    uint8_t ophi = (instr >> 8) & 0xFF;

    /* Format 1: Move shifted register (op 0-2 only; op=3 is Format 2) */
    if ((instr >> 13) == 0 && ((instr >> 11) & 3) < 3) {
        uint8_t op = (instr >> 11) & 3;
        uint8_t offset = (instr >> 6) & 0x1F;
        uint8_t rs = (instr >> 3) & 7;
        uint8_t rd = instr & 7;
        bool carry = GET_C();
        uint32_t val = cpu->r[rs];

        switch (op) {
            case 0: /* LSL */
                if (offset > 0) carry = (val >> (32 - offset)) & 1;
                val = offset ? val << offset : val;
                break;
            case 1: /* LSR */
                if (offset == 0) offset = 32;
                carry = (val >> (offset - 1)) & 1;
                val = (offset >= 32) ? 0 : val >> offset;
                break;
            case 2: /* ASR */
                if (offset == 0) offset = 32;
                carry = (val >> ((offset > 31 ? 31 : offset) - 1)) & 1;
                val = (offset >= 32) ? ((int32_t)val >> 31) : (uint32_t)((int32_t)val >> offset);
                break;
        }

        cpu->r[rd] = val;
        set_nz(cpu, val);
        SET_C(carry);
        return 1;
    }

    /* Format 2: Add/Subtract */
    if ((instr >> 11) == 0x03) {
        uint8_t op = (instr >> 9) & 3;
        uint8_t rn_or_imm = (instr >> 6) & 7;
        uint8_t rs = (instr >> 3) & 7;
        uint8_t rd = instr & 7;

        uint32_t a = cpu->r[rs];
        uint32_t b = (op & 2) ? rn_or_imm : cpu->r[rn_or_imm];

        if (!(op & 1)) {
            cpu->r[rd] = add_with_carry(cpu, a, b, false, true);
        } else {
            cpu->r[rd] = sub_with_carry(cpu, a, b, true, true);
        }
        return 1;
    }

    /* Format 3: Move/Compare/Add/Sub immediate */
    if ((instr >> 13) == 1) {
        uint8_t op = (instr >> 11) & 3;
        uint8_t rd = (instr >> 8) & 7;
        uint8_t imm = instr & 0xFF;

        switch (op) {
            case 0: /* MOV */
                cpu->r[rd] = imm;
                set_nz(cpu, imm);
                break;
            case 1: /* CMP */
                sub_with_carry(cpu, cpu->r[rd], imm, true, true);
                break;
            case 2: /* ADD */
                cpu->r[rd] = add_with_carry(cpu, cpu->r[rd], imm, false, true);
                break;
            case 3: /* SUB */
                cpu->r[rd] = sub_with_carry(cpu, cpu->r[rd], imm, true, true);
                break;
        }
        return 1;
    }

    /* Format 4: ALU operations */
    if ((instr >> 10) == 0x10) {
        uint8_t op = (instr >> 6) & 0xF;
        uint8_t rs = (instr >> 3) & 7;
        uint8_t rd = instr & 7;
        uint32_t a = cpu->r[rd];
        uint32_t b = cpu->r[rs];
        uint32_t result;

        switch (op) {
            case 0x0: result = a & b; set_nz(cpu, result); cpu->r[rd] = result; break;
            case 0x1: result = a ^ b; set_nz(cpu, result); cpu->r[rd] = result; break;
            case 0x2: { /* LSL */
                bool c = GET_C();
                result = barrel_shift(cpu, a, 0, b & 0xFF, &c, true);
                set_nz(cpu, result); SET_C(c); cpu->r[rd] = result; break;
            }
            case 0x3: { /* LSR */
                bool c = GET_C();
                result = barrel_shift(cpu, a, 1, b & 0xFF, &c, true);
                set_nz(cpu, result); SET_C(c); cpu->r[rd] = result; break;
            }
            case 0x4: { /* ASR */
                bool c = GET_C();
                result = barrel_shift(cpu, a, 2, b & 0xFF, &c, true);
                set_nz(cpu, result); SET_C(c); cpu->r[rd] = result; break;
            }
            case 0x5: cpu->r[rd] = add_with_carry(cpu, a, b, GET_C(), true); break;  /* ADC */
            case 0x6: cpu->r[rd] = sub_with_carry(cpu, a, b, GET_C(), true); break;  /* SBC */
            case 0x7: { /* ROR */
                bool c = GET_C();
                result = barrel_shift(cpu, a, 3, b & 0xFF, &c, true);
                set_nz(cpu, result); SET_C(c); cpu->r[rd] = result; break;
            }
            case 0x8: set_nz(cpu, a & b); break;  /* TST */
            case 0x9: cpu->r[rd] = sub_with_carry(cpu, 0, b, true, true); break;  /* NEG */
            case 0xA: sub_with_carry(cpu, a, b, true, true); break;  /* CMP */
            case 0xB: add_with_carry(cpu, a, b, false, true); break; /* CMN */
            case 0xC: result = a | b; set_nz(cpu, result); cpu->r[rd] = result; break;
            case 0xD: result = a * b; set_nz(cpu, result); cpu->r[rd] = result; break; /* MUL */
            case 0xE: result = a & ~b; set_nz(cpu, result); cpu->r[rd] = result; break; /* BIC */
            case 0xF: result = ~b; set_nz(cpu, result); cpu->r[rd] = result; break; /* MVN */
        }
        return 1;
    }

    /* Format 5: Hi register operations / BX */
    if ((instr >> 10) == 0x11) {
        uint8_t op = (instr >> 8) & 3;
        uint8_t rs = ((instr >> 3) & 0xF) | ((instr >> 3) & 0x8 ? 0 : 0);
        uint8_t rd = (instr & 7) | ((instr & 0x80) ? 8 : 0);
        rs = ((instr >> 3) & 7) | ((instr & 0x40) ? 8 : 0);

        uint32_t rs_val = cpu->r[rs];
        if (rs == 15) rs_val += 2; /* Pipeline: real PC = inst+4, ours = inst+2 */
        uint32_t rd_val = cpu->r[rd];
        if (rd == 15) rd_val += 2;

        switch (op) {
            case 0: /* ADD */
                cpu->r[rd] = rd_val + rs_val;
                if (rd == 15) flush_pipeline(cpu);
                break;
            case 1: /* CMP */
                sub_with_carry(cpu, rd_val, rs_val, true, true);
                break;
            case 2: /* MOV */
                cpu->r[rd] = rs_val;
                if (rd == 15) flush_pipeline(cpu);
                break;
            case 3: { /* BX */
                uint32_t addr = rs_val;
                if (addr & 1) {
                    cpu->cpsr |= ARM_FLAG_T;
                    PC = addr & ~1u;
                } else {
                    cpu->cpsr &= ~ARM_FLAG_T;
                    PC = addr & ~3u;
                }
                flush_pipeline(cpu);
                break;
            }
        }
        return 1;
    }

    /* Format 6: PC-relative load */
    if ((instr >> 11) == 0x09) {
        uint8_t rd = (instr >> 8) & 7;
        uint32_t offset = (instr & 0xFF) << 2;
        uint32_t addr = ((PC + 2) & ~3u) + offset; /* +2: pipeline (real PC = inst+4) */
        cpu->r[rd] = READ32(addr);
        return 3;
    }

    /* Format 7: Load/store with register offset */
    if ((instr >> 12) == 0x5 && !((instr >> 9) & 1)) {
        uint8_t ro = (instr >> 6) & 7;
        uint8_t rb = (instr >> 3) & 7;
        uint8_t rd = instr & 7;
        bool L = (instr >> 11) & 1;
        bool B = (instr >> 10) & 1;
        uint32_t addr = cpu->r[rb] + cpu->r[ro];

        if (L) {
            cpu->r[rd] = B ? READ8(addr) : READ32(addr);
        } else {
            if (B) WRITE8(addr, (uint8_t)cpu->r[rd]);
            else   WRITE32(addr, cpu->r[rd]);
        }
        return L ? 3 : 2;
    }

    /* Format 8: Load/store sign-extended byte/halfword */
    if ((instr >> 12) == 0x5 && ((instr >> 9) & 1)) {
        uint8_t ro = (instr >> 6) & 7;
        uint8_t rb = (instr >> 3) & 7;
        uint8_t rd = instr & 7;
        uint8_t op = (instr >> 10) & 3;
        uint32_t addr = cpu->r[rb] + cpu->r[ro];

        switch (op) {
            case 0: WRITE16(addr, (uint16_t)cpu->r[rd]); break; /* STRH */
            case 1: cpu->r[rd] = (uint32_t)(int32_t)(int8_t)READ8(addr); break; /* LDSB */
            case 2: cpu->r[rd] = READ16(addr); break; /* LDRH */
            case 3: cpu->r[rd] = (uint32_t)(int32_t)(int16_t)READ16(addr); break; /* LDSH */
        }
        return 3;
    }

    /* Format 9: Load/store with immediate offset */
    if ((instr >> 13) == 3) {
        bool B = (instr >> 12) & 1;
        bool L = (instr >> 11) & 1;
        uint8_t offset = (instr >> 6) & 0x1F;
        uint8_t rb = (instr >> 3) & 7;
        uint8_t rd = instr & 7;
        uint32_t addr = cpu->r[rb] + (B ? offset : offset << 2);

        if (L) {
            cpu->r[rd] = B ? READ8(addr) : READ32(addr);
        } else {
            if (B) WRITE8(addr, (uint8_t)cpu->r[rd]);
            else   WRITE32(addr, cpu->r[rd]);
        }
        return L ? 3 : 2;
    }

    /* Format 10: Load/store halfword */
    if ((instr >> 12) == 0x8) {
        bool L = (instr >> 11) & 1;
        uint8_t offset = ((instr >> 6) & 0x1F) << 1;
        uint8_t rb = (instr >> 3) & 7;
        uint8_t rd = instr & 7;
        uint32_t addr = cpu->r[rb] + offset;

        if (L) {
            cpu->r[rd] = READ16(addr);
        } else {
            WRITE16(addr, (uint16_t)cpu->r[rd]);
        }
        return L ? 3 : 2;
    }

    /* Format 11: SP-relative load/store */
    if ((instr >> 12) == 0x9) {
        bool L = (instr >> 11) & 1;
        uint8_t rd = (instr >> 8) & 7;
        uint32_t offset = (instr & 0xFF) << 2;
        uint32_t addr = SP + offset;

        if (L) {
            cpu->r[rd] = READ32(addr);
        } else {
            WRITE32(addr, cpu->r[rd]);
        }
        return L ? 3 : 2;
    }

    /* Format 12: Load address (ADD rd, PC/SP, #imm) */
    if ((instr >> 12) == 0xA) {
        bool sp = (instr >> 11) & 1;
        uint8_t rd = (instr >> 8) & 7;
        uint32_t offset = (instr & 0xFF) << 2;
        cpu->r[rd] = (sp ? SP : ((PC + 2) & ~3u)) + offset;
        return 1;
    }

    /* Format 13: Add offset to SP */
    if ((instr & 0xFF00) == 0xB000) {
        int32_t offset = (instr & 0x7F) << 2;
        if (instr & 0x80) offset = -offset;
        SP += offset;
        return 1;
    }

    /* Format 14: Push/Pop */
    if ((instr & 0xF600) == 0xB400) {
        bool L = (instr >> 11) & 1;
        bool R = (instr >> 8) & 1; /* PC/LR */
        uint8_t rlist = instr & 0xFF;

        if (L) {
            /* POP */
            for (int i = 0; i < 8; i++) {
                if (rlist & (1 << i)) {
                    cpu->r[i] = READ32(SP);
                    SP += 4;
                }
            }
            if (R) {
                uint32_t val = READ32(SP);
                SP += 4;
                if (val & 1) {
                    cpu->cpsr |= ARM_FLAG_T;
                    PC = val & ~1u;
                } else {
                    cpu->cpsr &= ~ARM_FLAG_T;
                    PC = val & ~3u;
                }
                flush_pipeline(cpu);
            }
        } else {
            /* PUSH */
            int count = 0;
            for (int i = 0; i < 8; i++) if (rlist & (1 << i)) count++;
            if (R) count++;
            SP -= count * 4;

            uint32_t addr = SP;
            for (int i = 0; i < 8; i++) {
                if (rlist & (1 << i)) {
                    WRITE32(addr, cpu->r[i]);
                    addr += 4;
                }
            }
            if (R) {
                WRITE32(addr, LR);
            }
        }
        return L ? 4 : 3;
    }

    /* Format 15: Multiple load/store */
    if ((instr >> 12) == 0xC) {
        bool L = (instr >> 11) & 1;
        uint8_t rb = (instr >> 8) & 7;
        uint8_t rlist = instr & 0xFF;
        uint32_t addr = cpu->r[rb];

        for (int i = 0; i < 8; i++) {
            if (!(rlist & (1 << i))) continue;
            if (L) {
                cpu->r[i] = READ32(addr);
            } else {
                WRITE32(addr, cpu->r[i]);
            }
            addr += 4;
        }

        /* Write-back: don't update if rb is in the list (for LDM) */
        if (!L || !(rlist & (1 << rb))) {
            cpu->r[rb] = addr;
        }

        return 3;
    }

    /* Format 16: Conditional branch */
    if ((instr >> 12) == 0xD && ((instr >> 8) & 0xF) < 0xE) {
        uint8_t cond = (instr >> 8) & 0xF;
        if (check_condition(cpu, cond)) {
            int32_t offset = (int32_t)(int8_t)(instr & 0xFF) << 1;
            PC += offset + 2; /* +2: pipeline (real PC = inst+4) */
            flush_pipeline(cpu);
            return 3;
        }
        return 1;
    }

    /* Format 17: Software interrupt (SWI) */
    if ((instr & 0xFF00) == 0xDF00) {
        uint8_t swi_num = instr & 0xFF;
        uint32_t return_addr = PC; /* Next instruction (PC = inst+2 after fetch) */
        uint32_t old_cpsr = cpu->cpsr;
        arm7_switch_mode(cpu, ARM_MODE_SVC);
        cpu->spsr = old_cpsr;
        LR = return_addr;
        cpu->cpsr |= ARM_FLAG_I;
        cpu->cpsr &= ~ARM_FLAG_T;
        hle_swi(cpu, swi_num);
        return 3;
    }

    /* Format 18: Unconditional branch */
    if ((instr >> 11) == 0x1C) {
        int32_t offset = (int32_t)(instr << 21) >> 20;
        static int b_trace = 0;
        if (b_trace < 5) {
            fprintf(stderr, "[F18] instr=0x%04X PC_before=0x%08X offset=%d PC_after=0x%08X\n",
                instr, PC, offset, PC + offset + 2);
            b_trace++;
        }
        PC += offset + 2; /* +2: pipeline */
        flush_pipeline(cpu);
        return 3;
    }

    /* Format 19: Long branch with link (two-instruction) */
    if ((instr >> 11) == 0x1E) {
        /* First instruction: LR = PC + (offset << 12) */
        int32_t offset = (int32_t)(instr << 21) >> 9;
        LR = PC + 2 + offset; /* +2: pipeline (real PC = inst+4) */
        return 1;
    }
    if ((instr >> 11) == 0x1F) {
        /* Second instruction: PC = LR + (offset << 1), LR = old PC | 1 */
        uint32_t offset = (instr & 0x7FF) << 1;
        uint32_t temp = PC; /* Next instruction address */
        PC = (LR + offset) & ~1u;
        LR = temp | 1;
        flush_pipeline(cpu);
        return 3;
    }

    /* Unknown: NOP */
    return 1;
}

/* ---------- Main step ---------- */

uint32_t arm7_step(ARM7TDMI *cpu) {
    static int irq_trace = 0;
    static int irq_loop_trace = 0;
    static int arm_misaligned_trace = 0;
    static int intrwait_wake_trace = 0;

    if (cpu->intrwait_active) {
        uint8_t mode = cpu->cpsr & ARM_MODE_MASK;
        if (mode == ARM_MODE_SVC) {
            uint16_t bios_if = READ16(0x03007FF8);
            if (bios_if & cpu->intrwait_flags) {
                if (intrwait_wake_trace < 30) {
                    fprintf(stderr,
                        "[INTRWAIT_WAKE %d] PC=0x%08X CPSR=0x%08X SPSR=0x%08X IF_BIOS=0x%04X flags=0x%04X LR=0x%08X\n",
                        intrwait_wake_trace, PC, cpu->cpsr, cpu->spsr, bios_if,
                        cpu->intrwait_flags, LR);
                    intrwait_wake_trace++;
                }
                /* Clear matched flags in BIOS IF (GBATEK: "automatically cleared") */
                bios_if &= ~cpu->intrwait_flags;
                WRITE16(0x03007FF8, bios_if);

                cpu->intrwait_active = false;
                cpu->halted = false;
                swi_return(cpu);
            } else {
                static int intrwait_poll_trace = 0;
                if (intrwait_poll_trace < 30) {
                    fprintf(stderr, "[INTRWAIT_POLL %d] PC=0x%08X BIOS_IF=0x%04X flags=0x%04X iwram[7FF8]=0x%02X%02X\n",
                        intrwait_poll_trace, PC, bios_if, cpu->intrwait_flags,
                        cpu->read8(cpu->mem_ctx, 0x03007FF9),
                        cpu->read8(cpu->mem_ctx, 0x03007FF8));
                    intrwait_poll_trace++;
                }
                /* SWI wait blocks only while still in SVC context. */
                cpu->halted = true;
            }
        } else {
            /* IRQ handler (or resumed caller) must be allowed to execute. */
            cpu->halted = false;
        }
    }

    if (cpu->halted) {
        arm7_check_interrupts(cpu);
        cpu->cycles += 1;
        cpu->total_cycles += 1;
        return 1;
    }

    arm7_check_interrupts(cpu);

        if (((cpu->cpsr & ARM_MODE_MASK) == ARM_MODE_IRQ) && !(cpu->cpsr & ARM_FLAG_T)
            && PC >= 0x03001BF0 && PC < 0x03001D40 && irq_loop_trace < 220) {
        uint32_t instr = cpu->read32(cpu->mem_ctx, PC);
        fprintf(stderr, "[IRQ_LOOP %d] PC=0x%08X instr=0x%08X LR=0x%08X SP=0x%08X CPSR=0x%08X\n",
            irq_loop_trace, PC, instr, LR, SP, cpu->cpsr);
        irq_loop_trace++;
    }

    /* Trace first instructions after VBlankIntrWait returns */
    if (vbl_swi_done >= 1 && post_vbl_traced < 1200) {
        uint32_t pc_before = PC;
        if (IS_THUMB()) {
            uint16_t instr = cpu->read16(cpu->mem_ctx, pc_before);
            fprintf(stderr, "[POST_VBL %d] T 0x%08X: 0x%04X R0=0x%08X R1=0x%08X R2=0x%08X R3=0x%08X LR=0x%08X SP=0x%08X mode=%d\n",
                post_vbl_traced, pc_before, instr,
                cpu->r[0], cpu->r[1], cpu->r[2], cpu->r[3],
                cpu->r[14], cpu->r[13], cpu->cpsr & 0x1F);
        } else {
            uint32_t instr = cpu->read32(cpu->mem_ctx, pc_before);
            fprintf(stderr, "[POST_VBL %d] A 0x%08X: 0x%08X R0=0x%08X R1=0x%08X R2=0x%08X R3=0x%08X LR=0x%08X SP=0x%08X mode=%d\n",
                post_vbl_traced, pc_before, instr,
                cpu->r[0], cpu->r[1], cpu->r[2], cpu->r[3],
                cpu->r[14], cpu->r[13], cpu->cpsr & 0x1F);
        }
        post_vbl_traced++;
    }

    if (!IS_THUMB() && (PC & 3u) && arm_misaligned_trace < 24) {
        uint32_t instr = cpu->read32(cpu->mem_ctx, PC);
        fprintf(stderr, "[ARM_MISALIGN %d] PC=0x%08X instr=0x%08X LR=0x%08X SP=0x%08X CPSR=0x%08X\n",
            arm_misaligned_trace, PC, instr, LR, SP, cpu->cpsr);
        arm_misaligned_trace++;
    }

    uint32_t cycles;
    if (IS_THUMB()) {
        cycles = thumb_execute(cpu);
    } else {
        cycles = arm_execute(cpu);
    }

    cpu->cycles += cycles;
    cpu->total_cycles += cycles;
    return cycles;
}

/* ---------- Serialization ---------- */

size_t arm7_serialize(const ARM7TDMI *cpu, uint8_t *buf, size_t buf_size) {
    /* Serialize all register state */
    size_t needed = sizeof(cpu->r) + sizeof(cpu->cpsr) + sizeof(cpu->spsr)
                  + sizeof(cpu->fiq_r8_r12) + sizeof(uint32_t) * 14 /* banked */
                  + sizeof(cpu->usr_r8_r12) + sizeof(cpu->halted)
                  + sizeof(cpu->cycles) + sizeof(cpu->total_cycles)
                  + sizeof(cpu->intrwait_active) + sizeof(cpu->intrwait_flags) + 64;
    if (!buf || buf_size < needed) return needed;

    size_t pos = 0;
    #define S(f) memcpy(buf+pos, &cpu->f, sizeof(cpu->f)); pos += sizeof(cpu->f)
    S(r); S(cpsr); S(spsr);
    S(fiq_r8_r12); S(fiq_r13); S(fiq_r14); S(fiq_spsr);
    S(usr_r8_r12);
    S(irq_r13); S(irq_r14); S(irq_spsr);
    S(svc_r13); S(svc_r14); S(svc_spsr);
    S(abt_r13); S(abt_r14); S(abt_spsr);
    S(und_r13); S(und_r14); S(und_spsr);
    S(usr_r13); S(usr_r14);
    S(halted); S(cycles); S(total_cycles);
    S(intrwait_active); S(intrwait_flags);
    #undef S
    return pos;
}

size_t arm7_deserialize(ARM7TDMI *cpu, const uint8_t *buf, size_t buf_size) {
    size_t pos = 0;
    #define L(f) do { if(pos+sizeof(cpu->f)>buf_size) return 0; \
        memcpy(&cpu->f, buf+pos, sizeof(cpu->f)); pos += sizeof(cpu->f); } while(0)
    L(r); L(cpsr); L(spsr);
    L(fiq_r8_r12); L(fiq_r13); L(fiq_r14); L(fiq_spsr);
    L(usr_r8_r12);
    L(irq_r13); L(irq_r14); L(irq_spsr);
    L(svc_r13); L(svc_r14); L(svc_spsr);
    L(abt_r13); L(abt_r14); L(abt_spsr);
    L(und_r13); L(und_r14); L(und_spsr);
    L(usr_r13); L(usr_r14);
    L(halted); L(cycles); L(total_cycles);
    L(intrwait_active); L(intrwait_flags);
    #undef L
    cpu->pipeline_valid = false;
    return pos;
}
