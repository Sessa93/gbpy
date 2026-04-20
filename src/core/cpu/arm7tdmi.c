/*
 * ARM7TDMI CPU Implementation for Game Boy Advance
 *
 * ARM instruction set: 32-bit, condition-code based execution
 * Thumb instruction set: 16-bit subset for code density
 *
 * This implements all ARM and Thumb instructions needed for GBA emulation.
 */

#include "arm7tdmi.h"

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
        uint32_t return_addr = PC - (IS_THUMB() ? 2 : 4) + 4;
        arm7_switch_mode(cpu, ARM_MODE_FIQ);
        cpu->spsr = cpu->cpsr;
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
        uint32_t return_addr = PC - (IS_THUMB() ? 2 : 4) + 4;
        arm7_switch_mode(cpu, ARM_MODE_IRQ);
        cpu->spsr = cpu->cpsr;
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
        op2 = (imm >> rot) | (imm << (32 - rot));
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
        cpu->r[rd] = result;
        if (rd == 15) {
            if (S) {
                /* MOVS PC, ... restores CPSR from SPSR */
                cpu->cpsr = cpu->spsr;
            }
            flush_pipeline(cpu);
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
        if (rd == 15) flush_pipeline(cpu);
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
        if (rd == 15) flush_pipeline(cpu);
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
    bool S = (instr >> 22) & 1; /* User bank transfer */
    bool W = (instr >> 21) & 1;
    bool L = (instr >> 20) & 1;
    uint16_t rlist = instr & 0xFFFF;

    uint32_t base = cpu->r[rn];
    int count = 0;
    for (int i = 0; i < 16; i++) {
        if (rlist & (1 << i)) count++;
    }
    if (count == 0) count = 16; /* Empty rlist: transfer R15 and add 0x40 */

    uint32_t start_addr;
    if (U) {
        start_addr = P ? base + 4 : base;
    } else {
        start_addr = P ? base - count * 4 : base - count * 4 + 4;
    }

    (void)S; /* User bank transfer - simplified */

    uint32_t addr = start_addr;
    for (int i = 0; i < 16; i++) {
        if (!(rlist & (1 << i))) continue;
        if (L) {
            cpu->r[i] = READ32(addr);
            if (i == 15) flush_pipeline(cpu);
        } else {
            uint32_t val = cpu->r[i];
            if (i == 15) val += 4;
            WRITE32(addr, val);
        }
        addr += 4;
    }

    if (W) {
        cpu->r[rn] = U ? base + count * 4 : base - count * 4;
    }

    return count + (L ? 2 : 1);
}

/* ARM Branch / Branch with Link */
static uint32_t arm_branch(ARM7TDMI *cpu, uint32_t instr) {
    bool L = (instr >> 24) & 1;
    int32_t offset = (int32_t)(instr << 8) >> 6; /* Sign-extend 24-bit, *4 */

    if (L) {
        LR = PC - 4; /* Save return address */
    }

    PC += offset;
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

/* ARM Software Interrupt */
static uint32_t arm_swi(ARM7TDMI *cpu, uint32_t instr) {
    (void)instr;
    uint32_t return_addr = PC - 4;
    arm7_switch_mode(cpu, ARM_MODE_SVC);
    cpu->spsr = cpu->cpsr;
    LR = return_addr;
    cpu->cpsr |= ARM_FLAG_I;
    cpu->cpsr &= ~ARM_FLAG_T;
    PC = 0x08;
    flush_pipeline(cpu);
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
        val = (imm >> rot) | (imm << (32 - rot));
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
        cpu->cpsr = (cpu->cpsr & ~mask) | (val & mask);
        if ((old_cpsr & ARM_MODE_MASK) != (cpu->cpsr & ARM_MODE_MASK)) {
            arm7_switch_mode(cpu, cpu->cpsr & ARM_MODE_MASK);
        }
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
    LR = PC - 4;
    PC += offset + h;
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

    /* Format 1: Move shifted register */
    if ((instr >> 13) == 0) {
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
        uint32_t b = (op & 1) ? rn_or_imm : cpu->r[rn_or_imm];

        if (op < 2) {
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

        switch (op) {
            case 0: /* ADD */
                cpu->r[rd] += cpu->r[rs];
                if (rd == 15) flush_pipeline(cpu);
                break;
            case 1: /* CMP */
                sub_with_carry(cpu, cpu->r[rd], cpu->r[rs], true, true);
                break;
            case 2: /* MOV */
                cpu->r[rd] = cpu->r[rs];
                if (rd == 15) flush_pipeline(cpu);
                break;
            case 3: { /* BX */
                uint32_t addr = cpu->r[rs];
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
        uint32_t addr = (PC & ~3u) + offset;
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
        cpu->r[rd] = (sp ? SP : (PC & ~3u)) + offset;
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
            PC += offset;
            flush_pipeline(cpu);
            return 3;
        }
        return 1;
    }

    /* Format 17: Software interrupt (SWI) */
    if ((instr & 0xFF00) == 0xDF00) {
        uint32_t return_addr = PC - 2;
        arm7_switch_mode(cpu, ARM_MODE_SVC);
        cpu->spsr = cpu->cpsr;
        LR = return_addr;
        cpu->cpsr |= ARM_FLAG_I;
        cpu->cpsr &= ~ARM_FLAG_T;
        PC = 0x08;
        flush_pipeline(cpu);
        return 3;
    }

    /* Format 18: Unconditional branch */
    if ((instr >> 11) == 0x1C) {
        int32_t offset = (int32_t)(instr << 21) >> 20;
        PC += offset;
        flush_pipeline(cpu);
        return 3;
    }

    /* Format 19: Long branch with link (two-instruction) */
    if ((instr >> 11) == 0x1E) {
        /* First instruction: LR = PC + (offset << 12) */
        int32_t offset = (int32_t)(instr << 21) >> 9;
        LR = PC + offset;
        return 1;
    }
    if ((instr >> 11) == 0x1F) {
        /* Second instruction: PC = LR + (offset << 1), LR = old PC | 1 */
        uint32_t offset = (instr & 0x7FF) << 1;
        uint32_t temp = PC - 2;
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
    if (cpu->halted) {
        arm7_check_interrupts(cpu);
        return 1;
    }

    arm7_check_interrupts(cpu);

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
                  + sizeof(cpu->cycles) + sizeof(cpu->total_cycles) + 64;
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
    #undef L
    cpu->pipeline_valid = false;
    return pos;
}
