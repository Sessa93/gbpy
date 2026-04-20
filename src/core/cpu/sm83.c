/*
 * SM83 CPU Implementation - Game Boy / Game Boy Color
 *
 * Complete implementation of all opcodes documented in:
 *   Game Boy CPU Manual v1.01 (resources/gba-manual.md)
 *
 * Opcode reference: Sections 3.3.1 through 3.3.11
 * - 3.3.1: 8-Bit Loads (LD, LDD, LDI, LDH)
 * - 3.3.2: 16-Bit Loads (LD, PUSH, POP)
 * - 3.3.3: 8-Bit ALU (ADD, ADC, SUB, SBC, AND, OR, XOR, CP, INC, DEC)
 * - 3.3.4: 16-Bit Arithmetic (ADD HL, ADD SP, INC nn, DEC nn)
 * - 3.3.5: Miscellaneous (SWAP, DAA, CPL, CCF, SCF, NOP, HALT, STOP, DI, EI)
 * - 3.3.6: Rotates & Shifts (RLCA, RLA, RRCA, RRA, RLC, RL, RRC, RR, SLA, SRA, SRL)
 * - 3.3.7: Bit Opcodes (BIT, SET, RES)
 * - 3.3.8: Jumps (JP, JR)
 * - 3.3.9: Calls (CALL)
 * - 3.3.10: Restarts (RST)
 * - 3.3.11: Returns (RET, RETI)
 */

#include "sm83.h"

/* ---------- Helper macros ---------- */

/* Read a byte from memory via the callback */
#define READ(addr) cpu->read(cpu->mem_ctx, (addr))
#define WRITE(addr, val) cpu->write(cpu->mem_ctx, (addr), (val))

/* Read the next byte at PC and increment PC */
#define FETCH() READ(cpu->pc++)

/* Read a 16-bit value (little-endian) at PC */
GBPY_INLINE uint16_t fetch16(SM83 *cpu) {
    uint8_t lo = FETCH();
    uint8_t hi = FETCH();
    return (uint16_t)((hi << 8) | lo);
}

/* Stack operations (Section 3.2.4) */
GBPY_INLINE void push16(SM83 *cpu, uint16_t val) {
    cpu->sp--;
    WRITE(cpu->sp, (uint8_t)(val >> 8));
    cpu->sp--;
    WRITE(cpu->sp, (uint8_t)(val & 0xFF));
}

GBPY_INLINE uint16_t pop16(SM83 *cpu) {
    uint8_t lo = READ(cpu->sp++);
    uint8_t hi = READ(cpu->sp++);
    return (uint16_t)((hi << 8) | lo);
}

/* Flag helpers */
#define SET_FLAG(flag)   (cpu->f |= (flag))
#define CLEAR_FLAG(flag) (cpu->f &= ~(flag))
#define CHECK_FLAG(flag) ((cpu->f & (flag)) != 0)

GBPY_INLINE void set_flag_cond(SM83 *cpu, uint8_t flag, bool cond) {
    if (cond) cpu->f |= flag;
    else cpu->f &= ~flag;
}

/* ---------- 8-bit ALU operations (Section 3.3.3) ---------- */

/* ADD A,n - Section 3.3.3.1 */
GBPY_INLINE void alu_add(SM83 *cpu, uint8_t val) {
    uint16_t result = cpu->a + val;
    cpu->f = 0;
    set_flag_cond(cpu, FLAG_Z, (uint8_t)result == 0);
    set_flag_cond(cpu, FLAG_H, ((cpu->a & 0x0F) + (val & 0x0F)) > 0x0F);
    set_flag_cond(cpu, FLAG_C, result > 0xFF);
    cpu->a = (uint8_t)result;
}

/* ADC A,n - Section 3.3.3.2 */
GBPY_INLINE void alu_adc(SM83 *cpu, uint8_t val) {
    uint8_t carry = CHECK_FLAG(FLAG_C) ? 1 : 0;
    uint16_t result = cpu->a + val + carry;
    cpu->f = 0;
    set_flag_cond(cpu, FLAG_Z, (uint8_t)result == 0);
    set_flag_cond(cpu, FLAG_H, ((cpu->a & 0x0F) + (val & 0x0F) + carry) > 0x0F);
    set_flag_cond(cpu, FLAG_C, result > 0xFF);
    cpu->a = (uint8_t)result;
}

/* SUB n - Section 3.3.3.3 */
GBPY_INLINE void alu_sub(SM83 *cpu, uint8_t val) {
    uint16_t result = cpu->a - val;
    cpu->f = FLAG_N;
    set_flag_cond(cpu, FLAG_Z, (uint8_t)result == 0);
    set_flag_cond(cpu, FLAG_H, (cpu->a & 0x0F) < (val & 0x0F));
    set_flag_cond(cpu, FLAG_C, cpu->a < val);
    cpu->a = (uint8_t)result;
}

/* SBC A,n - Section 3.3.3.4 */
GBPY_INLINE void alu_sbc(SM83 *cpu, uint8_t val) {
    uint8_t carry = CHECK_FLAG(FLAG_C) ? 1 : 0;
    int result = cpu->a - val - carry;
    cpu->f = FLAG_N;
    set_flag_cond(cpu, FLAG_Z, (uint8_t)result == 0);
    set_flag_cond(cpu, FLAG_H, (int)(cpu->a & 0x0F) - (int)(val & 0x0F) - (int)carry < 0);
    set_flag_cond(cpu, FLAG_C, result < 0);
    cpu->a = (uint8_t)result;
}

/* AND n - Section 3.3.3.5: Z=*, N=0, H=1, C=0 */
GBPY_INLINE void alu_and(SM83 *cpu, uint8_t val) {
    cpu->a &= val;
    cpu->f = FLAG_H;
    set_flag_cond(cpu, FLAG_Z, cpu->a == 0);
}

/* OR n - Section 3.3.3.6: Z=*, N=0, H=0, C=0 */
GBPY_INLINE void alu_or(SM83 *cpu, uint8_t val) {
    cpu->a |= val;
    cpu->f = 0;
    set_flag_cond(cpu, FLAG_Z, cpu->a == 0);
}

/* XOR n - Section 3.3.3.7: Z=*, N=0, H=0, C=0 */
GBPY_INLINE void alu_xor(SM83 *cpu, uint8_t val) {
    cpu->a ^= val;
    cpu->f = 0;
    set_flag_cond(cpu, FLAG_Z, cpu->a == 0);
}

/* CP n - Section 3.3.3.8: Compare A with n (A - n, result discarded) */
GBPY_INLINE void alu_cp(SM83 *cpu, uint8_t val) {
    cpu->f = FLAG_N;
    set_flag_cond(cpu, FLAG_Z, cpu->a == val);
    set_flag_cond(cpu, FLAG_H, (cpu->a & 0x0F) < (val & 0x0F));
    set_flag_cond(cpu, FLAG_C, cpu->a < val);
}

/* INC n - Section 3.3.3.9: Z=*, N=0, H=*, C=unaffected */
GBPY_INLINE uint8_t alu_inc(SM83 *cpu, uint8_t val) {
    uint8_t result = val + 1;
    CLEAR_FLAG(FLAG_N);
    set_flag_cond(cpu, FLAG_Z, result == 0);
    set_flag_cond(cpu, FLAG_H, (val & 0x0F) == 0x0F);
    return result;
}

/* DEC n - Section 3.3.3.10: Z=*, N=1, H=*, C=unaffected */
GBPY_INLINE uint8_t alu_dec(SM83 *cpu, uint8_t val) {
    uint8_t result = val - 1;
    SET_FLAG(FLAG_N);
    set_flag_cond(cpu, FLAG_Z, result == 0);
    set_flag_cond(cpu, FLAG_H, (val & 0x0F) == 0x00);
    return result;
}

/* ---------- Rotate/Shift operations (Section 3.3.6) ---------- */

GBPY_INLINE uint8_t alu_rlc(SM83 *cpu, uint8_t val) {
    uint8_t carry = (val >> 7) & 1;
    uint8_t result = (val << 1) | carry;
    cpu->f = 0;
    set_flag_cond(cpu, FLAG_Z, result == 0);
    set_flag_cond(cpu, FLAG_C, carry);
    return result;
}

GBPY_INLINE uint8_t alu_rrc(SM83 *cpu, uint8_t val) {
    uint8_t carry = val & 1;
    uint8_t result = (val >> 1) | (carry << 7);
    cpu->f = 0;
    set_flag_cond(cpu, FLAG_Z, result == 0);
    set_flag_cond(cpu, FLAG_C, carry);
    return result;
}

GBPY_INLINE uint8_t alu_rl(SM83 *cpu, uint8_t val) {
    uint8_t old_carry = CHECK_FLAG(FLAG_C) ? 1 : 0;
    uint8_t new_carry = (val >> 7) & 1;
    uint8_t result = (val << 1) | old_carry;
    cpu->f = 0;
    set_flag_cond(cpu, FLAG_Z, result == 0);
    set_flag_cond(cpu, FLAG_C, new_carry);
    return result;
}

GBPY_INLINE uint8_t alu_rr(SM83 *cpu, uint8_t val) {
    uint8_t old_carry = CHECK_FLAG(FLAG_C) ? 1 : 0;
    uint8_t new_carry = val & 1;
    uint8_t result = (val >> 1) | (old_carry << 7);
    cpu->f = 0;
    set_flag_cond(cpu, FLAG_Z, result == 0);
    set_flag_cond(cpu, FLAG_C, new_carry);
    return result;
}

GBPY_INLINE uint8_t alu_sla(SM83 *cpu, uint8_t val) {
    uint8_t carry = (val >> 7) & 1;
    uint8_t result = val << 1;
    cpu->f = 0;
    set_flag_cond(cpu, FLAG_Z, result == 0);
    set_flag_cond(cpu, FLAG_C, carry);
    return result;
}

GBPY_INLINE uint8_t alu_sra(SM83 *cpu, uint8_t val) {
    uint8_t carry = val & 1;
    uint8_t result = (val >> 1) | (val & 0x80); /* MSB doesn't change */
    cpu->f = 0;
    set_flag_cond(cpu, FLAG_Z, result == 0);
    set_flag_cond(cpu, FLAG_C, carry);
    return result;
}

GBPY_INLINE uint8_t alu_srl(SM83 *cpu, uint8_t val) {
    uint8_t carry = val & 1;
    uint8_t result = val >> 1;
    cpu->f = 0;
    set_flag_cond(cpu, FLAG_Z, result == 0);
    set_flag_cond(cpu, FLAG_C, carry);
    return result;
}

/* SWAP n - Section 3.3.5.1: Swap upper & lower nibbles */
GBPY_INLINE uint8_t alu_swap(SM83 *cpu, uint8_t val) {
    uint8_t result = ((val & 0x0F) << 4) | ((val & 0xF0) >> 4);
    cpu->f = 0;
    set_flag_cond(cpu, FLAG_Z, result == 0);
    return result;
}

/* BIT b,r - Section 3.3.7.1: Test bit b */
GBPY_INLINE void alu_bit(SM83 *cpu, uint8_t bit, uint8_t val) {
    set_flag_cond(cpu, FLAG_Z, !(val & (1 << bit)));
    CLEAR_FLAG(FLAG_N);
    SET_FLAG(FLAG_H);
}

/* ---------- Register access helper for CB-prefix instructions ---------- */

static uint8_t read_r8(SM83 *cpu, uint8_t idx) {
    switch (idx) {
        case 0: return cpu->b;
        case 1: return cpu->c;
        case 2: return cpu->d;
        case 3: return cpu->e;
        case 4: return cpu->h;
        case 5: return cpu->l;
        case 6: return READ(cpu->hl);
        case 7: return cpu->a;
        default: return 0;
    }
}

static void write_r8(SM83 *cpu, uint8_t idx, uint8_t val) {
    switch (idx) {
        case 0: cpu->b = val; break;
        case 1: cpu->c = val; break;
        case 2: cpu->d = val; break;
        case 3: cpu->e = val; break;
        case 4: cpu->h = val; break;
        case 5: cpu->l = val; break;
        case 6: WRITE(cpu->hl, val); break;
        case 7: cpu->a = val; break;
    }
}

/* ---------- CB-prefixed instructions (Section 3.3.6, 3.3.7) ---------- */

uint32_t sm83_execute_cb(SM83 *cpu) {
    uint8_t op = FETCH();
    uint8_t reg = op & 0x07;
    uint8_t bit = (op >> 3) & 0x07;
    uint8_t val = read_r8(cpu, reg);
    uint32_t cycles = (reg == 6) ? 16 : 8;
    uint8_t result;

    switch (op & 0xF8) {
        /* RLC r - Section 3.3.6.5 */
        case 0x00: result = alu_rlc(cpu, val); write_r8(cpu, reg, result); break;
        /* RRC r - Section 3.3.6.7 */
        case 0x08: result = alu_rrc(cpu, val); write_r8(cpu, reg, result); break;
        /* RL r - Section 3.3.6.6 */
        case 0x10: result = alu_rl(cpu, val); write_r8(cpu, reg, result); break;
        /* RR r - Section 3.3.6.8 */
        case 0x18: result = alu_rr(cpu, val); write_r8(cpu, reg, result); break;
        /* SLA r - Section 3.3.6.9 */
        case 0x20: result = alu_sla(cpu, val); write_r8(cpu, reg, result); break;
        /* SRA r - Section 3.3.6.10 */
        case 0x28: result = alu_sra(cpu, val); write_r8(cpu, reg, result); break;
        /* SWAP r - Section 3.3.5.1 */
        case 0x30: result = alu_swap(cpu, val); write_r8(cpu, reg, result); break;
        /* SRL r - Section 3.3.6.11 */
        case 0x38: result = alu_srl(cpu, val); write_r8(cpu, reg, result); break;

        /* BIT b,r - Section 3.3.7.1 */
        case 0x40: case 0x48: case 0x50: case 0x58:
        case 0x60: case 0x68: case 0x70: case 0x78:
            alu_bit(cpu, bit, val);
            if (reg == 6) cycles = 12;
            break;

        /* RES b,r - Section 3.3.7.3 */
        case 0x80: case 0x88: case 0x90: case 0x98:
        case 0xA0: case 0xA8: case 0xB0: case 0xB8:
            write_r8(cpu, reg, val & ~(1 << bit));
            break;

        /* SET b,r - Section 3.3.7.2 */
        case 0xC0: case 0xC8: case 0xD0: case 0xD8:
        case 0xE0: case 0xE8: case 0xF0: case 0xF8:
            write_r8(cpu, reg, val | (1 << bit));
            break;
    }

    return cycles;
}

/* ---------- Initialize ---------- */

void sm83_init(SM83 *cpu, sm83_read_fn read, sm83_write_fn write, void *ctx) {
    memset(cpu, 0, sizeof(SM83));
    cpu->read = read;
    cpu->write = write;
    cpu->mem_ctx = ctx;
}

/*
 * Reset to power-up state (Section 2.7.1 Power Up Sequence)
 *
 * "AF=$01-GB/SGB, $FF-GBP, $11-GBC"
 * "F=$B0"
 * "BC=$0013, DE=$00D8, HL=$014D"
 * "Stack Pointer=$FFFE"
 * PC starts at $0100
 */
void sm83_reset(SM83 *cpu, GbpyMode mode) {
    if (mode == GBPY_MODE_GBC) {
        cpu->a = 0x11;
    } else {
        cpu->a = 0x01;
    }
    cpu->f = 0xB0;
    cpu->b = 0x00; cpu->c = 0x13;
    cpu->d = 0x00; cpu->e = 0xD8;
    cpu->h = 0x01; cpu->l = 0x4D;
    cpu->sp = 0xFFFE;
    cpu->pc = 0x0100;

    cpu->ime = false;
    cpu->ime_pending = false;
    cpu->halted = false;
    cpu->stopped = false;
    cpu->halt_bug = false;
    cpu->double_speed = false;
    cpu->cycles = 0;
    cpu->total_cycles = 0;
}

/* ---------- Interrupt handling (Section 2.12) ---------- */

void sm83_request_interrupt(SM83 *cpu, uint8_t interrupt) {
    /* Set bit in IF register (FF0F) */
    uint8_t if_reg = READ(0xFF0F);
    WRITE(0xFF0F, if_reg | interrupt);
}

/*
 * Handle pending interrupts (Section 2.12.1):
 * 1. When an interrupt is generated, the IF flag will be set.
 * 2. If IME flag is set & corresponding IE flag is set:
 *    a. Reset IME flag
 *    b. Push PC onto stack
 *    c. Jump to interrupt start address
 */
uint32_t sm83_handle_interrupts(SM83 *cpu) {
    uint8_t ie = READ(0xFFFF);  /* Interrupt Enable */
    uint8_t if_reg = READ(0xFF0F);  /* Interrupt Flag */
    uint8_t pending = ie & if_reg & 0x1F;

    if (pending == 0) return 0;

    /* Any pending interrupt wakes from HALT */
    cpu->halted = false;

    if (!cpu->ime) return 0;

    cpu->ime = false;

    /* Service highest priority interrupt (Section 2.12.2) */
    static const uint8_t int_bits[] = {
        INT_VBLANK_BIT, INT_LCD_BIT, INT_TIMER_BIT,
        INT_SERIAL_BIT, INT_JOYPAD_BIT
    };
    static const uint16_t int_addrs[] = {
        INT_VBLANK_ADDR, INT_LCD_ADDR, INT_TIMER_ADDR,
        INT_SERIAL_ADDR, INT_JOYPAD_ADDR
    };

    for (int i = 0; i < 5; i++) {
        if (pending & int_bits[i]) {
            /* Clear IF flag bit */
            WRITE(0xFF0F, if_reg & ~int_bits[i]);
            /* Push PC and jump */
            push16(cpu, cpu->pc);
            cpu->pc = int_addrs[i];
            return 20; /* Interrupt dispatch takes 5 machine cycles = 20 clocks */
        }
    }

    return 0;
}

/* ---------- Main execution (all opcodes) ---------- */

/*
 * Execute one instruction. Returns the number of clock cycles consumed.
 *
 * Full opcode implementation per Game Boy CPU Manual sections 3.3.1-3.3.11.
 * Cycle counts are in T-cycles (clock cycles), 1 machine cycle = 4 T-cycles.
 */
uint32_t sm83_step(SM83 *cpu) {
    uint32_t cycles = 0;

    if (cpu->halted) {
        return 4; /* HALT consumes 4 cycles per step */
    }

    /* Handle EI delay: "Interrupts are enabled after instruction after EI" */
    bool pending_ime = cpu->ime_pending;
    if (cpu->ime_pending) {
        cpu->ime_pending = false;
        cpu->ime = true;
    }

    uint8_t opcode = FETCH();

    /* HALT bug: PC increment is skipped for the next instruction (Section 2.7.3) */
    if (cpu->halt_bug) {
        cpu->pc--;
        cpu->halt_bug = false;
    }

    uint8_t tmp8;
    uint16_t tmp16;
    int8_t stmp8;
    (void)pending_ime;

    switch (opcode) {
        /* ===== 0x00-0x0F ===== */

        /* NOP - Section 3.3.5.6: Opcode 00, 4 cycles */
        case 0x00: cycles = 4; break;

        /* LD BC,nn - Section 3.3.2.1: Opcode 01, 12 cycles */
        case 0x01: cpu->bc = fetch16(cpu); cycles = 12; break;

        /* LD (BC),A - Section 3.3.1.4: Opcode 02, 8 cycles */
        case 0x02: WRITE(cpu->bc, cpu->a); cycles = 8; break;

        /* INC BC - Section 3.3.4.3: Opcode 03, 8 cycles */
        case 0x03: cpu->bc++; cycles = 8; break;

        /* INC B - Section 3.3.3.9: Opcode 04, 4 cycles */
        case 0x04: cpu->b = alu_inc(cpu, cpu->b); cycles = 4; break;

        /* DEC B - Section 3.3.3.10: Opcode 05, 4 cycles */
        case 0x05: cpu->b = alu_dec(cpu, cpu->b); cycles = 4; break;

        /* LD B,n - Section 3.3.1.1: Opcode 06, 8 cycles */
        case 0x06: cpu->b = FETCH(); cycles = 8; break;

        /* RLCA - Section 3.3.6.1: Opcode 07, 4 cycles */
        case 0x07:
            cpu->a = alu_rlc(cpu, cpu->a);
            CLEAR_FLAG(FLAG_Z); /* RLCA always clears Z unlike CB RLC A */
            cycles = 4;
            break;

        /* LD (nn),SP - Section 3.3.2.5: Opcode 08, 20 cycles */
        case 0x08:
            tmp16 = fetch16(cpu);
            WRITE(tmp16, (uint8_t)(cpu->sp & 0xFF));
            WRITE(tmp16 + 1, (uint8_t)(cpu->sp >> 8));
            cycles = 20;
            break;

        /* ADD HL,BC - Section 3.3.4.1: Opcode 09, 8 cycles */
        case 0x09:
            tmp16 = cpu->hl + cpu->bc;
            CLEAR_FLAG(FLAG_N);
            set_flag_cond(cpu, FLAG_H, ((cpu->hl & 0x0FFF) + (cpu->bc & 0x0FFF)) > 0x0FFF);
            set_flag_cond(cpu, FLAG_C, ((uint32_t)cpu->hl + cpu->bc) > 0xFFFF);
            cpu->hl = tmp16;
            cycles = 8;
            break;

        /* LD A,(BC) - Section 3.3.1.3: Opcode 0A, 8 cycles */
        case 0x0A: cpu->a = READ(cpu->bc); cycles = 8; break;

        /* DEC BC - Section 3.3.4.4: Opcode 0B, 8 cycles */
        case 0x0B: cpu->bc--; cycles = 8; break;

        /* INC C - Opcode 0C, 4 cycles */
        case 0x0C: cpu->c = alu_inc(cpu, cpu->c); cycles = 4; break;

        /* DEC C - Opcode 0D, 4 cycles */
        case 0x0D: cpu->c = alu_dec(cpu, cpu->c); cycles = 4; break;

        /* LD C,n - Opcode 0E, 8 cycles */
        case 0x0E: cpu->c = FETCH(); cycles = 8; break;

        /* RRCA - Section 3.3.6.3: Opcode 0F, 4 cycles */
        case 0x0F:
            cpu->a = alu_rrc(cpu, cpu->a);
            CLEAR_FLAG(FLAG_Z);
            cycles = 4;
            break;

        /* ===== 0x10-0x1F ===== */

        /* STOP - Section 3.3.5.8: Opcode 10 00, 4 cycles */
        case 0x10:
            FETCH(); /* consume the 0x00 byte */
            cpu->stopped = true;
            cycles = 4;
            break;

        /* LD DE,nn - Opcode 11, 12 cycles */
        case 0x11: cpu->de = fetch16(cpu); cycles = 12; break;

        /* LD (DE),A - Opcode 12, 8 cycles */
        case 0x12: WRITE(cpu->de, cpu->a); cycles = 8; break;

        /* INC DE - Opcode 13, 8 cycles */
        case 0x13: cpu->de++; cycles = 8; break;

        /* INC D - Opcode 14, 4 cycles */
        case 0x14: cpu->d = alu_inc(cpu, cpu->d); cycles = 4; break;

        /* DEC D - Opcode 15, 4 cycles */
        case 0x15: cpu->d = alu_dec(cpu, cpu->d); cycles = 4; break;

        /* LD D,n - Opcode 16, 8 cycles */
        case 0x16: cpu->d = FETCH(); cycles = 8; break;

        /* RLA - Section 3.3.6.2: Opcode 17, 4 cycles */
        case 0x17:
            cpu->a = alu_rl(cpu, cpu->a);
            CLEAR_FLAG(FLAG_Z);
            cycles = 4;
            break;

        /* JR n - Section 3.3.8.4: Opcode 18, 12 cycles */
        case 0x18:
            stmp8 = (int8_t)FETCH();
            cpu->pc += stmp8;
            cycles = 12;
            break;

        /* ADD HL,DE - Opcode 19, 8 cycles */
        case 0x19:
            CLEAR_FLAG(FLAG_N);
            set_flag_cond(cpu, FLAG_H, ((cpu->hl & 0x0FFF) + (cpu->de & 0x0FFF)) > 0x0FFF);
            set_flag_cond(cpu, FLAG_C, ((uint32_t)cpu->hl + cpu->de) > 0xFFFF);
            cpu->hl += cpu->de;
            cycles = 8;
            break;

        /* LD A,(DE) - Opcode 1A, 8 cycles */
        case 0x1A: cpu->a = READ(cpu->de); cycles = 8; break;

        /* DEC DE - Opcode 1B, 8 cycles */
        case 0x1B: cpu->de--; cycles = 8; break;

        /* INC E - Opcode 1C, 4 cycles */
        case 0x1C: cpu->e = alu_inc(cpu, cpu->e); cycles = 4; break;

        /* DEC E - Opcode 1D, 4 cycles */
        case 0x1D: cpu->e = alu_dec(cpu, cpu->e); cycles = 4; break;

        /* LD E,n - Opcode 1E, 8 cycles */
        case 0x1E: cpu->e = FETCH(); cycles = 8; break;

        /* RRA - Section 3.3.6.4: Opcode 1F, 4 cycles */
        case 0x1F:
            cpu->a = alu_rr(cpu, cpu->a);
            CLEAR_FLAG(FLAG_Z);
            cycles = 4;
            break;

        /* ===== 0x20-0x2F ===== */

        /* JR NZ,n - Section 3.3.8.5: Opcode 20, 12/8 cycles */
        case 0x20:
            stmp8 = (int8_t)FETCH();
            if (!CHECK_FLAG(FLAG_Z)) { cpu->pc += stmp8; cycles = 12; }
            else { cycles = 8; }
            break;

        /* LD HL,nn - Opcode 21, 12 cycles */
        case 0x21: cpu->hl = fetch16(cpu); cycles = 12; break;

        /* LDI (HL),A - Section 3.3.1.18: Opcode 22, 8 cycles */
        case 0x22: WRITE(cpu->hl, cpu->a); cpu->hl++; cycles = 8; break;

        /* INC HL - Opcode 23, 8 cycles */
        case 0x23: cpu->hl++; cycles = 8; break;

        /* INC H - Opcode 24, 4 cycles */
        case 0x24: cpu->h = alu_inc(cpu, cpu->h); cycles = 4; break;

        /* DEC H - Opcode 25, 4 cycles */
        case 0x25: cpu->h = alu_dec(cpu, cpu->h); cycles = 4; break;

        /* LD H,n - Opcode 26, 8 cycles */
        case 0x26: cpu->h = FETCH(); cycles = 8; break;

        /* DAA - Section 3.3.5.2: Opcode 27, 4 cycles
         * Decimal adjust register A for BCD operations */
        case 0x27: {
            int16_t a = cpu->a;
            if (!CHECK_FLAG(FLAG_N)) {
                if (CHECK_FLAG(FLAG_H) || (a & 0x0F) > 9) a += 0x06;
                if (CHECK_FLAG(FLAG_C) || a > 0x9F) { a += 0x60; SET_FLAG(FLAG_C); }
            } else {
                if (CHECK_FLAG(FLAG_H)) { a -= 0x06; a &= 0xFF; }
                if (CHECK_FLAG(FLAG_C)) a -= 0x60;
            }
            CLEAR_FLAG(FLAG_H);
            cpu->a = (uint8_t)(a & 0xFF);
            set_flag_cond(cpu, FLAG_Z, cpu->a == 0);
            cycles = 4;
            break;
        }

        /* JR Z,n - Opcode 28, 12/8 cycles */
        case 0x28:
            stmp8 = (int8_t)FETCH();
            if (CHECK_FLAG(FLAG_Z)) { cpu->pc += stmp8; cycles = 12; }
            else { cycles = 8; }
            break;

        /* ADD HL,HL - Opcode 29, 8 cycles */
        case 0x29:
            CLEAR_FLAG(FLAG_N);
            set_flag_cond(cpu, FLAG_H, ((cpu->hl & 0x0FFF) + (cpu->hl & 0x0FFF)) > 0x0FFF);
            set_flag_cond(cpu, FLAG_C, ((uint32_t)cpu->hl + cpu->hl) > 0xFFFF);
            cpu->hl += cpu->hl;
            cycles = 8;
            break;

        /* LDI A,(HL) - Section 3.3.1.15: Opcode 2A, 8 cycles */
        case 0x2A: cpu->a = READ(cpu->hl); cpu->hl++; cycles = 8; break;

        /* DEC HL - Opcode 2B, 8 cycles */
        case 0x2B: cpu->hl--; cycles = 8; break;

        /* INC L - Opcode 2C, 4 cycles */
        case 0x2C: cpu->l = alu_inc(cpu, cpu->l); cycles = 4; break;

        /* DEC L - Opcode 2D, 4 cycles */
        case 0x2D: cpu->l = alu_dec(cpu, cpu->l); cycles = 4; break;

        /* LD L,n - Opcode 2E, 8 cycles */
        case 0x2E: cpu->l = FETCH(); cycles = 8; break;

        /* CPL - Section 3.3.5.3: Opcode 2F, 4 cycles */
        case 0x2F:
            cpu->a = ~cpu->a;
            SET_FLAG(FLAG_N);
            SET_FLAG(FLAG_H);
            cycles = 4;
            break;

        /* ===== 0x30-0x3F ===== */

        /* JR NC,n - Opcode 30, 12/8 cycles */
        case 0x30:
            stmp8 = (int8_t)FETCH();
            if (!CHECK_FLAG(FLAG_C)) { cpu->pc += stmp8; cycles = 12; }
            else { cycles = 8; }
            break;

        /* LD SP,nn - Opcode 31, 12 cycles */
        case 0x31: cpu->sp = fetch16(cpu); cycles = 12; break;

        /* LDD (HL),A - Section 3.3.1.12: Opcode 32, 8 cycles */
        case 0x32: WRITE(cpu->hl, cpu->a); cpu->hl--; cycles = 8; break;

        /* INC SP - Opcode 33, 8 cycles */
        case 0x33: cpu->sp++; cycles = 8; break;

        /* INC (HL) - Opcode 34, 12 cycles */
        case 0x34:
            tmp8 = READ(cpu->hl);
            WRITE(cpu->hl, alu_inc(cpu, tmp8));
            cycles = 12;
            break;

        /* DEC (HL) - Opcode 35, 12 cycles */
        case 0x35:
            tmp8 = READ(cpu->hl);
            WRITE(cpu->hl, alu_dec(cpu, tmp8));
            cycles = 12;
            break;

        /* LD (HL),n - Opcode 36, 12 cycles */
        case 0x36: WRITE(cpu->hl, FETCH()); cycles = 12; break;

        /* SCF - Section 3.3.5.5: Opcode 37, 4 cycles */
        case 0x37:
            CLEAR_FLAG(FLAG_N);
            CLEAR_FLAG(FLAG_H);
            SET_FLAG(FLAG_C);
            cycles = 4;
            break;

        /* JR C,n - Opcode 38, 12/8 cycles */
        case 0x38:
            stmp8 = (int8_t)FETCH();
            if (CHECK_FLAG(FLAG_C)) { cpu->pc += stmp8; cycles = 12; }
            else { cycles = 8; }
            break;

        /* ADD HL,SP - Opcode 39, 8 cycles */
        case 0x39:
            CLEAR_FLAG(FLAG_N);
            set_flag_cond(cpu, FLAG_H, ((cpu->hl & 0x0FFF) + (cpu->sp & 0x0FFF)) > 0x0FFF);
            set_flag_cond(cpu, FLAG_C, ((uint32_t)cpu->hl + cpu->sp) > 0xFFFF);
            cpu->hl += cpu->sp;
            cycles = 8;
            break;

        /* LDD A,(HL) - Section 3.3.1.9: Opcode 3A, 8 cycles */
        case 0x3A: cpu->a = READ(cpu->hl); cpu->hl--; cycles = 8; break;

        /* DEC SP - Opcode 3B, 8 cycles */
        case 0x3B: cpu->sp--; cycles = 8; break;

        /* INC A - Opcode 3C, 4 cycles */
        case 0x3C: cpu->a = alu_inc(cpu, cpu->a); cycles = 4; break;

        /* DEC A - Opcode 3D, 4 cycles */
        case 0x3D: cpu->a = alu_dec(cpu, cpu->a); cycles = 4; break;

        /* LD A,n - Opcode 3E, 8 cycles */
        case 0x3E: cpu->a = FETCH(); cycles = 8; break;

        /* CCF - Section 3.3.5.4: Opcode 3F, 4 cycles */
        case 0x3F:
            CLEAR_FLAG(FLAG_N);
            CLEAR_FLAG(FLAG_H);
            set_flag_cond(cpu, FLAG_C, !CHECK_FLAG(FLAG_C));
            cycles = 4;
            break;

        /* ===== 0x40-0x7F: LD r1,r2 (Section 3.3.1.2) ===== */
        /* All LD r,r opcodes are 4 cycles, LD r,(HL) is 8, LD (HL),r is 8 */
        case 0x40: cpu->b = cpu->b; cycles = 4; break;
        case 0x41: cpu->b = cpu->c; cycles = 4; break;
        case 0x42: cpu->b = cpu->d; cycles = 4; break;
        case 0x43: cpu->b = cpu->e; cycles = 4; break;
        case 0x44: cpu->b = cpu->h; cycles = 4; break;
        case 0x45: cpu->b = cpu->l; cycles = 4; break;
        case 0x46: cpu->b = READ(cpu->hl); cycles = 8; break;
        case 0x47: cpu->b = cpu->a; cycles = 4; break;

        case 0x48: cpu->c = cpu->b; cycles = 4; break;
        case 0x49: cpu->c = cpu->c; cycles = 4; break;
        case 0x4A: cpu->c = cpu->d; cycles = 4; break;
        case 0x4B: cpu->c = cpu->e; cycles = 4; break;
        case 0x4C: cpu->c = cpu->h; cycles = 4; break;
        case 0x4D: cpu->c = cpu->l; cycles = 4; break;
        case 0x4E: cpu->c = READ(cpu->hl); cycles = 8; break;
        case 0x4F: cpu->c = cpu->a; cycles = 4; break;

        case 0x50: cpu->d = cpu->b; cycles = 4; break;
        case 0x51: cpu->d = cpu->c; cycles = 4; break;
        case 0x52: cpu->d = cpu->d; cycles = 4; break;
        case 0x53: cpu->d = cpu->e; cycles = 4; break;
        case 0x54: cpu->d = cpu->h; cycles = 4; break;
        case 0x55: cpu->d = cpu->l; cycles = 4; break;
        case 0x56: cpu->d = READ(cpu->hl); cycles = 8; break;
        case 0x57: cpu->d = cpu->a; cycles = 4; break;

        case 0x58: cpu->e = cpu->b; cycles = 4; break;
        case 0x59: cpu->e = cpu->c; cycles = 4; break;
        case 0x5A: cpu->e = cpu->d; cycles = 4; break;
        case 0x5B: cpu->e = cpu->e; cycles = 4; break;
        case 0x5C: cpu->e = cpu->h; cycles = 4; break;
        case 0x5D: cpu->e = cpu->l; cycles = 4; break;
        case 0x5E: cpu->e = READ(cpu->hl); cycles = 8; break;
        case 0x5F: cpu->e = cpu->a; cycles = 4; break;

        case 0x60: cpu->h = cpu->b; cycles = 4; break;
        case 0x61: cpu->h = cpu->c; cycles = 4; break;
        case 0x62: cpu->h = cpu->d; cycles = 4; break;
        case 0x63: cpu->h = cpu->e; cycles = 4; break;
        case 0x64: cpu->h = cpu->h; cycles = 4; break;
        case 0x65: cpu->h = cpu->l; cycles = 4; break;
        case 0x66: cpu->h = READ(cpu->hl); cycles = 8; break;
        case 0x67: cpu->h = cpu->a; cycles = 4; break;

        case 0x68: cpu->l = cpu->b; cycles = 4; break;
        case 0x69: cpu->l = cpu->c; cycles = 4; break;
        case 0x6A: cpu->l = cpu->d; cycles = 4; break;
        case 0x6B: cpu->l = cpu->e; cycles = 4; break;
        case 0x6C: cpu->l = cpu->h; cycles = 4; break;
        case 0x6D: cpu->l = cpu->l; cycles = 4; break;
        case 0x6E: cpu->l = READ(cpu->hl); cycles = 8; break;
        case 0x6F: cpu->l = cpu->a; cycles = 4; break;

        case 0x70: WRITE(cpu->hl, cpu->b); cycles = 8; break;
        case 0x71: WRITE(cpu->hl, cpu->c); cycles = 8; break;
        case 0x72: WRITE(cpu->hl, cpu->d); cycles = 8; break;
        case 0x73: WRITE(cpu->hl, cpu->e); cycles = 8; break;
        case 0x74: WRITE(cpu->hl, cpu->h); cycles = 8; break;
        case 0x75: WRITE(cpu->hl, cpu->l); cycles = 8; break;

        /* HALT - Section 3.3.5.7: Opcode 76, 4 cycles */
        case 0x76:
            if (cpu->ime) {
                cpu->halted = true;
            } else {
                /* HALT bug (Section 2.7.3): next instruction byte is read twice */
                uint8_t ie = READ(0xFFFF);
                uint8_t if_reg = READ(0xFF0F);
                if (ie & if_reg & 0x1F) {
                    cpu->halt_bug = true;
                } else {
                    cpu->halted = true;
                }
            }
            cycles = 4;
            break;

        case 0x77: WRITE(cpu->hl, cpu->a); cycles = 8; break;

        case 0x78: cpu->a = cpu->b; cycles = 4; break;
        case 0x79: cpu->a = cpu->c; cycles = 4; break;
        case 0x7A: cpu->a = cpu->d; cycles = 4; break;
        case 0x7B: cpu->a = cpu->e; cycles = 4; break;
        case 0x7C: cpu->a = cpu->h; cycles = 4; break;
        case 0x7D: cpu->a = cpu->l; cycles = 4; break;
        case 0x7E: cpu->a = READ(cpu->hl); cycles = 8; break;
        case 0x7F: cpu->a = cpu->a; cycles = 4; break;

        /* ===== 0x80-0xBF: ALU operations (Section 3.3.3) ===== */

        /* ADD A,r */
        case 0x80: alu_add(cpu, cpu->b); cycles = 4; break;
        case 0x81: alu_add(cpu, cpu->c); cycles = 4; break;
        case 0x82: alu_add(cpu, cpu->d); cycles = 4; break;
        case 0x83: alu_add(cpu, cpu->e); cycles = 4; break;
        case 0x84: alu_add(cpu, cpu->h); cycles = 4; break;
        case 0x85: alu_add(cpu, cpu->l); cycles = 4; break;
        case 0x86: alu_add(cpu, READ(cpu->hl)); cycles = 8; break;
        case 0x87: alu_add(cpu, cpu->a); cycles = 4; break;

        /* ADC A,r */
        case 0x88: alu_adc(cpu, cpu->b); cycles = 4; break;
        case 0x89: alu_adc(cpu, cpu->c); cycles = 4; break;
        case 0x8A: alu_adc(cpu, cpu->d); cycles = 4; break;
        case 0x8B: alu_adc(cpu, cpu->e); cycles = 4; break;
        case 0x8C: alu_adc(cpu, cpu->h); cycles = 4; break;
        case 0x8D: alu_adc(cpu, cpu->l); cycles = 4; break;
        case 0x8E: alu_adc(cpu, READ(cpu->hl)); cycles = 8; break;
        case 0x8F: alu_adc(cpu, cpu->a); cycles = 4; break;

        /* SUB r */
        case 0x90: alu_sub(cpu, cpu->b); cycles = 4; break;
        case 0x91: alu_sub(cpu, cpu->c); cycles = 4; break;
        case 0x92: alu_sub(cpu, cpu->d); cycles = 4; break;
        case 0x93: alu_sub(cpu, cpu->e); cycles = 4; break;
        case 0x94: alu_sub(cpu, cpu->h); cycles = 4; break;
        case 0x95: alu_sub(cpu, cpu->l); cycles = 4; break;
        case 0x96: alu_sub(cpu, READ(cpu->hl)); cycles = 8; break;
        case 0x97: alu_sub(cpu, cpu->a); cycles = 4; break;

        /* SBC A,r */
        case 0x98: alu_sbc(cpu, cpu->b); cycles = 4; break;
        case 0x99: alu_sbc(cpu, cpu->c); cycles = 4; break;
        case 0x9A: alu_sbc(cpu, cpu->d); cycles = 4; break;
        case 0x9B: alu_sbc(cpu, cpu->e); cycles = 4; break;
        case 0x9C: alu_sbc(cpu, cpu->h); cycles = 4; break;
        case 0x9D: alu_sbc(cpu, cpu->l); cycles = 4; break;
        case 0x9E: alu_sbc(cpu, READ(cpu->hl)); cycles = 8; break;
        case 0x9F: alu_sbc(cpu, cpu->a); cycles = 4; break;

        /* AND r */
        case 0xA0: alu_and(cpu, cpu->b); cycles = 4; break;
        case 0xA1: alu_and(cpu, cpu->c); cycles = 4; break;
        case 0xA2: alu_and(cpu, cpu->d); cycles = 4; break;
        case 0xA3: alu_and(cpu, cpu->e); cycles = 4; break;
        case 0xA4: alu_and(cpu, cpu->h); cycles = 4; break;
        case 0xA5: alu_and(cpu, cpu->l); cycles = 4; break;
        case 0xA6: alu_and(cpu, READ(cpu->hl)); cycles = 8; break;
        case 0xA7: alu_and(cpu, cpu->a); cycles = 4; break;

        /* XOR r */
        case 0xA8: alu_xor(cpu, cpu->b); cycles = 4; break;
        case 0xA9: alu_xor(cpu, cpu->c); cycles = 4; break;
        case 0xAA: alu_xor(cpu, cpu->d); cycles = 4; break;
        case 0xAB: alu_xor(cpu, cpu->e); cycles = 4; break;
        case 0xAC: alu_xor(cpu, cpu->h); cycles = 4; break;
        case 0xAD: alu_xor(cpu, cpu->l); cycles = 4; break;
        case 0xAE: alu_xor(cpu, READ(cpu->hl)); cycles = 8; break;
        case 0xAF: alu_xor(cpu, cpu->a); cycles = 4; break;

        /* OR r */
        case 0xB0: alu_or(cpu, cpu->b); cycles = 4; break;
        case 0xB1: alu_or(cpu, cpu->c); cycles = 4; break;
        case 0xB2: alu_or(cpu, cpu->d); cycles = 4; break;
        case 0xB3: alu_or(cpu, cpu->e); cycles = 4; break;
        case 0xB4: alu_or(cpu, cpu->h); cycles = 4; break;
        case 0xB5: alu_or(cpu, cpu->l); cycles = 4; break;
        case 0xB6: alu_or(cpu, READ(cpu->hl)); cycles = 8; break;
        case 0xB7: alu_or(cpu, cpu->a); cycles = 4; break;

        /* CP r */
        case 0xB8: alu_cp(cpu, cpu->b); cycles = 4; break;
        case 0xB9: alu_cp(cpu, cpu->c); cycles = 4; break;
        case 0xBA: alu_cp(cpu, cpu->d); cycles = 4; break;
        case 0xBB: alu_cp(cpu, cpu->e); cycles = 4; break;
        case 0xBC: alu_cp(cpu, cpu->h); cycles = 4; break;
        case 0xBD: alu_cp(cpu, cpu->l); cycles = 4; break;
        case 0xBE: alu_cp(cpu, READ(cpu->hl)); cycles = 8; break;
        case 0xBF: alu_cp(cpu, cpu->a); cycles = 4; break;

        /* ===== 0xC0-0xFF ===== */

        /* RET NZ - Section 3.3.11.2: Opcode C0, 20/8 cycles */
        case 0xC0:
            if (!CHECK_FLAG(FLAG_Z)) { cpu->pc = pop16(cpu); cycles = 20; }
            else { cycles = 8; }
            break;

        /* POP BC - Section 3.3.2.7: Opcode C1, 12 cycles */
        case 0xC1: cpu->bc = pop16(cpu); cycles = 12; break;

        /* JP NZ,nn - Section 3.3.8.2: Opcode C2, 16/12 cycles */
        case 0xC2:
            tmp16 = fetch16(cpu);
            if (!CHECK_FLAG(FLAG_Z)) { cpu->pc = tmp16; cycles = 16; }
            else { cycles = 12; }
            break;

        /* JP nn - Section 3.3.8.1: Opcode C3, 16 cycles */
        case 0xC3: cpu->pc = fetch16(cpu); cycles = 16; break;

        /* CALL NZ,nn - Section 3.3.9.2: Opcode C4, 24/12 cycles */
        case 0xC4:
            tmp16 = fetch16(cpu);
            if (!CHECK_FLAG(FLAG_Z)) { push16(cpu, cpu->pc); cpu->pc = tmp16; cycles = 24; }
            else { cycles = 12; }
            break;

        /* PUSH BC - Section 3.3.2.6: Opcode C5, 16 cycles */
        case 0xC5: push16(cpu, cpu->bc); cycles = 16; break;

        /* ADD A,# - Opcode C6, 8 cycles */
        case 0xC6: alu_add(cpu, FETCH()); cycles = 8; break;

        /* RST 00H - Section 3.3.10.1: Opcode C7, 16 cycles */
        case 0xC7: push16(cpu, cpu->pc); cpu->pc = 0x0000; cycles = 16; break;

        /* RET Z - Opcode C8, 20/8 cycles */
        case 0xC8:
            if (CHECK_FLAG(FLAG_Z)) { cpu->pc = pop16(cpu); cycles = 20; }
            else { cycles = 8; }
            break;

        /* RET - Section 3.3.11.1: Opcode C9, 16 cycles */
        case 0xC9: cpu->pc = pop16(cpu); cycles = 16; break;

        /* JP Z,nn - Opcode CA, 16/12 cycles */
        case 0xCA:
            tmp16 = fetch16(cpu);
            if (CHECK_FLAG(FLAG_Z)) { cpu->pc = tmp16; cycles = 16; }
            else { cycles = 12; }
            break;

        /* CB prefix - extended opcodes */
        case 0xCB: cycles = sm83_execute_cb(cpu); break;

        /* CALL Z,nn - Opcode CC, 24/12 cycles */
        case 0xCC:
            tmp16 = fetch16(cpu);
            if (CHECK_FLAG(FLAG_Z)) { push16(cpu, cpu->pc); cpu->pc = tmp16; cycles = 24; }
            else { cycles = 12; }
            break;

        /* CALL nn - Section 3.3.9.1: Opcode CD, 24 cycles */
        case 0xCD:
            tmp16 = fetch16(cpu);
            push16(cpu, cpu->pc);
            cpu->pc = tmp16;
            cycles = 24;
            break;

        /* ADC A,# - Opcode CE, 8 cycles */
        case 0xCE: alu_adc(cpu, FETCH()); cycles = 8; break;

        /* RST 08H - Opcode CF, 16 cycles */
        case 0xCF: push16(cpu, cpu->pc); cpu->pc = 0x0008; cycles = 16; break;

        /* RET NC - Opcode D0, 20/8 cycles */
        case 0xD0:
            if (!CHECK_FLAG(FLAG_C)) { cpu->pc = pop16(cpu); cycles = 20; }
            else { cycles = 8; }
            break;

        /* POP DE - Opcode D1, 12 cycles */
        case 0xD1: cpu->de = pop16(cpu); cycles = 12; break;

        /* JP NC,nn - Opcode D2, 16/12 cycles */
        case 0xD2:
            tmp16 = fetch16(cpu);
            if (!CHECK_FLAG(FLAG_C)) { cpu->pc = tmp16; cycles = 16; }
            else { cycles = 12; }
            break;

        /* 0xD3 - undefined */
        case 0xD3: cycles = 4; break;

        /* CALL NC,nn - Opcode D4, 24/12 cycles */
        case 0xD4:
            tmp16 = fetch16(cpu);
            if (!CHECK_FLAG(FLAG_C)) { push16(cpu, cpu->pc); cpu->pc = tmp16; cycles = 24; }
            else { cycles = 12; }
            break;

        /* PUSH DE - Opcode D5, 16 cycles */
        case 0xD5: push16(cpu, cpu->de); cycles = 16; break;

        /* SUB # - Opcode D6, 8 cycles */
        case 0xD6: alu_sub(cpu, FETCH()); cycles = 8; break;

        /* RST 10H - Opcode D7, 16 cycles */
        case 0xD7: push16(cpu, cpu->pc); cpu->pc = 0x0010; cycles = 16; break;

        /* RET C - Opcode D8, 20/8 cycles */
        case 0xD8:
            if (CHECK_FLAG(FLAG_C)) { cpu->pc = pop16(cpu); cycles = 20; }
            else { cycles = 8; }
            break;

        /* RETI - Section 3.3.11.3: Opcode D9, 16 cycles */
        case 0xD9:
            cpu->pc = pop16(cpu);
            cpu->ime = true;
            cycles = 16;
            break;

        /* JP C,nn - Opcode DA, 16/12 cycles */
        case 0xDA:
            tmp16 = fetch16(cpu);
            if (CHECK_FLAG(FLAG_C)) { cpu->pc = tmp16; cycles = 16; }
            else { cycles = 12; }
            break;

        /* 0xDB - undefined */
        case 0xDB: cycles = 4; break;

        /* CALL C,nn - Opcode DC, 24/12 cycles */
        case 0xDC:
            tmp16 = fetch16(cpu);
            if (CHECK_FLAG(FLAG_C)) { push16(cpu, cpu->pc); cpu->pc = tmp16; cycles = 24; }
            else { cycles = 12; }
            break;

        /* 0xDD - undefined */
        case 0xDD: cycles = 4; break;

        /* SBC A,# - Opcode DE, 8 cycles */
        case 0xDE: alu_sbc(cpu, FETCH()); cycles = 8; break;

        /* RST 18H - Opcode DF, 16 cycles */
        case 0xDF: push16(cpu, cpu->pc); cpu->pc = 0x0018; cycles = 16; break;

        /* LDH (n),A - Section 3.3.1.19: Opcode E0, 12 cycles */
        case 0xE0: WRITE(0xFF00 + FETCH(), cpu->a); cycles = 12; break;

        /* POP HL - Opcode E1, 12 cycles */
        case 0xE1: cpu->hl = pop16(cpu); cycles = 12; break;

        /* LD (C),A - Section 3.3.1.6: Opcode E2, 8 cycles */
        case 0xE2: WRITE(0xFF00 + cpu->c, cpu->a); cycles = 8; break;

        /* 0xE3, 0xE4 - undefined */
        case 0xE3: cycles = 4; break;
        case 0xE4: cycles = 4; break;

        /* PUSH HL - Opcode E5, 16 cycles */
        case 0xE5: push16(cpu, cpu->hl); cycles = 16; break;

        /* AND # - Opcode E6, 8 cycles */
        case 0xE6: alu_and(cpu, FETCH()); cycles = 8; break;

        /* RST 20H - Opcode E7, 16 cycles */
        case 0xE7: push16(cpu, cpu->pc); cpu->pc = 0x0020; cycles = 16; break;

        /* ADD SP,n - Section 3.3.4.2: Opcode E8, 16 cycles */
        case 0xE8: {
            int8_t offset = (int8_t)FETCH();
            uint16_t result = (uint16_t)(cpu->sp + offset);
            cpu->f = 0;
            set_flag_cond(cpu, FLAG_H, ((cpu->sp & 0x0F) + (offset & 0x0F)) > 0x0F);
            set_flag_cond(cpu, FLAG_C, ((cpu->sp & 0xFF) + (uint8_t)offset) > 0xFF);
            cpu->sp = result;
            cycles = 16;
            break;
        }

        /* JP (HL) - Section 3.3.8.3: Opcode E9, 4 cycles */
        case 0xE9: cpu->pc = cpu->hl; cycles = 4; break;

        /* LD (nn),A - Section 3.3.1.4: Opcode EA, 16 cycles */
        case 0xEA: WRITE(fetch16(cpu), cpu->a); cycles = 16; break;

        /* 0xEB, 0xEC, 0xED - undefined */
        case 0xEB: cycles = 4; break;
        case 0xEC: cycles = 4; break;
        case 0xED: cycles = 4; break;

        /* XOR # - Opcode EE, 8 cycles */
        case 0xEE: alu_xor(cpu, FETCH()); cycles = 8; break;

        /* RST 28H - Opcode EF, 16 cycles */
        case 0xEF: push16(cpu, cpu->pc); cpu->pc = 0x0028; cycles = 16; break;

        /* LDH A,(n) - Section 3.3.1.20: Opcode F0, 12 cycles */
        case 0xF0: cpu->a = READ(0xFF00 + FETCH()); cycles = 12; break;

        /* POP AF - Opcode F1, 12 cycles (lower nibble of F is always 0) */
        case 0xF1: cpu->af = pop16(cpu) & 0xFFF0; cycles = 12; break;

        /* LD A,(C) - Section 3.3.1.5: Opcode F2, 8 cycles */
        case 0xF2: cpu->a = READ(0xFF00 + cpu->c); cycles = 8; break;

        /* DI - Section 3.3.5.9: Opcode F3, 4 cycles */
        case 0xF3:
            cpu->ime = false;
            cpu->ime_pending = false;
            cycles = 4;
            break;

        /* 0xF4 - undefined */
        case 0xF4: cycles = 4; break;

        /* PUSH AF - Opcode F5, 16 cycles */
        case 0xF5: push16(cpu, cpu->af); cycles = 16; break;

        /* OR # - Opcode F6, 8 cycles */
        case 0xF6: alu_or(cpu, FETCH()); cycles = 8; break;

        /* RST 30H - Opcode F7, 16 cycles */
        case 0xF7: push16(cpu, cpu->pc); cpu->pc = 0x0030; cycles = 16; break;

        /* LDHL SP,n - Section 3.3.2.4: Opcode F8, 12 cycles */
        case 0xF8: {
            int8_t offset = (int8_t)FETCH();
            uint16_t result = (uint16_t)(cpu->sp + offset);
            cpu->f = 0;
            set_flag_cond(cpu, FLAG_H, ((cpu->sp & 0x0F) + (offset & 0x0F)) > 0x0F);
            set_flag_cond(cpu, FLAG_C, ((cpu->sp & 0xFF) + (uint8_t)offset) > 0xFF);
            cpu->hl = result;
            cycles = 12;
            break;
        }

        /* LD SP,HL - Section 3.3.2.2: Opcode F9, 8 cycles */
        case 0xF9: cpu->sp = cpu->hl; cycles = 8; break;

        /* LD A,(nn) - Section 3.3.1.3: Opcode FA, 16 cycles */
        case 0xFA: cpu->a = READ(fetch16(cpu)); cycles = 16; break;

        /* EI - Section 3.3.5.10: Opcode FB, 4 cycles */
        case 0xFB:
            cpu->ime_pending = true;
            cycles = 4;
            break;

        /* 0xFC, 0xFD - undefined */
        case 0xFC: cycles = 4; break;
        case 0xFD: cycles = 4; break;

        /* CP # - Opcode FE, 8 cycles */
        case 0xFE: alu_cp(cpu, FETCH()); cycles = 8; break;

        /* RST 38H - Opcode FF, 16 cycles */
        case 0xFF: push16(cpu, cpu->pc); cpu->pc = 0x0038; cycles = 16; break;
    }

    cpu->cycles = cycles;
    cpu->total_cycles += cycles;
    return cycles;
}

/* ---------- Accessor functions ---------- */

uint16_t sm83_get_af(const SM83 *cpu) { return cpu->af; }
uint16_t sm83_get_bc(const SM83 *cpu) { return cpu->bc; }
uint16_t sm83_get_de(const SM83 *cpu) { return cpu->de; }
uint16_t sm83_get_hl(const SM83 *cpu) { return cpu->hl; }

/* ---------- State serialization ---------- */

size_t sm83_serialize(const SM83 *cpu, uint8_t *buf, size_t buf_size) {
    size_t needed = sizeof(SM83) - sizeof(sm83_read_fn) - sizeof(sm83_write_fn) - sizeof(void *);
    if (!buf || buf_size < needed) return needed;

    size_t pos = 0;
    #define SAVE(field) memcpy(buf + pos, &cpu->field, sizeof(cpu->field)); pos += sizeof(cpu->field)
    SAVE(af); SAVE(bc); SAVE(de); SAVE(hl);
    SAVE(sp); SAVE(pc);
    SAVE(ime); SAVE(ime_pending);
    SAVE(halted); SAVE(stopped); SAVE(halt_bug); SAVE(double_speed);
    SAVE(cycles); SAVE(total_cycles);
    #undef SAVE
    return pos;
}

size_t sm83_deserialize(SM83 *cpu, const uint8_t *buf, size_t buf_size) {
    size_t pos = 0;
    #define LOAD(field) do { \
        if (pos + sizeof(cpu->field) > buf_size) return 0; \
        memcpy(&cpu->field, buf + pos, sizeof(cpu->field)); \
        pos += sizeof(cpu->field); \
    } while(0)
    LOAD(af); LOAD(bc); LOAD(de); LOAD(hl);
    LOAD(sp); LOAD(pc);
    LOAD(ime); LOAD(ime_pending);
    LOAD(halted); LOAD(stopped); LOAD(halt_bug); LOAD(double_speed);
    LOAD(cycles); LOAD(total_cycles);
    #undef LOAD
    return pos;
}
