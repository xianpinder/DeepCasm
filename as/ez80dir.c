/*
 * eZ80 ADL Mode Assembler - Directives and Main Loop
 * 
 * Part 3: Assembly directives and file I/O
 * C89 compatible, 24-bit integers
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "ez80asm.h"

/* Forward declarations */
static int dir_org(AsmState *as);
static int dir_equ(AsmState *as, const char *label);
static int dir_db(AsmState *as);
static int dir_dw(AsmState *as);
static int dir_dl(AsmState *as);
static int dir_ds(AsmState *as);
static int dir_section(AsmState *as);
static int dir_xdef(AsmState *as);
static int dir_xref(AsmState *as);
static int dir_end(AsmState *as);
static int dir_align(AsmState *as);
static int dir_ascii(AsmState *as);
static int dir_asciz(AsmState *as);
static int dir_assume(AsmState *as);
static int dir_include(AsmState *as);
static int dir_incbin(AsmState *as);

/* ============================================================
 * Directive Execution
 * ============================================================ */

int directive_execute(AsmState *as, const char *name)
{
    const char *dir;
    
    dir = name;
    if (dir[0] == '.') dir++;
    
    if (str_casecmp(dir, "org") == 0) return dir_org(as);
    if (str_casecmp(dir, "db") == 0 || 
        str_casecmp(dir, "defb") == 0 ||
        str_casecmp(dir, "byte") == 0) return dir_db(as);
    if (str_casecmp(dir, "dw") == 0 || 
        str_casecmp(dir, "defw") == 0 ||
        str_casecmp(dir, "word") == 0) return dir_dw(as);
    if (str_casecmp(dir, "dl") == 0 || 
        str_casecmp(dir, "defl") == 0 ||
        str_casecmp(dir, "long") == 0 ||
        str_casecmp(dir, "dd") == 0) return dir_dl(as);
    if (str_casecmp(dir, "ds") == 0 || 
        str_casecmp(dir, "defs") == 0 ||
        str_casecmp(dir, "rmb") == 0 ||
        str_casecmp(dir, "blkb") == 0) return dir_ds(as);
    if (str_casecmp(dir, "section") == 0 ||
        str_casecmp(dir, "segment") == 0) return dir_section(as);
    if (str_casecmp(dir, "xdef") == 0 ||
        str_casecmp(dir, "public") == 0 ||
        str_casecmp(dir, "global") == 0) return dir_xdef(as);
    if (str_casecmp(dir, "xref") == 0 ||
        str_casecmp(dir, "extern") == 0 ||
        str_casecmp(dir, "external") == 0) return dir_xref(as);
    if (str_casecmp(dir, "end") == 0) return dir_end(as);
    if (str_casecmp(dir, "align") == 0) return dir_align(as);
    if (str_casecmp(dir, "ascii") == 0) return dir_ascii(as);
    if (str_casecmp(dir, "asciz") == 0 ||
        str_casecmp(dir, "asciiz") == 0) return dir_asciz(as);
    if (str_casecmp(dir, "assume") == 0) return dir_assume(as);
    if (str_casecmp(dir, "include") == 0) return dir_include(as);
    if (str_casecmp(dir, "incbin") == 0) return dir_incbin(as);
    
    return -1;
}

int try_equ_directive(AsmState *as, const char *label)
{
    if (str_casecmp(as->current_token.text, "equ") == 0 ||
        str_casecmp(as->current_token.text, ".equ") == 0 ||
        as->current_token.type == TOK_EQUALS) {
        return dir_equ(as, label);
    }
    return -1;
}

/* ============================================================
 * Directive Implementations
 * ============================================================ */

static int dir_org(AsmState *as)
{
    int24 value;
    char symbol[MAX_LABEL_LEN];
    
    lexer_next(as);
    
    if (parse_expression(as, &value, symbol)) {
        asm_error(as, "ORG requires constant expression");
        return -1;
    }
    
    as->pc = value & 0xFFFFFF;
    return 0;
}

static int dir_equ(AsmState *as, const char *label)
{
    int24 value;
    char symbol[MAX_LABEL_LEN];
    uint8 saved_section;
    
    if (!label || label[0] == '\0') {
        asm_error(as, "EQU requires a label");
        return -1;
    }
    
    lexer_next(as);
    
    if (parse_expression(as, &value, symbol)) {
        if (as->pass == 2) {
            asm_error(as, "EQU requires constant expression");
            return -1;
        }
        value = 0;
    }
    
    /* EQU defines absolute symbols (section 0) */
    saved_section = as->current_section;
    as->current_section = 0;
    symbol_define(as, label, value);
    as->current_section = saved_section;
    return 0;
}

static int dir_db(AsmState *as)
{
    int24 value;
    char symbol[MAX_LABEL_LEN];
    int has_sym;
    const char *p;
    
    lexer_next(as);
    
    while (1) {
        if (as->current_token.type == TOK_STRING) {
            for (p = as->current_token.text; *p; p++) {
                emit_byte(as, (uint8)*p);
            }
            lexer_next(as);
        } else {
            has_sym = parse_expression(as, &value, symbol);
            if (has_sym) {
                asm_error(as, "DB cannot use relocatable symbols, use DL");
                return -1;
            }
            emit_byte(as, value & 0xFF);
        }
        
        if (as->current_token.type != TOK_COMMA) break;
        lexer_next(as);
    }
    
    return 0;
}

static int dir_dw(AsmState *as)
{
    int24 value;
    char symbol[MAX_LABEL_LEN];
    int has_sym;
    
    lexer_next(as);
    
    while (1) {
        has_sym = parse_expression(as, &value, symbol);
        if (has_sym) {
            asm_error(as, "DW cannot use relocatable symbols, use DL");
            return -1;
        }
        emit_word(as, value & 0xFFFF);
        
        if (as->current_token.type != TOK_COMMA) break;
        lexer_next(as);
    }
    
    return 0;
}

static int dir_dl(AsmState *as)
{
    int24 value;
    char symbol[MAX_LABEL_LEN];
    int has_sym;
    
    lexer_next(as);
    
    while (1) {
        has_sym = parse_expression(as, &value, symbol);
        if (has_sym) {
            emit_reloc(as, RELOC_ADDR24, symbol);
        }
        emit_long(as, value & 0xFFFFFF);
        
        if (as->current_token.type != TOK_COMMA) break;
        lexer_next(as);
    }
    
    return 0;
}

static int dir_ds(AsmState *as)
{
    int24 count, fill;
    char symbol[MAX_LABEL_LEN];
    int24 i;
    
    lexer_next(as);
    
    if (parse_expression(as, &count, symbol)) {
        asm_error(as, "DS requires constant expression");
        return -1;
    }
    
    fill = 0;
    if (as->current_token.type == TOK_COMMA) {
        lexer_next(as);
        parse_expression(as, &fill, symbol);
    }
    
    for (i = 0; i < count; i++) {
        emit_byte(as, fill & 0xFF);
    }
    
    return 0;
}

static int dir_section(AsmState *as)
{
    const char *name;
    
    lexer_next(as);
    
    if (as->current_token.type != TOK_IDENT) {
        asm_error(as, "SECTION requires name");
        return -1;
    }
    
    name = as->current_token.text;
    
    /* Save current section's PC before switching */
    switch (as->current_section) {
        case SECT_CODE: as->code_pc = as->pc; break;
        case SECT_DATA: as->data_pc = as->pc; break;
        case SECT_BSS:  as->bss_pc = as->pc; break;
    }
    
    if (str_casecmp(name, "code") == 0 ||
        str_casecmp(name, "text") == 0 ||
        str_casecmp(name, ".text") == 0) {
        as->current_section = SECT_CODE;
        as->pc = as->code_pc;
    }
    else if (str_casecmp(name, "data") == 0 ||
             str_casecmp(name, ".data") == 0) {
        as->current_section = SECT_DATA;
        as->pc = as->data_pc;
    }
    else if (str_casecmp(name, "bss") == 0 ||
             str_casecmp(name, ".bss") == 0) {
        as->current_section = SECT_BSS;
        as->pc = as->bss_pc;
    }
    else {
        asm_warning(as, "unknown section '%s', using CODE", name);
        as->current_section = SECT_CODE;
    }
    
    lexer_next(as);
    return 0;
}

static int dir_xdef(AsmState *as)
{
    lexer_next(as);
    
    while (as->current_token.type == TOK_IDENT) {
        if (symbol_is_local(as->current_token.text)) {
            asm_error(as, "local labels cannot be exported");
            return -1;
        }
        symbol_set_export(as, as->current_token.text);
        lexer_next(as);
        
        if (as->current_token.type != TOK_COMMA) break;
        lexer_next(as);
    }
    
    return 0;
}

static int dir_xref(AsmState *as)
{
    lexer_next(as);
    
    while (as->current_token.type == TOK_IDENT) {
        if (symbol_is_local(as->current_token.text)) {
            asm_error(as, "local labels cannot be external references");
            return -1;
        }
        symbol_set_extern(as, as->current_token.text);
        lexer_next(as);
        
        if (as->current_token.type != TOK_COMMA) break;
        lexer_next(as);
    }
    
    return 0;
}

static int dir_end(AsmState *as)
{
    (void)as;
    return 0;
}

static int dir_align(AsmState *as)
{
    int24 align;
    char symbol[MAX_LABEL_LEN];
    
    lexer_next(as);
    
    if (parse_expression(as, &align, symbol)) {
        asm_error(as, "ALIGN requires constant expression");
        return -1;
    }
    
    if (align <= 0 || (align & (align - 1)) != 0) {
        asm_error(as, "ALIGN must be power of 2");
        return -1;
    }
    
    while (as->pc & (align - 1)) {
        emit_byte(as, 0);
    }
    
    return 0;
}

static int dir_ascii(AsmState *as)
{
    const char *p;
    
    lexer_next(as);
    
    if (as->current_token.type != TOK_STRING) {
        asm_error(as, "ASCII requires string");
        return -1;
    }
    
    for (p = as->current_token.text; *p; p++) {
        emit_byte(as, (uint8)*p);
    }
    
    lexer_next(as);
    return 0;
}

static int dir_asciz(AsmState *as)
{
    int result;
    result = dir_ascii(as);
    if (result == 0) {
        emit_byte(as, 0);
    }
    return result;
}

static int dir_assume(AsmState *as)
{
    lexer_next(as);
    
    /* Expect "ADL" */
    if (as->current_token.type != TOK_IDENT ||
        str_casecmp(as->current_token.text, "ADL") != 0) {
        asm_error(as, "ASSUME expects ADL=0 or ADL=1");
        return -1;
    }
    
    lexer_next(as);
    
    /* Expect "=" */
    if (as->current_token.type != TOK_EQUALS) {
        asm_error(as, "ASSUME expects ADL=0 or ADL=1");
        return -1;
    }
    
    lexer_next(as);
    
    /* Expect 0 or 1 */
    if (as->current_token.type != TOK_NUMBER) {
        asm_error(as, "ASSUME expects ADL=0 or ADL=1");
        return -1;
    }
    
    if (as->current_token.value == 0) {
        asm_error(as, "this assembler only supports ADL mode (ADL=1)");
        return -1;
    } else if (as->current_token.value != 1) {
        asm_error(as, "ASSUME expects ADL=0 or ADL=1");
        return -1;
    }
    
    lexer_next(as);
    return 0;
}

static int dir_include(AsmState *as)
{
    FILE *fp;
    char line[1024];
    const char *saved_filename;
    int saved_line_num;
    char filename[256];
    int i;
    
    lexer_next(as);
    
    if (as->current_token.type != TOK_STRING) {
        asm_error(as, "INCLUDE requires filename string");
        return -1;
    }
    
    /* Copy filename */
    for (i = 0; i < 255 && as->current_token.text[i]; i++) {
        filename[i] = as->current_token.text[i];
    }
    filename[i] = '\0';
    
    lexer_next(as);
    
    /* Open included file */
    fp = fopen(filename, "r");
    if (!fp) {
        asm_error(as, "cannot open include file '%s'", filename);
        return -1;
    }
    
    /* Save current file context */
    saved_filename = as->filename;
    saved_line_num = as->line_num;
    
    /* Process included file */
    as->filename = filename;
    as->line_num = 0;
    
    while (fgets(line, sizeof(line), fp)) {
        int len;
        as->line_num++;
        
        len = strlen(line);
        
        /* Check for line too long */
        if (len == sizeof(line) - 1 && line[len-1] != '\n') {
            asm_error(as, "line too long (max %d characters)", 
                      (int)sizeof(line) - 2);
            /* Skip rest of the line */
            while ((len = fgetc(fp)) != EOF && len != '\n')
                ;
        }
        
        asm_line(as, line);
    }
    
    fclose(fp);
    
    /* Restore file context */
    as->filename = saved_filename;
    as->line_num = saved_line_num;
    
    return 0;
}

static int dir_incbin(AsmState *as)
{
    FILE *fp;
    char filename[256];
    int i;
    int c;
    
    lexer_next(as);
    
    if (as->current_token.type != TOK_STRING) {
        asm_error(as, "INCBIN requires filename string");
        return -1;
    }
    
    /* Copy filename */
    for (i = 0; i < 255 && as->current_token.text[i]; i++) {
        filename[i] = as->current_token.text[i];
    }
    filename[i] = '\0';
    
    lexer_next(as);
    
    /* Open binary file */
    fp = fopen(filename, "rb");
    if (!fp) {
        asm_error(as, "cannot open binary file '%s'", filename);
        return -1;
    }
    
    /* Read and emit each byte */
    while ((c = fgetc(fp)) != EOF) {
        emit_byte(as, (uint8)c);
    }
    
    fclose(fp);
    
    return 0;
}

/* ============================================================
 * Line Processing
 * ============================================================ */

int asm_line(AsmState *as, const char *line)
{
    char label[MAX_LABEL_LEN];
    char mangled[MAX_LABEL_LEN];
    char mnemonic[MAX_LABEL_LEN];
    Token *peek;
    int i;
    int is_local;
    int is_equ_line = 0;
    
    label[0] = '\0';
    
    lexer_init(as, line);
    lexer_next(as);
    
    if (as->current_token.type == TOK_EOL || 
        as->current_token.type == TOK_EOF) {
        return 0;
    }
    
    /* Check for label */
    if (as->current_token.type == TOK_LABEL) {
        for (i = 0; i < MAX_LABEL_LEN - 1 && as->current_token.text[i]; i++) {
            label[i] = as->current_token.text[i];
        }
        label[i] = '\0';
        lexer_next(as);
        
        /* Check if this is an EQU line - if so, don't define at PC */
        if (as->current_token.type == TOK_IDENT &&
            (str_casecmp(as->current_token.text, "equ") == 0 ||
             str_casecmp(as->current_token.text, ".equ") == 0)) {
            is_equ_line = 1;
        } else if (as->current_token.type == TOK_EQUALS) {
            is_equ_line = 1;
        }
        
        if (!is_equ_line) {
            /* Handle local vs global labels */
            is_local = symbol_is_local(label);
            if (is_local) {
                symbol_mangle_local(as, label, mangled, MAX_LABEL_LEN);
                symbol_define(as, mangled, as->pc);
            } else {
                symbol_define(as, label, as->pc);
                as->local_scope++;  /* New scope after global label */
            }
        }
    }
    else if (as->current_token.type == TOK_IDENT) {
        peek = lexer_peek(as);
        if (peek->type == TOK_COLON) {
            for (i = 0; i < MAX_LABEL_LEN - 1 && as->current_token.text[i]; i++) {
                label[i] = as->current_token.text[i];
            }
            label[i] = '\0';
            lexer_next(as);  /* skip label */
            lexer_next(as);  /* skip colon */
            
            /* Check if this is an EQU line - if so, don't define at PC */
            if (as->current_token.type == TOK_IDENT &&
                (str_casecmp(as->current_token.text, "equ") == 0 ||
                 str_casecmp(as->current_token.text, ".equ") == 0)) {
                is_equ_line = 1;
            } else if (as->current_token.type == TOK_EQUALS) {
                is_equ_line = 1;
            }
            
            if (!is_equ_line) {
                /* Handle local vs global labels */
                is_local = symbol_is_local(label);
                if (is_local) {
                    symbol_mangle_local(as, label, mangled, MAX_LABEL_LEN);
                    symbol_define(as, mangled, as->pc);
                } else {
                    symbol_define(as, label, as->pc);
                    as->local_scope++;  /* New scope after global label */
                }
            }
        }
        else if (peek->type == TOK_EQUALS) {
            /* label = value syntax for EQU */
            for (i = 0; i < MAX_LABEL_LEN - 1 && as->current_token.text[i]; i++) {
                label[i] = as->current_token.text[i];
            }
            label[i] = '\0';
            lexer_next(as);  /* move to = */
            /* Don't consume =, let dir_equ handle it */
        }
        else if (peek->type == TOK_IDENT &&
                 (str_casecmp(peek->text, "equ") == 0 ||
                  str_casecmp(peek->text, ".equ") == 0)) {
            /* label equ value syntax (no colon) */
            for (i = 0; i < MAX_LABEL_LEN - 1 && as->current_token.text[i]; i++) {
                label[i] = as->current_token.text[i];
            }
            label[i] = '\0';
            lexer_next(as);  /* move to equ */
            /* Let try_equ_directive handle it */
        }
    }
    
    if (as->current_token.type == TOK_EOL || 
        as->current_token.type == TOK_EOF) {
        return 0;
    }
    
    /* Handle = as EQU (label = value already extracted label) */
    if (as->current_token.type == TOK_EQUALS) {
        if (label[0] == '\0') {
            asm_error(as, "= requires a label");
            return -1;
        }
        return dir_equ(as, label);
    }
    
    if (as->current_token.type != TOK_IDENT) {
        asm_error(as, "expected instruction or directive");
        return -1;
    }
    
    for (i = 0; i < MAX_LABEL_LEN - 1 && as->current_token.text[i]; i++) {
        mnemonic[i] = as->current_token.text[i];
    }
    mnemonic[i] = '\0';
    
    if (try_equ_directive(as, label) == 0) {
        return 0;
    }
    
    if (instr_execute(as, mnemonic) == 0) {
        return 0;
    }
    
    {
        int errors_before = as->errors;
        if (directive_execute(as, mnemonic) == 0) {
            return 0;
        }
        /* Only print "unknown" if directive didn't report its own error */
        if (as->errors > errors_before) {
            return -1;
        }
    }
    
    asm_error(as, "unknown instruction or directive '%s'", mnemonic);
    return -1;
}

/* ============================================================
 * Pass Processing
 * ============================================================ */

int asm_pass(AsmState *as, FILE *fp)
{
    char line[MAX_LINE_LEN];
    int len;
    
    as->line_num = 0;
    
    while (fgets(line, sizeof(line), fp)) {
        as->line_num++;
        
        len = strlen(line);
        
        /* Check for line too long (buffer full but no newline) */
        if (len == MAX_LINE_LEN - 1 && line[len-1] != '\n') {
            asm_error(as, "line too long (max %d characters)", MAX_LINE_LEN - 2);
            /* Skip rest of the line */
            while ((len = fgetc(fp)) != EOF && len != '\n')
                ;
        }
        
        if (len > 0 && line[len-1] == '\n') {
            line[len-1] = '\0';
        }
        
        asm_line(as, line);
    }
    
    return as->errors;
}

/* ============================================================
 * File Processing
 * ============================================================ */

int asm_file(AsmState *as, const char *filename)
{
    FILE *fp;
    
    fp = fopen(filename, "r");
    if (!fp) {
        fprintf(stderr, "error: cannot open '%s'\n", filename);
        return -1;
    }
    
    as->filename = filename;
    
    /* Pass 1 */
    as->pass = 1;
    as->pc = 0;
    as->code_size = 0;
    as->data_size = 0;
    as->bss_size = 0;
    as->num_relocs = 0;
    as->current_section = SECT_CODE;
    as->local_scope = 0;
    as->code_pc = 0;
    as->data_pc = 0;
    as->bss_pc = 0;
    
    asm_pass(as, fp);
    
    if (as->errors > 0) {
        fclose(fp);
        return -1;
    }
    
    /* Open temp files for pass 2 */
    as->code_tmp = tmpfile();
    as->data_tmp = tmpfile();
    as->reloc_tmp = tmpfile();
    
    if (!as->code_tmp || !as->data_tmp || !as->reloc_tmp) {
        fprintf(stderr, "error: cannot create temporary files\n");
        fclose(fp);
        return -1;
    }
    
    /* Pass 2 */
    rewind(fp);
    as->pass = 2;
    as->pc = 0;
    as->code_size = 0;
    as->data_size = 0;
    as->num_relocs = 0;
    as->current_section = SECT_CODE;
    as->local_scope = 0;
    as->code_pc = 0;
    as->data_pc = 0;
    as->bss_pc = 0;
    
    asm_pass(as, fp);
    
    fclose(fp);
    return as->errors;
}

/* ============================================================
 * Object File Output
 * ============================================================ */

/* Copy n bytes from src file to dest file */
static int copy_file_data(FILE *dest, FILE *src, uint24 nbytes)
{
    int ch;
    uint24 i;
    
    rewind(src);
    for (i = 0; i < nbytes; i++) {
        ch = fgetc(src);
        if (ch == EOF) return -1;
        fputc(ch, dest);
    }
    return 0;
}

int asm_output(AsmState *as, const char *filename)
{
    FILE *fp;
    FILE *strtab_tmp;
    ObjHeader header;
    ObjSymbol obj_sym;
    ObjReloc obj_reloc;
    ObjExtern obj_ext;
    Relocation reloc;
    int num_obj_symbols;
    int i;
    uint24 strtab_size;
    uint24 name_off;
    const char *p;
    
    fp = fopen(filename, "wb");
    if (!fp) {
        fprintf(stderr, "error: cannot create '%s'\n", filename);
        return -1;
    }
    
    /* Create temp file for string table */
    strtab_tmp = tmpfile();
    if (!strtab_tmp) {
        fprintf(stderr, "error: cannot create temp file for strings\n");
        fclose(fp);
        return -1;
    }
    
    strtab_size = 0;
    
    /* Count exported symbols only */
    num_obj_symbols = 0;
    for (i = 0; i < as->num_symbols; i++) {
        if (as->symbols[i].flags == SYM_EXPORT) {
            num_obj_symbols++;
        }
    }
    
    /* Write header placeholder */
    memset(&header, 0, sizeof(header));
    fwrite(&header, sizeof(header), 1, fp);
    
    /* Write code section from temp file */
    if (as->code_size > 0 && as->code_tmp) {
        copy_file_data(fp, as->code_tmp, as->code_size);
    }
    
    /* Write data section from temp file */
    if (as->data_size > 0 && as->data_tmp) {
        copy_file_data(fp, as->data_tmp, as->data_size);
    }
    
    /* Write symbol table - exported symbols only */
    for (i = 0; i < as->num_symbols; i++) {
        Symbol *sym = &as->symbols[i];
        if (sym->flags != SYM_EXPORT) continue;
        
        memset(&obj_sym, 0, sizeof(obj_sym));
        
        /* Add name to string table */
        name_off = strtab_size;
        for (p = sym->name; *p; p++) {
            fputc(*p, strtab_tmp);
            strtab_size++;
        }
        fputc(0, strtab_tmp);
        strtab_size++;
        
        WRITE24(obj_sym.name_offset, name_off);
        obj_sym.section = sym->section;
        obj_sym.flags = sym->flags;
        WRITE24(obj_sym.value, sym->value);
        
        fwrite(&obj_sym, sizeof(obj_sym), 1, fp);
    }
    
    /* Write relocation table from temp file */
    if (as->num_relocs > 0 && as->reloc_tmp) {
        rewind(as->reloc_tmp);
        for (i = 0; i < (int)as->num_relocs; i++) {
            if (fread(&reloc, sizeof(reloc), 1, as->reloc_tmp) != 1) {
                fprintf(stderr, "error: reading relocation temp file\n");
                break;
            }
            
            memset(&obj_reloc, 0, sizeof(obj_reloc));
            WRITE24(obj_reloc.offset, reloc.offset);
            obj_reloc.section = reloc.section;
            obj_reloc.type = reloc.type;
            obj_reloc.target_sect = reloc.target_sect;
            obj_reloc.ext_index[0] = reloc.ext_index & 0xFF;
            obj_reloc.ext_index[1] = (reloc.ext_index >> 8) & 0xFF;
            
            fwrite(&obj_reloc, sizeof(obj_reloc), 1, fp);
        }
    }
    
    /* Write external references */
    for (i = 0; i < as->num_externs; i++) {
        memset(&obj_ext, 0, sizeof(obj_ext));
        
        /* Add name to string table */
        name_off = strtab_size;
        for (p = as->externs[i]; *p; p++) {
            fputc(*p, strtab_tmp);
            strtab_size++;
        }
        fputc(0, strtab_tmp);
        strtab_size++;
        
        WRITE24(obj_ext.name_offset, name_off);
        WRITE24(obj_ext.symbol_index, i);
        fwrite(&obj_ext, sizeof(obj_ext), 1, fp);
    }
    
    /* Write string table from temp file */
    if (strtab_size > 0) {
        copy_file_data(fp, strtab_tmp, strtab_size);
    }
    
    fclose(strtab_tmp);
    
    /* Write final header */
    memset(&header, 0, sizeof(header));
    header.magic[0] = OBJ_MAGIC_0;
    header.magic[1] = OBJ_MAGIC_1;
    header.magic[2] = OBJ_MAGIC_2;
    header.magic[3] = OBJ_MAGIC_3;
    header.version = OBJ_VERSION;
    header.flags = 0;
    WRITE24(header.code_size, as->code_size);
    WRITE24(header.data_size, as->data_size);
    WRITE24(header.bss_size, as->bss_size);
    WRITE24(header.num_symbols, num_obj_symbols);
    WRITE24(header.num_relocs, as->num_relocs);
    WRITE24(header.num_externs, as->num_externs);
    WRITE24(header.strtab_size, strtab_size);
    
    fseek(fp, 0, SEEK_SET);
    fwrite(&header, sizeof(header), 1, fp);
    
    fclose(fp);
    
    if (as->verbose) {
        printf("Output: %s\n", filename);
        printf("  Code: %u bytes\n", (unsigned)as->code_size);
        printf("  Data: %u bytes\n", (unsigned)as->data_size);
        printf("  BSS:  %u bytes\n", (unsigned)as->bss_size);
        printf("  Symbols: %d\n", num_obj_symbols);
        printf("  Relocations: %d\n", (int)as->num_relocs);
        printf("  Externals: %d\n", as->num_externs);
    }
    
    return 0;
}
