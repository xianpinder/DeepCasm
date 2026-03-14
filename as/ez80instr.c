/*
 * eZ80 ADL Mode Assembler - Instruction Handlers
 * 
 * Part 2: All eZ80 instruction encoding in ADL mode
 * C89 compatible, 24-bit integers
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "ez80asm.h"

/* Forward declare handler type for unified instruction table */
typedef int (*InstrHandler)(AsmState *as);


/* Helper: get 8-bit register encoding (for r field) */
static int get_reg8_code(int reg)
{
    switch (reg) {
        case REG_B: return 0;
        case REG_C: return 1;
        case REG_D: return 2;
        case REG_E: return 3;
        case REG_H: return 4;
        case REG_L: return 5;
        case REG_A: return 7;
        /* IXH/IXL/IYH/IYL use same codes as H/L */
        case REG_IXH: return 4;
        case REG_IXL: return 5;
        case REG_IYH: return 4;
        case REG_IYL: return 5;
        default: return -1;
    }
}

/* Check if register is IXH or IXL */
static int is_ix_half(int reg)
{
    return (reg == REG_IXH || reg == REG_IXL);
}

/* Check if register is IYH or IYL */
static int is_iy_half(int reg)
{
    return (reg == REG_IYH || reg == REG_IYL);
}

/* Emit prefix for index half registers, return 1 if prefix emitted */
static int emit_index_prefix_if_needed(AsmState *as, int reg1, int reg2)
{
    /* Check for conflicting prefixes (can't mix IX and IY halves) */
    if ((is_ix_half(reg1) && is_iy_half(reg2)) ||
        (is_iy_half(reg1) && is_ix_half(reg2))) {
        return -1;  /* Error: conflicting prefixes */
    }
    
    if (is_ix_half(reg1) || is_ix_half(reg2)) {
        emit_byte(as, 0xDD);
        return 1;
    }
    if (is_iy_half(reg1) || is_iy_half(reg2)) {
        emit_byte(as, 0xFD);
        return 1;
    }
    return 0;  /* No prefix needed */
}

/* Helper: get 16-bit register pair encoding (for dd field) */
static int get_reg16_dd_code(int reg)
{
    switch (reg) {
        case REG_BC: return 0;
        case REG_DE: return 1;
        case REG_HL: return 2;
        case REG_SP: return 3;
        default: return -1;
    }
}

/* Helper: get 16-bit register pair encoding (for qq field - push/pop) */
static int get_reg16_qq_code(int reg)
{
    switch (reg) {
        case REG_BC: return 0;
        case REG_DE: return 1;
        case REG_HL: return 2;
        case REG_AF: return 3;
        default: return -1;
    }
}

/* Helper: emit DD or FD prefix based on register (IX or IY) */
static void emit_idx_reg_prefix(AsmState *as, int reg)
{
    emit_byte(as, reg == REG_IX ? 0xDD : 0xFD);
}

/* Helper: emit DD or FD prefix based on operand type (IX+d or IY+d) */
static void emit_idx_off_prefix(AsmState *as, int op_type)
{
    emit_byte(as, op_type == OP_IX_OFF ? 0xDD : 0xFD);
}

/* Helper: emit DD or FD prefix for indirect index register (IND_IX or IND_IY) */
static void emit_ind_idx_prefix(AsmState *as, int ind_reg)
{
    emit_byte(as, ind_reg == REG_IND_IX ? 0xDD : 0xFD);
}

/* Helper: get 16-bit index pair encoding for ADD IX/IY, pp
 * Returns pair code (0-3), or -1 if invalid.
 * For ADD IX,rr: BC=0 DE=1 IX=2 SP=3
 * For ADD IY,rr: BC=0 DE=1 IY=2 SP=3 */
static int get_idx_pair_code(int dest_reg, int src_reg)
{
    switch (src_reg) {
        case REG_BC: return 0;
        case REG_DE: return 1;
        case REG_SP: return 3;
        default:
            if (src_reg == dest_reg) return 2;  /* IX,IX or IY,IY */
            return -1;
    }
}

/* Helper: resolve condition code from operand, handling C register ambiguity.
 * Returns condition code (0-7), or -1 if not a condition. */
static int get_condition_code(Operand *op)
{
    if (op->type == OP_COND) return op->cc;
    if (op->type == OP_REG && op->reg == REG_C) return CC_C;
    return -1;
}


/* ============================================================
 * IM - Interrupt Mode
 * ============================================================ */

static int handle_im(AsmState *as)
{
    Operand op;
    
    lexer_next(as);
    if (parse_operand(as, &op) < 0) return -1;
    
    if (op.type != OP_IMM) {
        asm_error(as, "IM requires immediate operand");
        return -1;
    }
    
    emit_byte(as, 0xED);
    switch (op.value) {
        case 0: emit_byte(as, 0x46); break;
        case 1: emit_byte(as, 0x56); break;
        case 2: emit_byte(as, 0x5E); break;
        default:
            asm_error(as, "invalid interrupt mode");
            return -1;
    }
    
    return 0;
}

/* ============================================================
 * LD - Load Instructions
 * ============================================================ */

/* Table A: Special register-to-register pairs that encode as
 * a fixed prefix+opcode with no further operand logic. */
typedef struct {
    int dest_reg;
    int src_reg;
    uint8 prefix;   /* 0x00 = none, 0xDD/0xFD/0xED */
    uint8 opcode;
} LdSpecialPair;

static const LdSpecialPair ld_special_pairs[] = {
    {REG_SP, REG_HL, 0x00, 0xF9},
    {REG_SP, REG_IX, 0xDD, 0xF9},
    {REG_SP, REG_IY, 0xFD, 0xF9},
    {REG_I,  REG_A,  0xED, 0x47},
    {REG_R,  REG_A,  0xED, 0x4F},
    {REG_A,  REG_I,  0xED, 0x57},
    {REG_A,  REG_R,  0xED, 0x5F},
    {REG_A,  REG_MB, 0xED, 0x6E},
    {REG_MB, REG_A,  0xED, 0x6D}
};
#define NUM_LD_SPECIAL (sizeof(ld_special_pairs) / sizeof(ld_special_pairs[0]))

/* Table B removed — only two indirect registers (BC/DE), inline is simpler */

/* Table C: eZ80 16-bit register loads/stores via (HL), (IX+d), (IY+d).
 * Each row maps a 16-bit register to its opcode for each base.
 * The prefix byte depends on the base: ED for (HL), DD for (IX+d), FD for (IY+d).
 * BC/DE/HL rows are regular; IX/IY rows have irregular opcodes. */
typedef struct {
    int reg;
    uint8 load_hl,  store_hl;   /* via (HL), ED prefix */
    uint8 load_ix,  store_ix;   /* via (IX+d), DD prefix */
    uint8 load_iy,  store_iy;   /* via (IY+d), FD prefix */
} LdRR16Entry;

static const LdRR16Entry ld_rr16_table[] = {
    {REG_BC, 0x07, 0x0F,  0x07, 0x0F,  0x07, 0x0F},
    {REG_DE, 0x17, 0x1F,  0x17, 0x1F,  0x17, 0x1F},
    {REG_HL, 0x27, 0x2F,  0x27, 0x2F,  0x27, 0x2F},
    {REG_IX, 0x37, 0x3F,  0x37, 0x3E,  0x31, 0x3D},
    {REG_IY, 0x31, 0x3E,  0x31, 0x3D,  0x37, 0x3E}
};
#define NUM_LD_RR16 (sizeof(ld_rr16_table) / sizeof(ld_rr16_table[0]))

/* Find entry in ld_rr16_table, returns NULL if not found */
static const LdRR16Entry *find_ld_rr16(int reg)
{
    int i;
    for (i = 0; i < (int)NUM_LD_RR16; i++) {
        if (ld_rr16_table[i].reg == reg) return &ld_rr16_table[i];
    }
    return NULL;
}

static int handle_ld(AsmState *as)
{
    Operand dest, src;
    const LdRR16Entry *rr16;
    int d, s, dd, i;
    
    lexer_next(as);
    if (parse_operand(as, &dest) < 0) return -1;
    
    if (as->current_token.type != TOK_COMMA) {
        asm_error(as, "expected comma");
        return -1;
    }
    lexer_next(as);
    
    if (parse_operand(as, &src) < 0) return -1;
    
    /* ============ REG, REG ============ */
    if (dest.type == OP_REG && src.type == OP_REG) {
        /* LD r, r' (8-bit, including IXH/IXL/IYH/IYL) */
        d = get_reg8_code(dest.reg);
        s = get_reg8_code(src.reg);
        
        if (d >= 0 && s >= 0) {
            int prefix_result = emit_index_prefix_if_needed(as, dest.reg, src.reg);
            if (prefix_result < 0) {
                asm_error(as, "cannot mix IX and IY half registers");
                return -1;
            }
            if (prefix_result > 0) {
                if ((is_ix_half(dest.reg) || is_iy_half(dest.reg)) &&
                    (src.reg == REG_H || src.reg == REG_L)) {
                    asm_error(as, "cannot use H or L with index half registers");
                    return -1;
                }
                if ((is_ix_half(src.reg) || is_iy_half(src.reg)) &&
                    (dest.reg == REG_H || dest.reg == REG_L)) {
                    asm_error(as, "cannot use H or L with index half registers");
                    return -1;
                }
            }
            emit_byte(as, 0x40 | (d << 3) | s);
            return 0;
        }
        
        /* Special reg,reg pairs: SP<-HL/IX/IY, I/R/MB<->A */
        for (i = 0; i < (int)NUM_LD_SPECIAL; i++) {
            if (dest.reg == ld_special_pairs[i].dest_reg &&
                src.reg == ld_special_pairs[i].src_reg) {
                if (ld_special_pairs[i].prefix)
                    emit_byte(as, ld_special_pairs[i].prefix);
                emit_byte(as, ld_special_pairs[i].opcode);
                return 0;
            }
        }
    }
    
    /* ============ REG, IMM ============ */
    if (dest.type == OP_REG && src.type == OP_IMM) {
        /* LD r, n (8-bit immediate) */
        d = get_reg8_code(dest.reg);
        if (d >= 0) {
            emit_index_prefix_if_needed(as, dest.reg, REG_NONE);
            emit_byte(as, 0x06 | (d << 3));
            emit_byte(as, src.value & 0xFF);
            return 0;
        }
        
        /* LD dd, nn (16-bit immediate, 24-bit in ADL) */
        dd = get_reg16_dd_code(dest.reg);
        if (dd >= 0) {
            emit_byte(as, 0x01 | (dd << 4));
            if (src.has_symbol) emit_reloc(as, RELOC_ADDR24, src.symbol);
            emit_long(as, src.value & 0xFFFFFF);
            return 0;
        }
        
        /* LD IX/IY, nn */
        if (dest.reg == REG_IX || dest.reg == REG_IY) {
            emit_idx_reg_prefix(as, dest.reg);
            emit_byte(as, 0x21);
            if (src.has_symbol) emit_reloc(as, RELOC_ADDR24, src.symbol);
            emit_long(as, src.value & 0xFFFFFF);
            return 0;
        }
    }
    
    /* ============ REG, (HL) ============ */
    if (dest.type == OP_REG && src.type == OP_IND_REG && src.reg == REG_IND_HL) {
        /* eZ80: LD rr, (HL) - 16-bit register pairs */
        rr16 = find_ld_rr16(dest.reg);
        if (rr16) {
            emit_byte(as, 0xED);
            emit_byte(as, rr16->load_hl);
            return 0;
        }
        /* LD r, (HL) - 8-bit */
        d = get_reg8_code(dest.reg);
        if (d >= 0) {
            emit_byte(as, 0x46 | (d << 3));
            return 0;
        }
    }
    
    /* ============ (HL), REG ============ */
    if (dest.type == OP_IND_REG && dest.reg == REG_IND_HL && src.type == OP_REG) {
        /* eZ80: LD (HL), rr - 16-bit register pairs */
        rr16 = find_ld_rr16(src.reg);
        if (rr16) {
            emit_byte(as, 0xED);
            emit_byte(as, rr16->store_hl);
            return 0;
        }
        /* LD (HL), r - 8-bit */
        s = get_reg8_code(src.reg);
        if (s >= 0) {
            emit_byte(as, 0x70 | s);
            return 0;
        }
    }
    
    /* LD (HL), n */
    if (dest.type == OP_IND_REG && dest.reg == REG_IND_HL && src.type == OP_IMM) {
        emit_byte(as, 0x36);
        emit_byte(as, src.value & 0xFF);
        return 0;
    }
    
    /* ============ REG, (IX+d)/(IY+d) ============ */
    if (dest.type == OP_REG &&
        (src.type == OP_IX_OFF || src.type == OP_IY_OFF)) {
        /* LD r, (IX/IY+d) - 8-bit */
        d = get_reg8_code(dest.reg);
        if (d >= 0) {
            emit_idx_off_prefix(as, src.type);
            emit_byte(as, 0x46 | (d << 3));
            emit_byte(as, src.value & 0xFF);
            return 0;
        }
        /* eZ80: LD rr, (IX/IY+d) - 16-bit */
        rr16 = find_ld_rr16(dest.reg);
        if (rr16) {
            emit_idx_off_prefix(as, src.type);
            emit_byte(as, src.type == OP_IX_OFF ? rr16->load_ix : rr16->load_iy);
            emit_byte(as, src.value & 0xFF);
            return 0;
        }
    }
    
    /* ============ (IX+d)/(IY+d), REG ============ */
    if ((dest.type == OP_IX_OFF || dest.type == OP_IY_OFF) &&
        src.type == OP_REG) {
        /* LD (IX/IY+d), r - 8-bit */
        s = get_reg8_code(src.reg);
        if (s >= 0) {
            emit_idx_off_prefix(as, dest.type);
            emit_byte(as, 0x70 | s);
            emit_byte(as, dest.value & 0xFF);
            return 0;
        }
        /* eZ80: LD (IX/IY+d), rr - 16-bit */
        rr16 = find_ld_rr16(src.reg);
        if (rr16) {
            emit_idx_off_prefix(as, dest.type);
            emit_byte(as, dest.type == OP_IX_OFF ? rr16->store_ix : rr16->store_iy);
            emit_byte(as, dest.value & 0xFF);
            return 0;
        }
    }
    
    /* LD (IX/IY+d), n */
    if ((dest.type == OP_IX_OFF || dest.type == OP_IY_OFF) && src.type == OP_IMM) {
        emit_idx_off_prefix(as, dest.type);
        emit_byte(as, 0x36);
        emit_byte(as, dest.value & 0xFF);
        emit_byte(as, src.value & 0xFF);
        return 0;
    }
    
    /* ============ LD A, (BC)/(DE) and LD (BC)/(DE), A ============ */
    if (dest.type == OP_REG && dest.reg == REG_A && src.type == OP_IND_REG) {
        if (src.reg == REG_IND_BC) { emit_byte(as, 0x0A); return 0; }
        if (src.reg == REG_IND_DE) { emit_byte(as, 0x1A); return 0; }
    }
    if (dest.type == OP_IND_REG && src.type == OP_REG && src.reg == REG_A) {
        if (dest.reg == REG_IND_BC) { emit_byte(as, 0x02); return 0; }
        if (dest.reg == REG_IND_DE) { emit_byte(as, 0x12); return 0; }
    }
    
    /* ============ LD A, (nn) ============ */
    if (dest.type == OP_REG && dest.reg == REG_A && src.type == OP_ADDR) {
        emit_byte(as, 0x3A);
        if (src.has_symbol) emit_reloc(as, RELOC_ADDR24, src.symbol);
        emit_long(as, src.value & 0xFFFFFF);
        return 0;
    }
    
    /* ============ LD (nn), A ============ */
    if (dest.type == OP_ADDR && src.type == OP_REG && src.reg == REG_A) {
        emit_byte(as, 0x32);
        if (dest.has_symbol) emit_reloc(as, RELOC_ADDR24, dest.symbol);
        emit_long(as, dest.value & 0xFFFFFF);
        return 0;
    }
    
    /* ============ LD HL, (nn) ============ */
    if (dest.type == OP_REG && dest.reg == REG_HL && src.type == OP_ADDR) {
        emit_byte(as, 0x2A);
        if (src.has_symbol) emit_reloc(as, RELOC_ADDR24, src.symbol);
        emit_long(as, src.value & 0xFFFFFF);
        return 0;
    }
    
    /* ============ LD (nn), HL ============ */
    if (dest.type == OP_ADDR && src.type == OP_REG && src.reg == REG_HL) {
        emit_byte(as, 0x22);
        if (dest.has_symbol) emit_reloc(as, RELOC_ADDR24, dest.symbol);
        emit_long(as, dest.value & 0xFFFFFF);
        return 0;
    }
    
    /* ============ REG, (nn) ============ */
    if (dest.type == OP_REG && src.type == OP_ADDR) {
        /* LD dd, (nn) */
        dd = get_reg16_dd_code(dest.reg);
        if (dd >= 0) {
            emit_byte(as, 0xED);
            emit_byte(as, 0x4B | (dd << 4));
            if (src.has_symbol) emit_reloc(as, RELOC_ADDR24, src.symbol);
            emit_long(as, src.value & 0xFFFFFF);
            return 0;
        }
        /* LD IX/IY, (nn) */
        if (dest.reg == REG_IX || dest.reg == REG_IY) {
            emit_idx_reg_prefix(as, dest.reg);
            emit_byte(as, 0x2A);
            if (src.has_symbol) emit_reloc(as, RELOC_ADDR24, src.symbol);
            emit_long(as, src.value & 0xFFFFFF);
            return 0;
        }
    }
    
    /* ============ (nn), REG ============ */
    if (dest.type == OP_ADDR && src.type == OP_REG) {
        /* LD (nn), dd */
        dd = get_reg16_dd_code(src.reg);
        if (dd >= 0) {
            emit_byte(as, 0xED);
            emit_byte(as, 0x43 | (dd << 4));
            if (dest.has_symbol) emit_reloc(as, RELOC_ADDR24, dest.symbol);
            emit_long(as, dest.value & 0xFFFFFF);
            return 0;
        }
        /* LD (nn), IX/IY */
        if (src.reg == REG_IX || src.reg == REG_IY) {
            emit_idx_reg_prefix(as, src.reg);
            emit_byte(as, 0x22);
            if (dest.has_symbol) emit_reloc(as, RELOC_ADDR24, dest.symbol);
            emit_long(as, dest.value & 0xFFFFFF);
            return 0;
        }
    }
    
    asm_error(as, "invalid operands for LD");
    return -1;
}

/* ============================================================
 * PUSH/POP
 * ============================================================ */

/* Shared handler for PUSH/POP.
 * qq_base: 0xC5 (PUSH) or 0xC1 (POP) - combined with (qq << 4)
 * idx_op:  0xE5 (PUSH) or 0xE1 (POP) - for IX/IY */
static int handle_push_pop(AsmState *as, int qq_base, int idx_op,
                           const char *name)
{
    Operand op;
    int qq;
    
    lexer_next(as);
    if (parse_operand(as, &op) < 0) return -1;
    
    if (op.type != OP_REG) {
        asm_error(as, "%s requires register operand", name);
        return -1;
    }
    
    qq = get_reg16_qq_code(op.reg);
    if (qq >= 0) {
        emit_byte(as, qq_base | (qq << 4));
        return 0;
    }
    
    if (op.reg == REG_IX || op.reg == REG_IY) {
        emit_idx_reg_prefix(as, op.reg);
        emit_byte(as, idx_op);
        return 0;
    }
    
    asm_error(as, "invalid register for %s", name);
    return -1;
}

static int handle_push(AsmState *as) { return handle_push_pop(as, 0xC5, 0xE5, "PUSH"); }
static int handle_pop(AsmState *as)  { return handle_push_pop(as, 0xC1, 0xE1, "POP"); }

/* ============================================================
 * EX - Exchange
 * ============================================================ */

static int handle_ex(AsmState *as)
{
    Operand op1, op2;
    
    lexer_next(as);
    if (parse_operand(as, &op1) < 0) return -1;
    
    if (as->current_token.type != TOK_COMMA) {
        asm_error(as, "expected comma");
        return -1;
    }
    lexer_next(as);
    
    if (parse_operand(as, &op2) < 0) return -1;
    
    /* EX DE, HL */
    if (op1.type == OP_REG && op1.reg == REG_DE &&
        op2.type == OP_REG && op2.reg == REG_HL) {
        emit_byte(as, 0xEB);
        return 0;
    }
    
    /* EX AF, AF' */
    if (op1.type == OP_REG && op1.reg == REG_AF &&
        op2.type == OP_REG && op2.reg == REG_AF_) {
        emit_byte(as, 0x08);
        return 0;
    }
    
    /* EX (SP), HL */
    if (op1.type == OP_IND_REG && op1.reg == REG_IND_SP &&
        op2.type == OP_REG && op2.reg == REG_HL) {
        emit_byte(as, 0xE3);
        return 0;
    }
    
    /* EX (SP), IX/IY */
    if (op1.type == OP_IND_REG && op1.reg == REG_IND_SP &&
        op2.type == OP_REG && (op2.reg == REG_IX || op2.reg == REG_IY)) {
        emit_idx_reg_prefix(as, op2.reg);
        emit_byte(as, 0xE3);
        return 0;
    }
    
    asm_error(as, "invalid operands for EX");
    return -1;
}

/* ============================================================
 * ALU Operations
 * ============================================================ */

static int emit_alu8(AsmState *as, int aluop, Operand *src)
{
    int r;
    
    if (src->type == OP_REG) {
        r = get_reg8_code(src->reg);
        if (r >= 0) {
            /* Emit prefix for index half registers */
            emit_index_prefix_if_needed(as, src->reg, REG_NONE);
            emit_byte(as, 0x80 | (aluop << 3) | r);
            return 0;
        }
    }
    
    if (src->type == OP_IMM) {
        emit_byte(as, 0xC6 | (aluop << 3));
        emit_byte(as, src->value & 0xFF);
        return 0;
    }
    
    if (src->type == OP_IND_REG && src->reg == REG_IND_HL) {
        emit_byte(as, 0x86 | (aluop << 3));
        return 0;
    }
    
    if (src->type == OP_IX_OFF || src->type == OP_IY_OFF) {
        emit_idx_off_prefix(as, src->type);
        emit_byte(as, 0x86 | (aluop << 3));
        emit_byte(as, src->value & 0xFF);
        return 0;
    }
    
    return -1;
}

static int handle_add(AsmState *as)
{
    Operand dest, src;
    int ss, pp;
    
    lexer_next(as);
    if (parse_operand(as, &dest) < 0) return -1;
    
    if (as->current_token.type != TOK_COMMA) {
        if (emit_alu8(as, 0, &dest) == 0) return 0;
        asm_error(as, "invalid operand for ADD");
        return -1;
    }
    
    lexer_next(as);
    if (parse_operand(as, &src) < 0) return -1;
    
    if (dest.type == OP_REG && dest.reg == REG_A) {
        if (emit_alu8(as, 0, &src) == 0) return 0;
    }
    
    /* ADD HL, ss */
    if (dest.type == OP_REG && dest.reg == REG_HL && src.type == OP_REG) {
        ss = get_reg16_dd_code(src.reg);
        if (ss >= 0) {
            emit_byte(as, 0x09 | (ss << 4));
            return 0;
        }
    }
    
    /* ADD IX, pp / IY, rr */
    if (dest.type == OP_REG &&
        (dest.reg == REG_IX || dest.reg == REG_IY) && src.type == OP_REG) {
        pp = get_idx_pair_code(dest.reg, src.reg);
        if (pp >= 0) {
            emit_idx_reg_prefix(as, dest.reg);
            emit_byte(as, 0x09 | (pp << 4));
            return 0;
        }
    }
    
    asm_error(as, "invalid operands for ADD");
    return -1;
}

/* Shared handler for ADC/SBC.
 * aluop:  1 (ADC) or 3 (SBC) - for 8-bit ALU encoding
 * hl_op:  0x4A (ADC) or 0x42 (SBC) - for HL,ss encoding */
static int handle_adc_sbc(AsmState *as, int aluop, int hl_op, const char *name)
{
    Operand dest, src;
    int ss;
    
    lexer_next(as);
    if (parse_operand(as, &dest) < 0) return -1;
    
    if (as->current_token.type != TOK_COMMA) {
        if (emit_alu8(as, aluop, &dest) == 0) return 0;
        asm_error(as, "invalid operand for %s", name);
        return -1;
    }
    
    lexer_next(as);
    if (parse_operand(as, &src) < 0) return -1;
    
    if (dest.type == OP_REG && dest.reg == REG_A) {
        if (emit_alu8(as, aluop, &src) == 0) return 0;
    }
    
    /* ADC/SBC HL, ss */
    if (dest.type == OP_REG && dest.reg == REG_HL && src.type == OP_REG) {
        ss = get_reg16_dd_code(src.reg);
        if (ss >= 0) {
            emit_byte(as, 0xED);
            emit_byte(as, hl_op | (ss << 4));
            return 0;
        }
    }
    
    asm_error(as, "invalid operands for %s", name);
    return -1;
}

static int handle_adc(AsmState *as)   { return handle_adc_sbc(as, 1, 0x4A, "ADC"); }
static int handle_sbc(AsmState *as)   { return handle_adc_sbc(as, 3, 0x42, "SBC"); }

/* Shared handler for simple ALU ops: AND, OR, XOR, CP, SUB
 * All accept optional "A," prefix and delegate to emit_alu8 */
static int handle_alu_simple(AsmState *as, int aluop, const char *name)
{
    Operand src;
    lexer_next(as);
    if (parse_operand(as, &src) < 0) return -1;
    
    /* Handle alternate syntax: op a, n -> op n */
    if (src.type == OP_REG && src.reg == REG_A && 
        as->current_token.type == TOK_COMMA) {
        lexer_next(as);
        if (parse_operand(as, &src) < 0) return -1;
    }
    
    if (emit_alu8(as, aluop, &src) == 0) return 0;
    asm_error(as, "invalid operand for %s", name);
    return -1;
}

static int handle_sub(AsmState *as) { return handle_alu_simple(as, 2, "SUB"); }
static int handle_and(AsmState *as) { return handle_alu_simple(as, 4, "AND"); }
static int handle_xor(AsmState *as) { return handle_alu_simple(as, 5, "XOR"); }
static int handle_or(AsmState *as)  { return handle_alu_simple(as, 6, "OR"); }
static int handle_cp(AsmState *as)  { return handle_alu_simple(as, 7, "CP"); }

/* ============================================================
 * INC/DEC
 * ============================================================ */

/* Shared handler for INC/DEC.
 * r8_base:  0x04 (INC) or 0x05 (DEC) - combined with (r << 3)
 * r16_base: 0x03 (INC) or 0x0B (DEC) - combined with (ss << 4)
 * idx_op:   0x23 (INC) or 0x2B (DEC) - for IX/IY
 * ind_op:   0x34 (INC) or 0x35 (DEC) - for (HL)/(IX+d)/(IY+d) */
static int handle_inc_dec(AsmState *as, int r8_base, int r16_base,
                          int idx_op, int ind_op, const char *name)
{
    Operand op;
    int r, ss;
    
    lexer_next(as);
    if (parse_operand(as, &op) < 0) return -1;
    
    if (op.type == OP_REG) {
        r = get_reg8_code(op.reg);
        if (r >= 0) {
            emit_index_prefix_if_needed(as, op.reg, REG_NONE);
            emit_byte(as, r8_base | (r << 3));
            return 0;
        }
        
        ss = get_reg16_dd_code(op.reg);
        if (ss >= 0) {
            emit_byte(as, r16_base | (ss << 4));
            return 0;
        }
        
        if (op.reg == REG_IX || op.reg == REG_IY) {
            emit_idx_reg_prefix(as, op.reg);
            emit_byte(as, idx_op);
            return 0;
        }
    }
    
    if (op.type == OP_IND_REG && op.reg == REG_IND_HL) {
        emit_byte(as, ind_op);
        return 0;
    }
    
    if (op.type == OP_IX_OFF || op.type == OP_IY_OFF) {
        emit_idx_off_prefix(as, op.type);
        emit_byte(as, ind_op);
        emit_byte(as, op.value & 0xFF);
        return 0;
    }
    
    asm_error(as, "invalid operand for %s", name);
    return -1;
}

static int handle_inc(AsmState *as) { return handle_inc_dec(as, 0x04, 0x03, 0x23, 0x34, "INC"); }
static int handle_dec(AsmState *as) { return handle_inc_dec(as, 0x05, 0x0B, 0x2B, 0x35, "DEC"); }

/* ============================================================
 * Control Flow
 * ============================================================ */

static int handle_jp(AsmState *as)
{
    Operand op, addr;
    int cc;
    
    lexer_next(as);
    if (parse_operand(as, &op) < 0) return -1;
    
    /* JP (HL) */
    if (op.type == OP_IND_REG && op.reg == REG_IND_HL) {
        emit_byte(as, 0xE9);
        return 0;
    }
    
    /* JP (IX) / JP (IY) */
    if (op.type == OP_IND_REG &&
        (op.reg == REG_IND_IX || op.reg == REG_IND_IY)) {
        emit_ind_idx_prefix(as, op.reg);
        emit_byte(as, 0xE9);
        return 0;
    }
    
    /* JP cc, nn - check for condition code or C register (ambiguous) */
    cc = get_condition_code(&op);
    
    if (cc >= 0) {
        if (as->current_token.type != TOK_COMMA) {
            asm_error(as, "expected comma after condition");
            return -1;
        }
        lexer_next(as);
        
        if (parse_operand(as, &addr) < 0) return -1;
        
        if (addr.type != OP_IMM && addr.type != OP_ADDR) {
            asm_error(as, "JP requires address operand");
            return -1;
        }
        
        emit_byte(as, 0xC2 | (cc << 3));
        if (addr.has_symbol) {
            emit_reloc(as, RELOC_ADDR24, addr.symbol);
        }
        emit_long(as, addr.value & 0xFFFFFF);
        return 0;
    }
    
    /* JP nn */
    if (op.type == OP_IMM || op.type == OP_ADDR) {
        emit_byte(as, 0xC3);
        if (op.has_symbol) {
            emit_reloc(as, RELOC_ADDR24, op.symbol);
        }
        emit_long(as, op.value & 0xFFFFFF);
        return 0;
    }
    
    asm_error(as, "invalid operand for JP");
    return -1;
}

static int handle_jr(AsmState *as)
{
    Operand op, addr;
    int24 offset;
    int cc;
    
    lexer_next(as);
    if (parse_operand(as, &op) < 0) return -1;
    
    /* JR cc, e - check for condition code or C register (ambiguous) */
    cc = get_condition_code(&op);
    
    if (cc >= 0) {
        if (cc > CC_C) {
            asm_error(as, "JR only supports NZ, Z, NC, C conditions");
            return -1;
        }
        
        if (as->current_token.type != TOK_COMMA) {
            asm_error(as, "expected comma after condition");
            return -1;
        }
        lexer_next(as);
        
        if (parse_operand(as, &addr) < 0) return -1;
        
        if (addr.has_symbol && symbol_is_extern(as, addr.symbol)) {
            asm_error(as, "JR cannot use external symbols");
            return -1;
        }
        
        emit_byte(as, 0x20 | (cc << 3));
        
        offset = addr.value - (as->pc + 1);
        if (!is_signed_8bit(offset) && as->pass == 2) {
            asm_error(as, "JR offset out of range");
        }
        emit_byte(as, offset & 0xFF);
        return 0;
    }
    
    /* JR e */
    if (op.type == OP_IMM) {
        if (op.has_symbol && symbol_is_extern(as, op.symbol)) {
            asm_error(as, "JR cannot use external symbols");
            return -1;
        }
        
        emit_byte(as, 0x18);
        offset = op.value - (as->pc + 1);
        if (!is_signed_8bit(offset) && as->pass == 2) {
            asm_error(as, "JR offset out of range");
        }
        emit_byte(as, offset & 0xFF);
        return 0;
    }
    
    asm_error(as, "invalid operand for JR");
    return -1;
}

static int handle_djnz(AsmState *as)
{
    Operand op;
    int24 offset;
    
    lexer_next(as);
    if (parse_operand(as, &op) < 0) return -1;
    
    if (op.type != OP_IMM) {
        asm_error(as, "DJNZ requires address operand");
        return -1;
    }
    
    if (op.has_symbol && symbol_is_extern(as, op.symbol)) {
        asm_error(as, "DJNZ cannot use external symbols");
        return -1;
    }
    
    emit_byte(as, 0x10);
    offset = op.value - (as->pc + 1);
    if (!is_signed_8bit(offset) && as->pass == 2) {
        asm_error(as, "DJNZ offset out of range");
    }
    emit_byte(as, offset & 0xFF);
    return 0;
}

static int handle_call(AsmState *as)
{
    Operand op, addr;
    int cc;
    
    lexer_next(as);
    if (parse_operand(as, &op) < 0) return -1;
    
    /* CALL cc, nn - check for condition code or C register (ambiguous) */
    cc = get_condition_code(&op);
    
    if (cc >= 0) {
        if (as->current_token.type != TOK_COMMA) {
            asm_error(as, "expected comma after condition");
            return -1;
        }
        lexer_next(as);
        
        if (parse_operand(as, &addr) < 0) return -1;
        
        emit_byte(as, 0xC4 | (cc << 3));
        if (addr.has_symbol) {
            emit_reloc(as, RELOC_ADDR24, addr.symbol);
        }
        emit_long(as, addr.value & 0xFFFFFF);
        return 0;
    }
    
    /* CALL nn */
    if (op.type == OP_IMM || op.type == OP_ADDR) {
        emit_byte(as, 0xCD);
        if (op.has_symbol) {
            emit_reloc(as, RELOC_ADDR24, op.symbol);
        }
        emit_long(as, op.value & 0xFFFFFF);
        return 0;
    }
    
    asm_error(as, "invalid operand for CALL");
    return -1;
}

static int handle_ret(AsmState *as)
{
    Operand op;
    int cc;
    
    lexer_next(as);
    
    if (as->current_token.type == TOK_EOL || as->current_token.type == TOK_EOF) {
        emit_byte(as, 0xC9);
        return 0;
    }
    
    if (parse_operand(as, &op) < 0) return -1;
    
    /* RET cc - check for condition code or C register (ambiguous) */
    cc = get_condition_code(&op);
    
    if (cc >= 0) {
        emit_byte(as, 0xC0 | (cc << 3));
        return 0;
    }
    
    asm_error(as, "invalid operand for RET");
    return -1;
}

static int parse_rst_vector(AsmState *as, uint8 *vec)
{
    Operand op;
    
    lexer_next(as);
    if (parse_operand(as, &op) < 0) return -1;
    
    if (op.type != OP_IMM) {
        asm_error(as, "RST requires immediate operand");
        return -1;
    }
    
    /* RST accepts 0-7 (shorthand) or 0x00, 0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38 */
    if (op.value >= 0 && op.value <= 7) {
        *vec = op.value << 3;
    } else if ((op.value & 0x07) == 0 && op.value >= 0 && op.value <= 0x38) {
        *vec = op.value;
    } else {
        asm_error(as, "invalid RST vector (use 0-7 or 0x00-0x38)");
        return -1;
    }
    
    return 0;
}

static int handle_rst(AsmState *as)
{
    uint8 vec;
    
    if (parse_rst_vector(as, &vec) < 0) return -1;
    
    emit_byte(as, 0xC7 | vec);
    return 0;
}

/* ============================================================
 * I/O Instructions
 * ============================================================ */

/* Shared handler for IN/OUT.
 * IN A,(n): reg=dest, addr=src, opcode 0xDB
 * OUT (n),A: addr=dest, reg=src, opcode 0xD3
 * IN r,(C): reg=dest, (C)=src, ED 0x40|(r<<3)
 * OUT (C),r: (C)=dest, reg=src, ED 0x41|(r<<3) */
static int handle_in_out(AsmState *as, int is_out, const char *name)
{
    Operand op1, op2;
    Operand *reg_op, *other_op;
    int r;
    
    lexer_next(as);
    if (parse_operand(as, &op1) < 0) return -1;
    
    if (as->current_token.type != TOK_COMMA) {
        asm_error(as, "expected comma");
        return -1;
    }
    lexer_next(as);
    
    if (parse_operand(as, &op2) < 0) return -1;
    
    reg_op   = is_out ? &op2 : &op1;
    other_op = is_out ? &op1 : &op2;
    
    /* IN A,(n) / OUT (n),A */
    if (reg_op->type == OP_REG && reg_op->reg == REG_A &&
        other_op->type == OP_ADDR) {
        emit_byte(as, is_out ? 0xD3 : 0xDB);
        emit_byte(as, other_op->value & 0xFF);
        return 0;
    }
    
    /* IN r,(C) / OUT (C),r */
    if (reg_op->type == OP_REG &&
        other_op->type == OP_IND_REG && other_op->reg == REG_IND_C) {
        r = get_reg8_code(reg_op->reg);
        if (r >= 0) {
            emit_byte(as, 0xED);
            emit_byte(as, 0x40 | (r << 3) | is_out);
            return 0;
        }
    }
    
    asm_error(as, "invalid operands for %s", name);
    return -1;
}

static int handle_in(AsmState *as)  { return handle_in_out(as, 0, "IN"); }
static int handle_out(AsmState *as) { return handle_in_out(as, 1, "OUT"); }

/* Shared handler for IN0/OUT0.
 * IN0 r,(n): reg is first operand, addr is second, opcode = r << 3
 * OUT0 (n),r: addr is first operand, reg is second, opcode = (r << 3) | 1 */
static int handle_in0_out0(AsmState *as, int is_out, const char *name)
{
    Operand op1, op2;
    Operand *reg_op, *addr_op;
    int r;
    
    lexer_next(as);
    if (parse_operand(as, &op1) < 0) return -1;
    
    if (as->current_token.type != TOK_COMMA) {
        asm_error(as, "expected comma");
        return -1;
    }
    lexer_next(as);
    
    if (parse_operand(as, &op2) < 0) return -1;
    
    reg_op  = is_out ? &op2 : &op1;
    addr_op = is_out ? &op1 : &op2;
    
    if (reg_op->type == OP_REG && addr_op->type == OP_ADDR) {
        r = get_reg8_code(reg_op->reg);
        if (r >= 0 && r != 6) {  /* Can't use (HL) */
            emit_byte(as, 0xED);
            emit_byte(as, (r << 3) | is_out);
            emit_byte(as, addr_op->value & 0xFF);
            return 0;
        }
    }
    
    asm_error(as, "invalid operands for %s", name);
    return -1;
}

static int handle_in0(AsmState *as)  { return handle_in0_out0(as, 0, "IN0"); }
static int handle_out0(AsmState *as) { return handle_in0_out0(as, 1, "OUT0"); }

/* ============================================================
 * Bit Operations
 * ============================================================ */

static int emit_cb_op(AsmState *as, int base, Operand *bit, Operand *op)
{
    int b, r;
    
    b = bit->value;
    if (b < 0 || b > 7) {
        asm_error(as, "bit number must be 0-7");
        return -1;
    }
    
    if (op->type == OP_REG) {
        r = get_reg8_code(op->reg);
        if (r >= 0) {
            emit_byte(as, 0xCB);
            emit_byte(as, base | (b << 3) | r);
            return 0;
        }
    }
    
    if (op->type == OP_IND_REG && op->reg == REG_IND_HL) {
        emit_byte(as, 0xCB);
        emit_byte(as, base | (b << 3) | 6);
        return 0;
    }
    
    if (op->type == OP_IX_OFF || op->type == OP_IY_OFF) {
        emit_idx_off_prefix(as, op->type);
        emit_byte(as, 0xCB);
        emit_byte(as, op->value & 0xFF);
        emit_byte(as, base | (b << 3) | 6);
        return 0;
    }
    
    return -1;
}

/* Shared handler for BIT, SET, RES */
static int handle_cb_op(AsmState *as, int base, const char *name)
{
    Operand bit, op;
    
    lexer_next(as);
    if (parse_operand(as, &bit) < 0) return -1;
    
    if (as->current_token.type != TOK_COMMA) {
        asm_error(as, "expected comma");
        return -1;
    }
    lexer_next(as);
    
    if (parse_operand(as, &op) < 0) return -1;
    
    if (emit_cb_op(as, base, &bit, &op) == 0) return 0;
    
    asm_error(as, "invalid operands for %s", name);
    return -1;
}

static int handle_bit(AsmState *as) { return handle_cb_op(as, 0x40, "BIT"); }
static int handle_set(AsmState *as) { return handle_cb_op(as, 0xC0, "SET"); }
static int handle_res(AsmState *as) { return handle_cb_op(as, 0x80, "RES"); }

/* ============================================================
 * Rotate/Shift Operations
 * ============================================================ */

static int emit_shift_op(AsmState *as, int opcode, Operand *op)
{
    int r;
    
    if (op->type == OP_REG) {
        r = get_reg8_code(op->reg);
        if (r >= 0) {
            emit_byte(as, 0xCB);
            emit_byte(as, opcode | r);
            return 0;
        }
    }
    
    if (op->type == OP_IND_REG && op->reg == REG_IND_HL) {
        emit_byte(as, 0xCB);
        emit_byte(as, opcode | 6);
        return 0;
    }
    
    if (op->type == OP_IX_OFF || op->type == OP_IY_OFF) {
        emit_idx_off_prefix(as, op->type);
        emit_byte(as, 0xCB);
        emit_byte(as, op->value & 0xFF);
        emit_byte(as, opcode | 6);
        return 0;
    }
    
    return -1;
}

/* Shared handler for rotate/shift: RLC, RRC, RL, RR, SLA, SRA, SRL */
static int handle_shift(AsmState *as, int opcode, const char *name)
{
    Operand op;
    lexer_next(as);
    if (parse_operand(as, &op) < 0) return -1;
    if (emit_shift_op(as, opcode, &op) == 0) return 0;
    asm_error(as, "invalid operand for %s", name);
    return -1;
}

static int handle_rlc(AsmState *as) { return handle_shift(as, 0x00, "RLC"); }
static int handle_rrc(AsmState *as) { return handle_shift(as, 0x08, "RRC"); }
static int handle_rl(AsmState *as)  { return handle_shift(as, 0x10, "RL"); }
static int handle_rr(AsmState *as)  { return handle_shift(as, 0x18, "RR"); }
static int handle_sla(AsmState *as) { return handle_shift(as, 0x20, "SLA"); }
static int handle_sra(AsmState *as) { return handle_shift(as, 0x28, "SRA"); }
static int handle_srl(AsmState *as) { return handle_shift(as, 0x38, "SRL"); }

/* ============================================================
 * eZ80 Specific Instructions
 * ============================================================ */

static int handle_lea(AsmState *as)
{
    Operand dest, src;
    
    lexer_next(as);
    if (parse_operand(as, &dest) < 0) return -1;
    
    if (dest.type != OP_REG) {
        asm_error(as, "LEA requires register destination");
        return -1;
    }
    
    if (as->current_token.type != TOK_COMMA) {
        asm_error(as, "expected comma");
        return -1;
    }
    lexer_next(as);
    
    if (parse_operand(as, &src) < 0) return -1;
    
    /* LEA encoding: ED + opcode + displacement
     * IX source opcodes: BC=02 DE=12 HL=22 IX=32 IY=55
     * IY source opcodes: BC=03 DE=13 HL=23 IY=33 IX=54
     * Regular regs: IY opcode = IX opcode + 1
     * Self (IX->IX, IY->IY): 32/33
     * Cross (IX->IY, IY->IX): 55/54 */
    if (src.type == OP_IX_OFF || src.type == OP_IY_OFF) {
        int is_iy_src = (src.type == OP_IY_OFF);
        int opc = -1;
        switch (dest.reg) {
            case REG_BC: opc = 0x02 + is_iy_src; break;
            case REG_DE: opc = 0x12 + is_iy_src; break;
            case REG_HL: opc = 0x22 + is_iy_src; break;
            case REG_IX: opc = is_iy_src ? 0x54 : 0x32; break;
            case REG_IY: opc = is_iy_src ? 0x33 : 0x55; break;
        }
        if (opc >= 0) {
            emit_byte(as, 0xED);
            emit_byte(as, opc);
            emit_byte(as, src.value & 0xFF);
            return 0;
        }
        asm_error(as, "invalid destination for LEA");
        return -1;
    }
    
    asm_error(as, "LEA requires IX+d or IY+d source");
    return -1;
}

static int handle_pea(AsmState *as)
{
    Operand op;
    
    lexer_next(as);
    if (parse_operand(as, &op) < 0) return -1;
    
    if (op.type == OP_IX_OFF || op.type == OP_IY_OFF) {
        emit_byte(as, 0xED);
        emit_byte(as, op.type == OP_IX_OFF ? 0x65 : 0x66);
        emit_byte(as, op.value & 0xFF);
        return 0;
    }
    
    asm_error(as, "PEA requires IX+d or IY+d operand");
    return -1;
}

static int handle_mlt(AsmState *as)
{
    Operand op;
    int ss;
    
    lexer_next(as);
    if (parse_operand(as, &op) < 0) return -1;
    
    if (op.type != OP_REG) {
        asm_error(as, "MLT requires register operand");
        return -1;
    }
    
    ss = get_reg16_dd_code(op.reg);
    if (ss < 0) {
        asm_error(as, "MLT requires BC, DE, HL, or SP");
        return -1;
    }
    
    emit_byte(as, 0xED);
    emit_byte(as, 0x4C | (ss << 4));
    return 0;
}

static int handle_tst(AsmState *as)
{
    Operand op;
    int r;
    
    lexer_next(as);
    if (parse_operand(as, &op) < 0) return -1;
    
    /* Skip optional A, */
    if (op.type == OP_REG && op.reg == REG_A) {
        if (as->current_token.type == TOK_COMMA) {
            lexer_next(as);
            if (parse_operand(as, &op) < 0) return -1;
        }
    }
    
    if (op.type == OP_REG) {
        r = get_reg8_code(op.reg);
        if (r >= 0) {
            emit_byte(as, 0xED);
            emit_byte(as, 0x04 | (r << 3));
            return 0;
        }
    }
    
    if (op.type == OP_IMM) {
        emit_byte(as, 0xED);
        emit_byte(as, 0x64);
        emit_byte(as, op.value & 0xFF);
        return 0;
    }
    
    asm_error(as, "invalid operand for TST");
    return -1;
}

/* ============================================================
 * Instruction Table and Lookup
 * ============================================================ */

/* ============================================================
 * Unified Instruction Table with Perfect Hash Lookup
 *
 * Hash function generated by gperf from the frozen mnemonic set.
 * Single lookup handles both simple (no-operand) and complex
 * instructions. Simple: handler==NULL, emit prefix+opcode.
 * Complex: handler!=NULL, call handler.
 * ============================================================ */

/* Unified instruction entry */
typedef struct {
    const char *mnemonic;
    InstrHandler handler;   /* NULL = simple (emit prefix+opcode) */
    uint8 prefix;           /* simple: 0xED or 0x00 */
    uint8 opcode;           /* simple: opcode byte */
} InstrEntry;

/* Perfect hash - computed positions: k'1-4'
 * Generated by gperf, max hash value 95 */
static const unsigned char asso_values[] =
{
      96, 96, 96, 96, 96, 96, 96, 96, 96, 96,
      96, 96, 96, 96, 96, 96, 96, 96, 96, 96,
      96, 96, 96, 96, 96, 96, 96, 96, 96, 96,
      96, 96, 96, 96, 96, 96, 96, 96, 96, 96,
      96, 96, 96, 96, 96, 96, 96, 96,  6, 96,
      96, 96, 96, 96, 96, 96, 96, 96, 96, 96,
      96, 96, 96, 96, 96, 96, 96, 96, 96, 96,
      96, 96, 96, 96, 96, 96, 96, 96, 96, 96,
      96, 96, 96, 96, 96, 96, 96, 96, 96, 96,
      96, 96, 96, 96, 96, 96, 96,  9, 38, 25,
       3, 38, 25, 11,  4, 11,  3, 96,  6, 18,
      14, 55, 20, 96,  4, 28, 15,  3, 96, 96,
      24, 96,  7, 96, 96, 96, 96, 96
};

static unsigned int instr_hash(const char *str, unsigned int len)
{
	unsigned int hval = 0;

	switch (len) {
		default: hval += asso_values[(unsigned char)str[3]]; /*FALLTHROUGH*/
		case 3:	 hval += asso_values[(unsigned char)str[2]]; /*FALLTHROUGH*/
		case 2:	 hval += asso_values[(unsigned char)str[1]]; /*FALLTHROUGH*/
		case 1:	 hval += asso_values[(unsigned char)str[0]]; break;
	}
	return hval;
}

#define MAX_HASH_VALUE 95

/* Unified instruction wordlist - indexed by perfect hash.
 * Empty slots have mnemonic==NULL (zero-initialized).
 * 76 instructions, hash range 7..95. */
static const InstrEntry instr_wordlist[MAX_HASH_VALUE + 1] = {
    {0},
	{0},
	{0},
	{0},
	{0},
	{0},
	{0},
    {"jr",     handle_jr, 0x00, 0x00},
    {"rr",     handle_rr, 0x00, 0x00},
    {"ld",     handle_ld, 0x00, 0x00},
    {"rl",     handle_rl, 0x00, 0x00},
    {"rrd",    NULL, 0xED, 0x67},
    {"ldd",    NULL, 0xED, 0xA8},
    {"rld",    NULL, 0xED, 0x6F},
    {"di",     NULL, 0x00, 0xF3},
    {"add",    handle_add, 0x00, 0x00},
    {"lddr",   NULL, 0xED, 0xB8},
    {"rra",    NULL, 0x00, 0x1F},
    {0},
    {"rla",    NULL, 0x00, 0x17},
    {"ldi",    NULL, 0xED, 0xA0},
    {"daa",    NULL, 0x00, 0x27},
    {0},
    {"jp",     handle_jp, 0x00, 0x00},
    {"ldir",   NULL, 0xED, 0xB0},
    {"in",     handle_in, 0x00, 0x00},
    {"and",    handle_and, 0x00, 0x00},
    {"djnz",   handle_djnz, 0x00, 0x00},
    {"ind",    NULL, 0xED, 0xAA},
    {"im",     handle_im, 0x00, 0x00},
    {0},
    {"in0",    handle_in0, 0x00, 0x00},
    {"indr",   NULL, 0xED, 0xBA},
    {"rrc",    handle_rrc, 0x00, 0x00},
    {"halt",   NULL, 0x00, 0x76},
    {"rlc",    handle_rlc, 0x00, 0x00},
    {"ini",    NULL, 0xED, 0xA2},
    {"adc",    handle_adc, 0x00, 0x00},
    {"srl",    handle_srl, 0x00, 0x00},
    {"mlt",    handle_mlt, 0x00, 0x00},
    {"inir",   NULL, 0xED, 0xB2},
    {"sra",    handle_sra, 0x00, 0x00},
    {"rrca",   NULL, 0x00, 0x0F},
    {"sla",    handle_sla, 0x00, 0x00},
    {"rlca",   NULL, 0x00, 0x07},
    {"cp",     handle_cp, 0x00, 0x00},
    {"call",   handle_call, 0x00, 0x00},
    {"rst",    handle_rst, 0x00, 0x00},
    {"cpd",    NULL, 0xED, 0xA9},
    {"ei",     NULL, 0x00, 0xFB},
    {"inc",    handle_inc, 0x00, 0x00},
    {"cpl",    NULL, 0x00, 0x2F},
    {"cpdr",   NULL, 0xED, 0xB9},
    {"lea",    handle_lea, 0x00, 0x00},
    {"slp",    NULL, 0xED, 0x76},
    {"push",   handle_push, 0x00, 0x00},
    {"cpi",    NULL, 0xED, 0xA1},
    {"ret",    handle_ret, 0x00, 0x00},
    {"tst",    handle_tst, 0x00, 0x00},
    {"or",     handle_or, 0x00, 0x00},
    {"cpir",   NULL, 0xED, 0xB1},
    {"rsmix",  NULL, 0xED, 0x7E},
    {"ex",     handle_ex, 0x00, 0x00},
    {"neg",    NULL, 0xED, 0x44},
    {"bit",    handle_bit, 0x00, 0x00},
    {0},
    {"dec",    handle_dec, 0x00, 0x00},
    {"pea",    handle_pea, 0x00, 0x00},
    {"reti",   NULL, 0xED, 0x4D},
    {"sub",    handle_sub, 0x00, 0x00},
    {"res",    handle_res, 0x00, 0x00},
    {"retn",   NULL, 0xED, 0x45},
    {"stmix",  NULL, 0xED, 0x7D},
    {"out",    handle_out, 0x00, 0x00},
    {0},
    {"ccf",    NULL, 0x00, 0x3F},
    {"outd",   NULL, 0xED, 0xAB},
    {"otdr",   NULL, 0xED, 0xBB},
    {"scf",    NULL, 0x00, 0x37},
    {"out0",   handle_out0, 0x00, 0x00},
    {0},
    {"set",    handle_set, 0x00, 0x00},
    {0},
    {"xor",    handle_xor, 0x00, 0x00},
    {"outi",   NULL, 0xED, 0xA3},
    {"otir",   NULL, 0xED, 0xB3},
    {"exx",    NULL, 0x00, 0xD9},
    {0},
	{0},
    {"nop",    NULL, 0x00, 0x00},
    {0},
    {"sbc",    handle_sbc, 0x00, 0x00},
    {0},
	{0},
	{0},
    {"pop",    handle_pop, 0x00, 0x00}
};

/* Lowercase a string into a fixed-size buffer */
static void instr_tolower(char *dest, const char *src, int maxlen)
{
    int i;
    for (i = 0; i < maxlen - 1 && src[i]; i++) {
        dest[i] = tolower((unsigned char)src[i]);
    }
    dest[i] = '\0';
}

int instr_execute(AsmState *as, const char *mnemonic)
{
    char lower[16];
    const InstrEntry *ie;
    unsigned int key;
    int len;
    int result;
    int suffix_byte = 0;
    char *dot;
    
    instr_tolower(lower, mnemonic, sizeof(lower));
    
    /* Check for suffix (.S, .L, .IS, .IL, .SIS, .SIL, .LIS, .LIL) */
    dot = strchr(lower, '.');
    if (dot) {
        const char *suf = dot + 1;
        if (strcmp(suf, "sis") == 0)      suffix_byte = 0x40;
        else if (strcmp(suf, "sil") == 0) suffix_byte = 0x52;
        else if (strcmp(suf, "lis") == 0) suffix_byte = 0x49;
        else if (strcmp(suf, "lil") == 0) suffix_byte = 0x5B;
        else if (strcmp(suf, "s") == 0)   suffix_byte = 0x52;
        else if (strcmp(suf, "l") == 0)   suffix_byte = 0x5B;
        else if (strcmp(suf, "is") == 0)  suffix_byte = 0x49;
        else if (strcmp(suf, "il") == 0)  suffix_byte = 0x5B;
        else {
            return -1;
        }
        *dot = '\0';
    }
    
    /* Perfect hash lookup */
    len = (int)strlen(lower);
    if (len < 2 || len > 5)
        return -1;
    
    key = instr_hash(lower, (unsigned int)len);
    if (key > MAX_HASH_VALUE)
        return -1;
    
    ie = &instr_wordlist[key];
    if (!ie->mnemonic || strcmp(lower, ie->mnemonic) != 0)
        return -1;
    
    /* Emit suffix prefix byte if present */
    if (suffix_byte) emit_byte(as, suffix_byte);
    
    if (ie->handler) {
        /* Complex instruction - call handler */
        result = ie->handler(as);
    } else {
        /* Simple instruction - emit prefix + opcode */
        lexer_next(as);
        if (ie->prefix) emit_byte(as, ie->prefix);
        emit_byte(as, ie->opcode);
        result = 0;
    }
    
    /* Check for unparsed content at end of line */
    if (result == 0 &&
        as->current_token.type != TOK_EOL &&
        as->current_token.type != TOK_EOF) {
        asm_error(as, "unexpected content after instruction");
        return -1;
    }
    
    return result;
}
