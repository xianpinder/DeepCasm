/*
 * eZ80 ADL Mode Assembler
 * 
 * Assembler for eZ80 processors in ADL (24-bit) mode.
 * C89 compatible, designed for 24-bit int/long.
 * Uses temporary files to reduce memory usage.
 */

#ifndef EZ80ASM_H
#define EZ80ASM_H

#include <stdio.h>
#include "objformat.h"

/* Configuration limits */
#define MAX_LINE_LEN    512
#define MAX_LABEL_LEN   64
#define MAX_STRING_LEN  256
#define MAX_SYMBOLS     4096 /* was 512 */
#define MAX_EXTERNS     128
#define SYM_HASH_SIZE   256

/* Token types */
#define TOK_EOF         0
#define TOK_EOL         1
#define TOK_LABEL       2
#define TOK_IDENT       3
#define TOK_NUMBER      4
#define TOK_STRING      5
#define TOK_CHAR        6
#define TOK_COMMA       7
#define TOK_COLON       8
#define TOK_LPAREN      9
#define TOK_RPAREN      10
#define TOK_PLUS        11
#define TOK_MINUS       12
#define TOK_STAR        13
#define TOK_SLASH       14
#define TOK_DOLLAR      15
#define TOK_HASH        16
#define TOK_DOT         17
#define TOK_EQUALS      18
#define TOK_ERROR       19

/* Token structure */
typedef struct {
    int type;
    char text[MAX_STRING_LEN];
    int24 value;
    int line;
    int column;
} Token;

/* Register identifiers */
#define REG_NONE    0
#define REG_A       1
#define REG_B       2
#define REG_C       3
#define REG_D       4
#define REG_E       5
#define REG_H       6
#define REG_L       7
#define REG_IXH     8
#define REG_IXL     9
#define REG_IYH     10
#define REG_IYL     11
#define REG_I       12
#define REG_R       13
#define REG_MB      14
#define REG_AF      15
#define REG_BC      16
#define REG_DE      17
#define REG_HL      18
#define REG_SP      19
#define REG_IX      20
#define REG_IY      21
#define REG_AF_     22
#define REG_IND_BC  23
#define REG_IND_DE  24
#define REG_IND_HL  25
#define REG_IND_SP  26
#define REG_IND_IX  27
#define REG_IND_IY  28
#define REG_IND_C   29

/* Condition codes */
#define CC_NONE     (-1)
#define CC_NZ       0
#define CC_Z        1
#define CC_NC       2
#define CC_C        3
#define CC_PO       4
#define CC_PE       5
#define CC_P        6
#define CC_M        7

/* Operand types */
#define OP_NONE     0
#define OP_REG      1
#define OP_IMM      2
#define OP_ADDR     3
#define OP_IND_REG  4
#define OP_IX_OFF   5
#define OP_IY_OFF   6
#define OP_COND     7
#define OP_BIT      8
#define OP_RST      9
#define OP_IM       10

/* Operand structure */
typedef struct {
    int type;
    int reg;
    int cc;
    int24 value;
    char symbol[MAX_LABEL_LEN];
    int has_symbol;
} Operand;

/* Symbol definition */
typedef struct {
    char name[MAX_LABEL_LEN];
    uint24 value;
    uint8 section;
    uint8 flags;
    int defined;
    uint24 pass1_value;
    int hash_next;              /* next index in hash chain, -1 = end */
} Symbol;

/* Relocation entry (for temp file) */
typedef struct {
    uint24 offset;
    uint8 section;
    uint8 type;
    uint8 target_sect;      /* Target section (0=external, 1=CODE, 2=DATA, 3=BSS) */
    uint24 ext_index;       /* External index if target_sect==0 */
} Relocation;

/* Assembler state */
typedef struct {
    /* Source tracking */
    const char *filename;
    int line_num;
    int pass;
    int errors;
    int warnings;
    
    /* Current line parsing */
    const char *line_ptr;
    Token current_token;
    Token peek_token;           /* buffer for lexer_peek results */
    
    /* Temporary files for output (pass 2) */
    FILE *code_tmp;
    FILE *data_tmp;
    FILE *reloc_tmp;
    
    /* Section sizes */
    uint24 code_size;
    uint24 data_size;
    uint24 bss_size;
    uint24 num_relocs;
    
    /* Current section and position */
    uint8 current_section;
    uint24 pc;
    
    /* Per-section PC tracking (for switching between sections) */
    uint24 code_pc;
    uint24 data_pc;
    uint24 bss_pc;
    
    /* Symbol table (kept in memory for lookups) */
    Symbol *symbols;
    int num_symbols;
    int sym_hash[SYM_HASH_SIZE]; /* hash bucket heads, -1 = empty */
    
    /* External references */
    char (*externs)[MAX_LABEL_LEN];
    int num_externs;
    
    /* Local label scope counter */
    int local_scope;
    
    /* Options */
    int verbose;
    int list_enabled;
    FILE *list_file;
} AsmState;

/* Function prototypes - Lexer */
void lexer_init(AsmState *as, const char *line);
Token *lexer_next(AsmState *as);
Token *lexer_peek(AsmState *as);
void lexer_skip_whitespace(AsmState *as);

/* Function prototypes - Parser */
int parse_operand(AsmState *as, Operand *op);
int parse_expression(AsmState *as, int24 *result, char *symbol_out);
int parse_register(const char *name);
int parse_condition(const char *name);

/* Function prototypes - Symbol table */
Symbol *symbol_find(AsmState *as, const char *name);
Symbol *symbol_add(AsmState *as, const char *name);
int symbol_define(AsmState *as, const char *name, uint24 value);
int symbol_set_export(AsmState *as, const char *name);
int symbol_set_extern(AsmState *as, const char *name);
int symbol_is_extern(AsmState *as, const char *name);
int symbol_is_local(const char *name);
void symbol_mangle_local(AsmState *as, const char *name, char *out, int max_len);

/* Function prototypes - Code generation */
void emit_byte(AsmState *as, uint8 b);
void emit_word(AsmState *as, uint24 w);
void emit_long(AsmState *as, uint24 l);
void emit_reloc(AsmState *as, uint8 type, const char *symbol);

/* Function prototypes - Main assembler */
int asm_init(AsmState *as);
void asm_free(AsmState *as);
int asm_line(AsmState *as, const char *line);
int asm_file(AsmState *as, const char *filename);
int asm_pass(AsmState *as, FILE *fp);
int asm_output(AsmState *as, const char *filename);

/* Function prototypes - Error handling */
void asm_error(AsmState *as, const char *fmt, ...);
void asm_warning(AsmState *as, const char *fmt, ...);

/* Function prototypes - Instructions */
int instr_lookup(const char *mnemonic);
int instr_execute(AsmState *as, const char *mnemonic);

/* Function prototypes - Directives */
int directive_execute(AsmState *as, const char *name);
int try_equ_directive(AsmState *as, const char *label);

/* Utility functions */
int is_8bit(int24 val);
int is_16bit(int24 val);
int is_24bit(int24 val);
int is_signed_8bit(int24 val);
int str_casecmp(const char *s1, const char *s2);

#endif /* EZ80ASM_H */
