/*
 * eZ80 Object File Format
 * 
 * Simple object format for linking eZ80 ADL mode programs.
 * All multi-byte values are little-endian.
 * All addresses and offsets are 24-bit.
 *
 * Designed for eZ80 C compiler with 24-bit int/long.
 * C89 compatible.
 */

#ifndef OBJFORMAT_H
#define OBJFORMAT_H

/* 
 * Type definitions for portability
 * On eZ80 ADL mode: char=8bit, int=24bit, long=24bit
 */
typedef unsigned char  uint8;
typedef signed char    int8;
typedef unsigned int   uint24;
typedef signed int     int24;

/* Magic number: "EZ8O" stored as bytes */
#define OBJ_MAGIC_0     0x45
#define OBJ_MAGIC_1     0x5A
#define OBJ_MAGIC_2     0x38
#define OBJ_MAGIC_3     0x4F

/* Object file version */
#define OBJ_VERSION     3

/* Section types */
#define SECT_CODE       0x01
#define SECT_DATA       0x02
#define SECT_BSS        0x03

/* Symbol flags */
#define SYM_LOCAL       0x00
#define SYM_EXPORT      0x01
#define SYM_EXTERN      0x02

/* Relocation types */
#define RELOC_ADDR24    0x01    /* 24-bit absolute address */

/*
 * Object File Header (27 bytes)
 */
typedef struct {
    uint8 magic[4];         /* OBJ_MAGIC bytes */
    uint8 version;          /* OBJ_VERSION */
    uint8 flags;            /* Reserved for future use */
    uint8 code_size[3];     /* Size of code section (24-bit LE) */
    uint8 data_size[3];     /* Size of data section (24-bit LE) */
    uint8 bss_size[3];      /* Size of BSS section (24-bit LE) */
    uint8 num_symbols[3];   /* Number of symbols (24-bit LE) */
    uint8 num_relocs[3];    /* Number of relocations (24-bit LE) */
    uint8 num_externs[3];   /* Number of external references (24-bit LE) */
    uint8 strtab_size[3];   /* Size of string table (24-bit LE) */
} ObjHeader;

/*
 * Symbol Table Entry (10 bytes)
 */
typedef struct {
    uint8 name_offset[3];   /* Offset into string table (24-bit LE) */
    uint8 section;          /* Section (SECT_CODE, etc.) */
    uint8 flags;            /* SYM_LOCAL, SYM_EXPORT, SYM_EXTERN */
    uint8 value[3];         /* Value/address (24-bit LE) */
    uint8 reserved[2];      /* Padding for alignment */
} ObjSymbol;

/*
 * Relocation Entry (8 bytes)
 * 
 * For local references: target_sect = CODE/DATA/BSS, value at offset already correct
 * For external refs: target_sect = 0, ext_index contains external symbol index
 */
typedef struct {
    uint8 offset[3];        /* Offset in section where reloc applies (24-bit LE) */
    uint8 section;          /* Section containing the relocation */
    uint8 type;             /* Relocation type (RELOC_*) */
    uint8 target_sect;      /* Target section (0=external, 1=CODE, 2=DATA, 3=BSS) */
    uint8 ext_index[2];     /* External index if target_sect==0 (16-bit LE) */
} ObjReloc;

/*
 * External Reference Entry (6 bytes)
 */
typedef struct {
    uint8 name_offset[3];   /* Offset into string table (24-bit LE) */
    uint8 symbol_index[3];  /* Index assigned to this external (24-bit LE) */
} ObjExtern;

/*
 * Helper macros for multi-byte values
 */
#define WRITE24(arr, val) \
    (arr)[0] = (uint8)((val) & 0xFF), \
    (arr)[1] = (uint8)(((val) >> 8) & 0xFF), \
    (arr)[2] = (uint8)(((val) >> 16) & 0xFF)

#define READ24(arr) \
    ((uint24)(arr)[0] | ((uint24)(arr)[1] << 8) | ((uint24)(arr)[2] << 16))

#endif /* OBJFORMAT_H */
