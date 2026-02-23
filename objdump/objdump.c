/*
 * eZ80 Object File Dump Utility
 * 
 * Displays contents of eZ80 object files in human-readable format.
 * C89 compatible.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "objformat.h"

static const char *section_name(uint8 sect)
{
    switch (sect) {
        case 0:         return "ABS";
        case SECT_CODE: return "CODE";
        case SECT_DATA: return "DATA";
        case SECT_BSS:  return "BSS";
        default:        return "???";
    }
}

static const char *symbol_flags(uint8 flags)
{
    switch (flags) {
        case SYM_LOCAL:  return "LOCAL";
        case SYM_EXPORT: return "EXPORT";
        case SYM_EXTERN: return "EXTERN";
        default:         return "???";
    }
}

static const char *reloc_type(uint8 type)
{
    switch (type) {
        case RELOC_ADDR24: return "ADDR24";
        default:           return "???";
    }
}

static void dump_hex(FILE *fp, uint24 size)
{
    int ch;
    uint24 addr;
    char ascii[17];
    int col;
    
    if (size == 0) {
        printf("  (empty)\n");
        return;
    }
    
    addr = 0;
    col = 0;
    
    while (addr < size) {
        if (col == 0) {
            printf("  %06X: ", (unsigned)addr);
        }
        
        ch = fgetc(fp);
        if (ch == EOF) break;
        
        printf("%02X ", ch);
        ascii[col] = (ch >= 32 && ch < 127) ? ch : '.';
        
        col++;
        addr++;
        
        if (col == 16 || addr == size) {
            ascii[col] = '\0';
            /* Pad if needed */
            while (col < 16) {
                printf("   ");
                col++;
            }
            printf(" |%s|\n", ascii);
            col = 0;
        }
    }
}

static int dump_object(const char *filename)
{
    FILE *fp;
    ObjHeader header;
    ObjSymbol sym;
    ObjReloc reloc;
    ObjExtern ext;
    uint24 code_size, data_size, bss_size;
    uint24 num_symbols, num_relocs, num_externs, strtab_size;
    char *strtab;
    long strtab_offset;
    uint24 i;
    uint24 name_off, value, offset, sym_idx;
    
    fp = fopen(filename, "rb");
    if (!fp) {
        fprintf(stderr, "error: cannot open '%s'\n", filename);
        return -1;
    }
    
    /* Read header */
    if (fread(&header, sizeof(header), 1, fp) != 1) {
        fprintf(stderr, "error: cannot read header\n");
        fclose(fp);
        return -1;
    }
    
    /* Verify magic */
    if (header.magic[0] != OBJ_MAGIC_0 || header.magic[1] != OBJ_MAGIC_1 ||
        header.magic[2] != OBJ_MAGIC_2 || header.magic[3] != OBJ_MAGIC_3) {
        fprintf(stderr, "error: not a valid object file\n");
        fclose(fp);
        return -1;
    }
    
    /* Extract header fields */
    code_size = READ24(header.code_size);
    data_size = READ24(header.data_size);
    bss_size = READ24(header.bss_size);
    num_symbols = READ24(header.num_symbols);
    num_relocs = READ24(header.num_relocs);
    num_externs = READ24(header.num_externs);
    strtab_size = READ24(header.strtab_size);
    
    /* Print header */
    printf("=== Object File: %s ===\n\n", filename);
    printf("Header:\n");
    printf("  Magic:       %c%c%c%c\n", 
           header.magic[0], header.magic[1], header.magic[2], header.magic[3]);
    printf("  Version:     %d\n", header.version);
    printf("  Flags:       0x%02X\n", header.flags);
    printf("  Code size:   %u bytes\n", (unsigned)code_size);
    printf("  Data size:   %u bytes\n", (unsigned)data_size);
    printf("  BSS size:    %u bytes\n", (unsigned)bss_size);
    printf("  Symbols:     %u\n", (unsigned)num_symbols);
    printf("  Relocations: %u\n", (unsigned)num_relocs);
    printf("  Externals:   %u\n", (unsigned)num_externs);
    printf("  String tab:  %u bytes\n", (unsigned)strtab_size);
    printf("\n");
    
    /* Calculate string table offset and read it */
    strtab_offset = sizeof(header) + code_size + data_size +
                    (num_symbols * sizeof(ObjSymbol)) +
                    (num_relocs * sizeof(ObjReloc)) +
                    (num_externs * sizeof(ObjExtern));
    
    strtab = NULL;
    if (strtab_size > 0) {
        strtab = (char *)malloc(strtab_size);
        if (strtab) {
            fseek(fp, strtab_offset, SEEK_SET);
            if (fread(strtab, 1, strtab_size, fp) != strtab_size) {
                fprintf(stderr, "warning: could not read string table\n");
                free(strtab);
                strtab = NULL;
            }
        }
    }
    
    /* Dump code section */
    printf("Code Section:\n");
    fseek(fp, sizeof(header), SEEK_SET);
    dump_hex(fp, code_size);
    printf("\n");
    
    /* Dump data section */
    printf("Data Section:\n");
    dump_hex(fp, data_size);
    printf("\n");
    
    /* Dump BSS info */
    printf("BSS Section:\n");
    if (bss_size > 0) {
        printf("  %u bytes (uninitialized)\n", (unsigned)bss_size);
    } else {
        printf("  (empty)\n");
    }
    printf("\n");
    
    /* Dump symbol table */
    printf("Symbol Table:\n");
    if (num_symbols == 0) {
        printf("  (empty)\n");
    } else {
        printf("  %-6s %-8s %-8s %-6s %s\n", "Index", "Value", "Section", "Flags", "Name");
        printf("  %-6s %-8s %-8s %-6s %s\n", "-----", "--------", "--------", "------", "----");
        
        fseek(fp, sizeof(header) + code_size + data_size, SEEK_SET);
        
        for (i = 0; i < num_symbols; i++) {
            if (fread(&sym, sizeof(sym), 1, fp) != 1) break;
            
            name_off = READ24(sym.name_offset);
            value = READ24(sym.value);
            
            printf("  %-6u %06X   %-8s %-6s %s\n",
                   (unsigned)i,
                   (unsigned)value,
                   section_name(sym.section),
                   symbol_flags(sym.flags),
                   (strtab && name_off < strtab_size) ? &strtab[name_off] : "???");
        }
    }
    printf("\n");
    
    /* Dump relocation table */
    printf("Relocation Table:\n");
    if (num_relocs == 0) {
        printf("  (empty)\n");
    } else {
        printf("  %-6s %-8s %-8s %-8s %s\n", "Index", "Offset", "Section", "Type", "Target");
        printf("  %-6s %-8s %-8s %-8s %s\n", "-----", "--------", "--------", "--------", "------");
        
        for (i = 0; i < num_relocs; i++) {
            unsigned ext_idx;
            
            if (fread(&reloc, sizeof(reloc), 1, fp) != 1) break;
            
            offset = READ24(reloc.offset);
            ext_idx = reloc.ext_index[0] | (reloc.ext_index[1] << 8);
            
            printf("  %-6u %06X   %-8s %-8s ",
                   (unsigned)i,
                   (unsigned)offset,
                   section_name(reloc.section),
                   reloc_type(reloc.type));
            
            if (reloc.target_sect == 0) {
                printf("EXT:%u", ext_idx);
            } else {
                printf("%s", section_name(reloc.target_sect));
            }
            printf("\n");
        }
    }
    printf("\n");
    
    /* Dump external references */
    printf("External References:\n");
    if (num_externs == 0) {
        printf("  (empty)\n");
    } else {
        printf("  %-6s %s\n", "Index", "Name");
        printf("  %-6s %s\n", "-----", "----");
        
        for (i = 0; i < num_externs; i++) {
            if (fread(&ext, sizeof(ext), 1, fp) != 1) break;
            
            name_off = READ24(ext.name_offset);
            sym_idx = READ24(ext.symbol_index);
            
            printf("  %-6u %s\n",
                   (unsigned)sym_idx,
                   (strtab && name_off < strtab_size) ? &strtab[name_off] : "???");
        }
    }
    printf("\n");
    
    /* Dump string table */
    printf("String Table:\n");
    if (strtab_size == 0) {
        printf("  (empty)\n");
    } else {
        uint24 off = 0;
        while (off < strtab_size) {
            printf("  %06X: \"%s\"\n", (unsigned)off, &strtab[off]);
            off += strlen(&strtab[off]) + 1;
        }
    }
    printf("\n");
    
    if (strtab) free(strtab);
    fclose(fp);
    
    return 0;
}

int main(int argc, char *argv[])
{
    int i;
    
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <object-file> [...]\n", argv[0]);
        return 1;
    }
    
    for (i = 1; i < argc; i++) {
        if (i > 1) printf("\n");
        dump_object(argv[i]);
    }
    
    return 0;
}
