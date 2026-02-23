/*
 * eZ80 ADL Mode Assembler - Main Implementation
 * 
 * Part 1: Lexer, Parser, Symbol Table, and Core Functions
 * C89 compatible, 24-bit integers
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include "ez80asm.h"

/* ============================================================
 * Utility Functions
 * ============================================================ */

int is_8bit(int24 val)
{
    return (val >= 0 && val <= 255);
}

int is_16bit(int24 val)
{
    return (val >= 0 && val <= 65535);
}

int is_24bit(int24 val)
{
    return (val >= 0 && val <= 0xFFFFFF);
}

int is_signed_8bit(int24 val)
{
    return (val >= -128 && val <= 127);
}

int str_casecmp(const char *s1, const char *s2)
{
    while (*s1 && *s2) {
        char c1 = tolower((unsigned char)*s1);
        char c2 = tolower((unsigned char)*s2);
        if (c1 != c2) return c1 - c2;
        s1++;
        s2++;
    }
    return tolower((unsigned char)*s1) - tolower((unsigned char)*s2);
}

static void str_copy(char *dest, const char *src, int maxlen)
{
    int i;
    for (i = 0; i < maxlen - 1 && src[i]; i++) {
        dest[i] = src[i];
    }
    dest[i] = '\0';
}

/* Lowercase a string into a fixed-size buffer */
static void str_tolower(char *dest, const char *src, int maxlen)
{
    int i;
    for (i = 0; i < maxlen - 1 && src[i]; i++) {
        dest[i] = tolower((unsigned char)src[i]);
    }
    dest[i] = '\0';
}

/* ============================================================
 * Error Handling
 * ============================================================ */

void asm_error(AsmState *as, const char *fmt, ...)
{
    va_list args;
    fprintf(stderr, "%s:%d: error: ", as->filename, as->line_num);
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
    as->errors++;
}

void asm_warning(AsmState *as, const char *fmt, ...)
{
    va_list args;
    fprintf(stderr, "%s:%d: warning: ", as->filename, as->line_num);
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
    as->warnings++;
}

/* ============================================================
 * Lexer
 * ============================================================ */

void lexer_init(AsmState *as, const char *line)
{
    as->line_ptr = line;
    as->current_token.type = TOK_ERROR;
}

void lexer_skip_whitespace(AsmState *as)
{
    while (*as->line_ptr && (*as->line_ptr == ' ' || *as->line_ptr == '\t')) {
        as->line_ptr++;
    }
}

static int is_ident_start(char c)
{
    return isalpha((unsigned char)c) || c == '_' || c == '.' || c == '@';
}

static int is_ident_char(char c)
{
    return isalnum((unsigned char)c) || c == '_' || c == '.' || c == '@';
}

Token *lexer_next(AsmState *as)
{
    Token *tok = &as->current_token;
    int i;
    int base;
    char c;
    
    /* Minimal init instead of memset on ~270 bytes */
    tok->type = TOK_ERROR;
    tok->text[0] = '\0';
    tok->value = 0;
    tok->line = as->line_num;
    
    lexer_skip_whitespace(as);
    
    /* Check for end of line or comment */
    if (*as->line_ptr == '\0' || *as->line_ptr == '\n' || 
        *as->line_ptr == ';' || *as->line_ptr == '#') {
        tok->type = TOK_EOL;
        return tok;
    }
    
    /* Check for single character tokens */
    switch (*as->line_ptr) {
        case ',': tok->type = TOK_COMMA; tok->text[0] = *as->line_ptr++; 
                  tok->text[1] = '\0'; return tok;
        case ':': tok->type = TOK_COLON; tok->text[0] = *as->line_ptr++; 
                  tok->text[1] = '\0'; return tok;
        case '(': tok->type = TOK_LPAREN; tok->text[0] = *as->line_ptr++; 
                  tok->text[1] = '\0'; return tok;
        case ')': tok->type = TOK_RPAREN; tok->text[0] = *as->line_ptr++; 
                  tok->text[1] = '\0'; return tok;
        case '+': tok->type = TOK_PLUS; tok->text[0] = *as->line_ptr++; 
                  tok->text[1] = '\0'; return tok;
        case '-': tok->type = TOK_MINUS; tok->text[0] = *as->line_ptr++; 
                  tok->text[1] = '\0'; return tok;
        case '*': tok->type = TOK_STAR; tok->text[0] = *as->line_ptr++; 
                  tok->text[1] = '\0'; return tok;
        case '/': tok->type = TOK_SLASH; tok->text[0] = *as->line_ptr++; 
                  tok->text[1] = '\0'; return tok;
        case '=': tok->type = TOK_EQUALS; tok->text[0] = *as->line_ptr++; 
                  tok->text[1] = '\0'; return tok;
    }
    
    /* Number */
    if (isdigit((unsigned char)*as->line_ptr)) {
        i = 0;
        base = 10;
        
        /* Check for hex prefix */
        if (*as->line_ptr == '0' && 
            (as->line_ptr[1] == 'x' || as->line_ptr[1] == 'X')) {
            base = 16;
            as->line_ptr += 2;
        } else {
            /* Look ahead for h suffix: [0-9][0-9a-fA-F]*[hH] */
            const char *p = as->line_ptr;
            while (isxdigit((unsigned char)*p)) p++;
            if ((*p == 'h' || *p == 'H') &&
                !is_ident_char(p[1])) {
                base = 16;
            }
        }
        
        while (i < MAX_LABEL_LEN - 1) {
            c = *as->line_ptr;
            if (base == 16 && isxdigit((unsigned char)c)) {
                tok->text[i++] = c;
                as->line_ptr++;
            } else if (base == 10 && isdigit((unsigned char)c)) {
                tok->text[i++] = c;
                as->line_ptr++;
            } else if (c == 'h' || c == 'H') {
                /* Suffix hex notation - consume the h */
                as->line_ptr++;
                break;
            } else {
                break;
            }
        }
        tok->text[i] = '\0';
        tok->value = (int24)strtoul(tok->text, NULL, base);
        tok->type = TOK_NUMBER;
        return tok;
    }
    
    /* Hex with $ prefix */
    if (*as->line_ptr == '$' && isxdigit((unsigned char)as->line_ptr[1])) {
        i = 0;
        as->line_ptr++; /* Skip $ */
        while (i < MAX_LABEL_LEN - 1 && isxdigit((unsigned char)*as->line_ptr)) {
            tok->text[i++] = *as->line_ptr++;
        }
        tok->text[i] = '\0';
        tok->value = (int24)strtoul(tok->text, NULL, 16);
        tok->type = TOK_NUMBER;
        return tok;
    }
    
    /* Binary with % prefix */
    if (*as->line_ptr == '%' && 
        (as->line_ptr[1] == '0' || as->line_ptr[1] == '1')) {
        i = 0;
        as->line_ptr++; /* Skip % */
        while (i < MAX_LABEL_LEN - 1 && 
               (*as->line_ptr == '0' || *as->line_ptr == '1')) {
            tok->text[i++] = *as->line_ptr++;
        }
        tok->text[i] = '\0';
        tok->value = (int24)strtoul(tok->text, NULL, 2);
        tok->type = TOK_NUMBER;
        return tok;
    }
    
    /* $ alone means current PC */
    if (*as->line_ptr == '$') {
        tok->type = TOK_DOLLAR;
        tok->text[0] = *as->line_ptr++;
        tok->text[1] = '\0';
        return tok;
    }
    
    /* Identifier */
    if (is_ident_start(*as->line_ptr)) {
        i = 0;
        while (i < MAX_LABEL_LEN - 1 && is_ident_char(*as->line_ptr)) {
            tok->text[i++] = *as->line_ptr++;
        }
        /* Special case: AF' register */
        if (i == 2 && *as->line_ptr == '\'' &&
            (tok->text[0] == 'a' || tok->text[0] == 'A') &&
            (tok->text[1] == 'f' || tok->text[1] == 'F')) {
            tok->text[i++] = *as->line_ptr++;
        }
        tok->text[i] = '\0';
        tok->type = TOK_IDENT;
        
        /* Check if followed by colon - it's a label */
        lexer_skip_whitespace(as);
        if (*as->line_ptr == ':') {
            tok->type = TOK_LABEL;
            as->line_ptr++; /* consume the colon */
        }
        return tok;
    }
    
    /* String literal */
    if (*as->line_ptr == '"') {
        i = 0;
        as->line_ptr++; /* Skip opening quote */
        while (*as->line_ptr && *as->line_ptr != '"' && i < MAX_STRING_LEN - 1) {
            if (*as->line_ptr == '\\' && as->line_ptr[1]) {
                as->line_ptr++;
                switch (*as->line_ptr) {
                    case 'n': tok->text[i++] = '\n'; break;
                    case 'r': tok->text[i++] = '\r'; break;
                    case 't': tok->text[i++] = '\t'; break;
                    case '0': tok->text[i++] = '\0'; break;
                    case '\\': tok->text[i++] = '\\'; break;
                    case '"': tok->text[i++] = '"'; break;
                    default: tok->text[i++] = *as->line_ptr; break;
                }
                as->line_ptr++;
            } else {
                tok->text[i++] = *as->line_ptr++;
            }
        }
        tok->text[i] = '\0';
        
        /* Check if string was truncated */
        if (*as->line_ptr && *as->line_ptr != '"') {
            asm_error(as, "string too long (max %d characters)", MAX_STRING_LEN - 1);
            /* Skip to end of string */
            while (*as->line_ptr && *as->line_ptr != '"') {
                as->line_ptr++;
            }
        }
        
        if (*as->line_ptr == '"') as->line_ptr++;
        tok->type = TOK_STRING;
        return tok;
    }
    
    /* Character literal */
    if (*as->line_ptr == '\'') {
        as->line_ptr++;
        if (*as->line_ptr == '\\' && as->line_ptr[1]) {
            as->line_ptr++;
            switch (*as->line_ptr) {
                case 'n': tok->value = '\n'; break;
                case 'r': tok->value = '\r'; break;
                case 't': tok->value = '\t'; break;
                case '0': tok->value = '\0'; break;
                default: tok->value = *as->line_ptr; break;
            }
            as->line_ptr++;
        } else {
            tok->value = *as->line_ptr++;
        }
        if (*as->line_ptr == '\'') as->line_ptr++;
        tok->type = TOK_CHAR;
        tok->text[0] = (char)tok->value;
        tok->text[1] = '\0';
        return tok;
    }
    
    tok->type = TOK_ERROR;
    tok->text[0] = *as->line_ptr++;
    tok->text[1] = '\0';
    return tok;
}

Token *lexer_peek(AsmState *as)
{
    const char *saved_ptr;
    Token saved_tok;
    
    saved_ptr = as->line_ptr;
    saved_tok = as->current_token;
    lexer_next(as);
    as->peek_token = as->current_token;
    as->current_token = saved_tok;
    as->line_ptr = saved_ptr;
    return &as->peek_token;
}

/* ============================================================
 * Register and Condition Code Parsing
 * ============================================================ */

/* Register name lookup table - sorted alphabetically for binary search */
typedef struct { const char *name; int code; } RegEntry;

static const RegEntry reg_table[] = {
    {"a",   REG_A},
    {"af",  REG_AF},
    {"af'", REG_AF_},
    {"b",   REG_B},
    {"bc",  REG_BC},
    {"c",   REG_C},
    {"d",   REG_D},
    {"de",  REG_DE},
    {"e",   REG_E},
    {"h",   REG_H},
    {"hl",  REG_HL},
    {"i",   REG_I},
    {"ix",  REG_IX},
    {"ixh", REG_IXH},
    {"ixl", REG_IXL},
    {"iy",  REG_IY},
    {"iyh", REG_IYH},
    {"iyl", REG_IYL},
    {"l",   REG_L},
    {"mb",  REG_MB},
    {"r",   REG_R},
    {"sp",  REG_SP}
};
#define NUM_REGS (sizeof(reg_table) / sizeof(reg_table[0]))

int parse_register(const char *name)
{
    int lo = 0, hi = (int)NUM_REGS - 1;
    char lower[8]; /* longest register name is "af'" = 3 chars */
    str_tolower(lower, name, sizeof(lower));
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int cmp = strcmp(lower, reg_table[mid].name);
        if (cmp == 0) return reg_table[mid].code;
        if (cmp < 0) hi = mid - 1;
        else lo = mid + 1;
    }
    return REG_NONE;
}

/* Condition code lookup table - sorted alphabetically for binary search */
typedef struct { const char *name; int code; } CCEntry;

static const CCEntry cc_table[] = {
    {"c",  CC_C},
    {"m",  CC_M},
    {"nc", CC_NC},
    {"nz", CC_NZ},
    {"p",  CC_P},
    {"pe", CC_PE},
    {"po", CC_PO},
    {"z",  CC_Z}
};
#define NUM_CCS (sizeof(cc_table) / sizeof(cc_table[0]))

int parse_condition(const char *name)
{
    int lo = 0, hi = (int)NUM_CCS - 1;
    char lower[4]; /* longest condition is "nz","nc","po","pe" = 2 chars */
    str_tolower(lower, name, sizeof(lower));
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int cmp = strcmp(lower, cc_table[mid].name);
        if (cmp == 0) return cc_table[mid].code;
        if (cmp < 0) hi = mid - 1;
        else lo = mid + 1;
    }
    return CC_NONE;
}

/* ============================================================
 * Symbol Table
 * ============================================================ */

/* Case-sensitive hash for symbol names */
static unsigned symbol_hash(const char *name)
{
    unsigned h = 5381;
    while (*name) {
        h = ((h << 5) + h) ^ (unsigned char)*name;
        name++;
    }
    return h & (SYM_HASH_SIZE - 1);
}

Symbol *symbol_find(AsmState *as, const char *name)
{
    unsigned h = symbol_hash(name);
    int i = as->sym_hash[h];
    while (i >= 0) {
        if (strcmp(as->symbols[i].name, name) == 0) {
            return &as->symbols[i];
        }
        i = as->symbols[i].hash_next;
    }
    return NULL;
}

Symbol *symbol_add(AsmState *as, const char *name)
{
    Symbol *sym;
    unsigned h;
    
    if (as->num_symbols >= MAX_SYMBOLS) {
        asm_error(as, "symbol table full");
        return NULL;
    }
    
    sym = &as->symbols[as->num_symbols];
    str_copy(sym->name, name, MAX_LABEL_LEN);
    sym->value = 0;
    sym->section = as->current_section;
    sym->flags = SYM_LOCAL;
    sym->defined = 0;
    sym->pass1_value = 0;
    
    /* Insert at head of hash chain */
    h = symbol_hash(name);
    sym->hash_next = as->sym_hash[h];
    as->sym_hash[h] = as->num_symbols;
    
    as->num_symbols++;
    
    return sym;
}

int symbol_define(AsmState *as, const char *name, uint24 value)
{
    Symbol *sym;
    
    sym = symbol_find(as, name);
    
    if (sym) {
        if (sym->defined && as->pass == 1) {
            asm_error(as, "symbol '%s' already defined", name);
            return -1;
        }
        if (sym->flags == SYM_EXTERN) {
            asm_error(as, "cannot define external symbol '%s'", name);
            return -1;
        }
    } else {
        sym = symbol_add(as, name);
        if (!sym) return -1;
    }
    
    sym->value = value;
    sym->section = as->current_section;
    sym->defined = 1;
    if (as->pass == 1) {
        sym->pass1_value = value;
    }
    
    return 0;
}

int symbol_set_export(AsmState *as, const char *name)
{
    Symbol *sym;
    
    sym = symbol_find(as, name);
    if (!sym) {
        sym = symbol_add(as, name);
        if (!sym) return -1;
    }
    sym->flags = SYM_EXPORT;
    return 0;
}

int symbol_set_extern(AsmState *as, const char *name)
{
    Symbol *sym;
    int i;
    
    sym = symbol_find(as, name);
    if (sym && sym->defined) {
        asm_error(as, "cannot declare defined symbol '%s' as external", name);
        return -1;
    }
    if (!sym) {
        sym = symbol_add(as, name);
        if (!sym) return -1;
    }
    sym->flags = SYM_EXTERN;
    
    /* Check if already in externs list */
    for (i = 0; i < as->num_externs; i++) {
        if (strcmp(as->externs[i], name) == 0) {
            return 0;  /* Already there */
        }
    }
    
    /* Add to externs list */
    if (as->num_externs < MAX_EXTERNS) {
        str_copy(as->externs[as->num_externs], name, MAX_LABEL_LEN);
        as->num_externs++;
    }
    
    return 0;
}

int symbol_is_extern(AsmState *as, const char *name)
{
    int i;
    
    for (i = 0; i < as->num_externs; i++) {
        if (strcmp(name, as->externs[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Get external symbol index, returns -1 if not found */
int symbol_extern_index(AsmState *as, const char *name)
{
    int i;
    
    for (i = 0; i < as->num_externs; i++) {
        if (strcmp(name, as->externs[i]) == 0) {
            return i;
        }
    }
    return -1;
}

/* Check if a label is local (starts with @) */
int symbol_is_local(const char *name)
{
    return name && name[0] == '@';
}

/* Mangle a local label by appending scope number */
void symbol_mangle_local(AsmState *as, const char *name, char *out, int max_len)
{
    int i, len;
    char scope_str[16];
    
    /* Copy the original name */
    for (i = 0; i < max_len - 1 && name[i]; i++) {
        out[i] = name[i];
    }
    out[i] = '\0';
    
    /* Append colon and scope number */
    sprintf(scope_str, ":%d", as->local_scope);
    len = (int)strlen(out);
    for (i = 0; scope_str[i] && len + i < max_len - 1; i++) {
        out[len + i] = scope_str[i];
    }
    out[len + i] = '\0';
}

/* ============================================================
 * Expression Parser
 * ============================================================ */

static int24 parse_expr_add(AsmState *as, char *symbol_out, int *has_symbol);

static int24 parse_expr_primary(AsmState *as, char *symbol_out, int *has_symbol)
{
    Token *tok;
    int24 val = 0;
    Symbol *sym;
    char lookup_name[MAX_LABEL_LEN];
    
    tok = &as->current_token;
    
    if (tok->type == TOK_NUMBER || tok->type == TOK_CHAR) {
        val = tok->value;
        lexer_next(as);
    }
    else if (tok->type == TOK_DOLLAR) {
        /* $ means current PC */
        lexer_next(as);
        val = as->pc;
    }
    else if (tok->type == TOK_IDENT) {
        /* Mangle local labels before lookup */
        if (symbol_is_local(tok->text)) {
            symbol_mangle_local(as, tok->text, lookup_name, MAX_LABEL_LEN);
        } else {
            str_copy(lookup_name, tok->text, MAX_LABEL_LEN);
        }
        
        sym = symbol_find(as, lookup_name);
        if (sym && sym->defined) {
            val = sym->value;
            /* Generate relocation for section-relative symbols */
            if (sym->section != 0) {
                str_copy(symbol_out, lookup_name, MAX_LABEL_LEN);
                *has_symbol = 1;
            }
        } else if (sym && sym->flags == SYM_EXTERN) {
            /* External symbol - value unknown until link */
            str_copy(symbol_out, lookup_name, MAX_LABEL_LEN);
            *has_symbol = 1;
            val = 0;
        } else if (as->pass == 1) {
            /* Pass 1: forward reference, use 0 */
            val = 0;
            str_copy(symbol_out, lookup_name, MAX_LABEL_LEN);
            *has_symbol = 1;
        } else {
            /* Pass 2: undefined symbol */
            asm_error(as, "undefined symbol '%s'", tok->text);
            val = 0;
        }
        lexer_next(as);
    }
    else if (tok->type == TOK_LPAREN) {
        lexer_next(as);
        val = parse_expr_add(as, symbol_out, has_symbol);
        if (as->current_token.type != TOK_RPAREN) {
            asm_error(as, "expected ')'");
        } else {
            lexer_next(as);
        }
    }
    else if (tok->type == TOK_MINUS) {
        lexer_next(as);
        val = -parse_expr_primary(as, symbol_out, has_symbol);
    }
    else if (tok->type == TOK_PLUS) {
        lexer_next(as);
        val = parse_expr_primary(as, symbol_out, has_symbol);
    }
    
    return val;
}

static int24 parse_expr_mul(AsmState *as, char *symbol_out, int *has_symbol)
{
    int24 val;
    int op;
    int24 rhs;
    
    val = parse_expr_primary(as, symbol_out, has_symbol);
    
    while (as->current_token.type == TOK_STAR || 
           as->current_token.type == TOK_SLASH) {
        op = as->current_token.type;
        lexer_next(as);
        rhs = parse_expr_primary(as, symbol_out, has_symbol);
        if (op == TOK_STAR) {
            val *= rhs;
        } else {
            if (rhs == 0) {
                asm_error(as, "division by zero");
            } else {
                val /= rhs;
            }
        }
    }
    
    return val;
}

static int24 parse_expr_add(AsmState *as, char *symbol_out, int *has_symbol)
{
    int24 val;
    int op;
    int24 rhs;
    char lhs_symbol[MAX_LABEL_LEN];
    char rhs_symbol[MAX_LABEL_LEN];
    int lhs_has_symbol = 0;
    int rhs_has_symbol = 0;
    Symbol *lhs_sym, *rhs_sym;
    
    lhs_symbol[0] = '\0';
    val = parse_expr_mul(as, lhs_symbol, &lhs_has_symbol);
    
    while (as->current_token.type == TOK_PLUS || 
           as->current_token.type == TOK_MINUS) {
        op = as->current_token.type;
        lexer_next(as);
        rhs_symbol[0] = '\0';
        rhs_has_symbol = 0;
        rhs = parse_expr_mul(as, rhs_symbol, &rhs_has_symbol);
        
        if (op == TOK_PLUS) {
            val += rhs;
            /* If RHS has symbol and LHS doesn't, use RHS symbol */
            if (rhs_has_symbol && !lhs_has_symbol) {
                str_copy(lhs_symbol, rhs_symbol, MAX_LABEL_LEN);
                lhs_has_symbol = 1;
            }
            /* If both have symbols, keep LHS (complex expression) */
        } else {
            val -= rhs;
            /* Subtraction: check if symbols cancel out */
            if (lhs_has_symbol && rhs_has_symbol) {
                /* Both have symbols - check if same section */
                lhs_sym = symbol_find(as, lhs_symbol);
                rhs_sym = symbol_find(as, rhs_symbol);
                if (lhs_sym && rhs_sym && 
                    lhs_sym->section == rhs_sym->section &&
                    lhs_sym->section != 0) {
                    /* Same section - symbols cancel out, result is constant */
                    lhs_has_symbol = 0;
                    lhs_symbol[0] = '\0';
                }
                /* Different sections - keep LHS symbol (may be invalid) */
            }
            /* If only RHS has symbol (0 - sym), this is unusual but keep it */
            else if (rhs_has_symbol && !lhs_has_symbol) {
                /* Negative symbol reference - keep as relocatable */
                str_copy(lhs_symbol, rhs_symbol, MAX_LABEL_LEN);
                lhs_has_symbol = 1;
            }
            /* If only LHS has symbol, keep it */
        }
    }
    
    /* Copy result back */
    str_copy(symbol_out, lhs_symbol, MAX_LABEL_LEN);
    *has_symbol = lhs_has_symbol;
    
    return val;
}

int parse_expression(AsmState *as, int24 *result, char *symbol_out)
{
    int has_symbol = 0;
    symbol_out[0] = '\0';
    *result = parse_expr_add(as, symbol_out, &has_symbol);
    return has_symbol;
}

/* ============================================================
 * Operand Parser
 * ============================================================ */

int parse_operand(AsmState *as, Operand *op)
{
    Token *tok;
    int reg;
    int cc;
    
    memset(op, 0, sizeof(*op));
    op->type = OP_NONE;
    op->cc = CC_NONE;
    
    tok = &as->current_token;
    
    /* Check for indirect addressing (xxx) */
    if (tok->type == TOK_LPAREN) {
        lexer_next(as);
        /* tok now points to the updated current_token */
        
        if (tok->type == TOK_IDENT) {
            reg = parse_register(tok->text);
            
            if (reg == REG_HL) {
                lexer_next(as);
                if (as->current_token.type != TOK_RPAREN) {
                    asm_error(as, "expected ')'");
                    return -1;
                }
                lexer_next(as);
                op->type = OP_IND_REG;
                op->reg = REG_IND_HL;
                return 0;
            }
            else if (reg == REG_BC) {
                lexer_next(as);
                if (as->current_token.type != TOK_RPAREN) {
                    asm_error(as, "expected ')'");
                    return -1;
                }
                lexer_next(as);
                op->type = OP_IND_REG;
                op->reg = REG_IND_BC;
                return 0;
            }
            else if (reg == REG_DE) {
                lexer_next(as);
                if (as->current_token.type != TOK_RPAREN) {
                    asm_error(as, "expected ')'");
                    return -1;
                }
                lexer_next(as);
                op->type = OP_IND_REG;
                op->reg = REG_IND_DE;
                return 0;
            }
            else if (reg == REG_SP) {
                lexer_next(as);
                if (as->current_token.type != TOK_RPAREN) {
                    asm_error(as, "expected ')'");
                    return -1;
                }
                lexer_next(as);
                op->type = OP_IND_REG;
                op->reg = REG_IND_SP;
                return 0;
            }
            else if (reg == REG_C) {
                lexer_next(as);
                if (as->current_token.type != TOK_RPAREN) {
                    asm_error(as, "expected ')'");
                    return -1;
                }
                lexer_next(as);
                op->type = OP_IND_REG;
                op->reg = REG_IND_C;
                return 0;
            }
            else if (reg == REG_IX || reg == REG_IY) {
                lexer_next(as);
                
                /* Check for bare (IX)/(IY) without offset */
                if (as->current_token.type == TOK_RPAREN) {
                    lexer_next(as);
                    op->type = OP_IND_REG;
                    op->reg = (reg == REG_IX) ? REG_IND_IX : REG_IND_IY;
                    return 0;
                }
                
                op->type = (reg == REG_IX) ? OP_IX_OFF : OP_IY_OFF;
                op->value = 0;
                
                if (as->current_token.type == TOK_PLUS) {
                    lexer_next(as);
                    op->has_symbol = parse_expression(as, &op->value, op->symbol);
                }
                else if (as->current_token.type == TOK_MINUS) {
                    lexer_next(as);
                    op->has_symbol = parse_expression(as, &op->value, op->symbol);
                    op->value = -op->value;
                }
                
                if (as->current_token.type != TOK_RPAREN) {
                    asm_error(as, "expected ')'");
                    return -1;
                }
                lexer_next(as);
                return 0;
            }
        }
        
        /* Not a register - must be address */
        op->type = OP_ADDR;
        op->has_symbol = parse_expression(as, &op->value, op->symbol);
        
        if (as->current_token.type != TOK_RPAREN) {
            asm_error(as, "expected ')'");
            return -1;
        }
        lexer_next(as);
        return 0;
    }
    
    /* Check for register or condition code */
    if (tok->type == TOK_IDENT) {
        reg = parse_register(tok->text);
        if (reg != REG_NONE) {
            lexer_next(as);
            
            /* Check for IX+d or IY+d without parentheses (for LEA/PEA) */
            if ((reg == REG_IX || reg == REG_IY) &&
                (as->current_token.type == TOK_PLUS || 
                 as->current_token.type == TOK_MINUS)) {
                op->type = (reg == REG_IX) ? OP_IX_OFF : OP_IY_OFF;
                op->value = 0;
                if (as->current_token.type == TOK_PLUS) {
                    lexer_next(as);
                    op->has_symbol = parse_expression(as, &op->value, op->symbol);
                } else {
                    lexer_next(as);
                    op->has_symbol = parse_expression(as, &op->value, op->symbol);
                    op->value = -op->value;
                }
                return 0;
            }
            
            /* Special case: 'C' can be both register and condition.
             * Store both interpretations - handlers will decide. */
            if (reg == REG_C) {
                op->type = OP_REG;
                op->reg = reg;
                op->cc = CC_C;  /* Also store condition interpretation */
                return 0;
            }
            
            op->type = OP_REG;
            op->reg = reg;
            return 0;
        }
        
        cc = parse_condition(tok->text);
        if (cc != CC_NONE) {
            op->type = OP_COND;
            op->cc = cc;
            lexer_next(as);
            return 0;
        }
    }
    
    /* Must be immediate value or expression */
    op->type = OP_IMM;
    op->has_symbol = parse_expression(as, &op->value, op->symbol);
    
    return 0;
}

/* ============================================================
 * Code Emission
 * ============================================================ */

void emit_byte(AsmState *as, uint8 b)
{
    if (as->pass == 2) {
        if (as->current_section == SECT_CODE) {
            if (as->code_tmp) {
                fputc(b, as->code_tmp);
            }
            as->code_size++;
        } else if (as->current_section == SECT_DATA) {
            if (as->data_tmp) {
                fputc(b, as->data_tmp);
            }
            as->data_size++;
        } else if (as->current_section == SECT_BSS) {
            /* BSS doesn't emit bytes, just tracks size */
            as->bss_size++;
        }
    }
    as->pc++;
}

void emit_word(AsmState *as, uint24 w)
{
    emit_byte(as, w & 0xFF);
    emit_byte(as, (w >> 8) & 0xFF);
}

void emit_long(AsmState *as, uint24 l)
{
    emit_byte(as, l & 0xFF);
    emit_byte(as, (l >> 8) & 0xFF);
    emit_byte(as, (l >> 16) & 0xFF);
}

void emit_reloc(AsmState *as, uint8 type, const char *symbol)
{
    Relocation r;
    Symbol *sym;
    int ext_idx;
    
    if (as->pass == 2 && symbol[0] != '\0' && as->reloc_tmp) {
        r.offset = (as->current_section == SECT_CODE) ? 
                    as->code_size : as->data_size;
        r.section = as->current_section;
        r.type = type;
        
        /* Check if external */
        ext_idx = symbol_extern_index(as, symbol);
        if (ext_idx >= 0) {
            r.target_sect = 0;
            r.ext_index = ext_idx;
        } else {
            /* Local symbol - get its section */
            sym = symbol_find(as, symbol);
            if (sym && sym->defined) {
                r.target_sect = sym->section;
            } else {
                r.target_sect = as->current_section;
            }
            r.ext_index = 0;
        }
        
        fwrite(&r, sizeof(r), 1, as->reloc_tmp);
        as->num_relocs++;
    }
}

/* ============================================================
 * Initialization and Cleanup
 * ============================================================ */

int asm_init(AsmState *as)
{
    int i;
    
    memset(as, 0, sizeof(*as));
    
    as->symbols = (Symbol *)malloc(MAX_SYMBOLS * sizeof(Symbol));
    as->externs = (char (*)[MAX_LABEL_LEN])malloc(MAX_EXTERNS * MAX_LABEL_LEN);
    
    if (!as->symbols || !as->externs) {
        asm_free(as);
        return -1;
    }
    
    /* Initialize hash buckets to empty */
    for (i = 0; i < SYM_HASH_SIZE; i++) {
        as->sym_hash[i] = -1;
    }
    
    as->current_section = SECT_CODE;
    as->pass = 1;
    
    return 0;
}

void asm_free(AsmState *as)
{
    if (as->symbols) free(as->symbols);
    if (as->externs) free(as->externs);
    if (as->code_tmp) fclose(as->code_tmp);
    if (as->data_tmp) fclose(as->data_tmp);
    if (as->reloc_tmp) fclose(as->reloc_tmp);
    if (as->list_file) fclose(as->list_file);
    memset(as, 0, sizeof(*as));
}
