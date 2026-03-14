/*
 * eZ80 Linker
 * 
 * Links eZ80 object files into a flat binary.
 * C89 compatible with 24-bit integers.
 *
 * Optimised to reduce file I/O: string tables and extern tables
 * are cached in memory, fread replaces fgetc loops, and file
 * opens are merged where possible.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "objformat.h"

/* Maximum limits */
#define MAX_OBJECTS     128
#define MAX_SYMBOLS     2048
#define MAX_EXTERNS     1024
#define MAX_FILENAME    256
#define MAX_LIBRARIES   16
#define MAX_LIB_OBJECTS 256
#define MAX_LIBDIRS     16  /* Library search directories */
#define MAX_OBJ_EXTERNS 256 /* Max externals per single object */
#define HASH_SIZE       256 /* Symbol hash table buckets (power of 2) */
#define LIB_HASH_SIZE   256 /* Library index hash buckets */
#define MAX_LIB_SYMS    1024 /* Max exported symbols across all libraries */
#define MAX_SYM_NAME    64  /* Symbol name length (including '\0') */
#define MAX_LIBSYM_NAME 32  /* Library index name length (including '\0') */

#define LINKER_DEFINED  -1  /* obj_index for linker-defined symbols */

/*
 * Library symbol index entry.
 * Maps an exported symbol name to the library object that defines it.
 *
 * Memory: 37 bytes per entry on eZ80 (int=3 bytes).
 * At MAX_LIB_SYMS=1024: ~37KB total (dynamically allocated, freed
 * after library resolution).
 */
typedef struct {
    char name[MAX_LIBSYM_NAME];    /* Symbol name (truncated) */
    uint8 lib_idx;              /* Index into ls->libraries[] (max 16) */
    uint8 obj_idx;              /* Index into lib->objects[] (max 256) */
    int hash_next;              /* Next entry in hash chain (-1 = end) */
} LibSymEntry;

/*
 * Library symbol index.
 * Built once when processing libraries; maps every exported symbol
 * in every library object to its location.  Allocated dynamically
 * and freed after library resolution is complete.
 */
typedef struct {
    LibSymEntry *entries;
    int num_entries;
    int max_entries;
    int hash_buckets[LIB_HASH_SIZE];
} LibSymIndex;

/* Library object entry (for scanning libraries) */
typedef struct {
    long offset;            /* File offset to this object */
    uint24 obj_size;        /* Size of object in file */
    int loaded;             /* Already loaded? */
} LibObject;

/* Library info */
typedef struct {
    char filename[MAX_FILENAME];
    LibObject objects[MAX_LIB_OBJECTS];
    int num_objects;
} LibraryInfo;

/* Read 24-bit little-endian value */
#define READ24(arr) ((uint24)(arr)[0] | ((uint24)(arr)[1] << 8) | ((uint24)(arr)[2] << 16))

/* Read 16-bit little-endian value */
#define READ16(arr) ((unsigned)(arr)[0] | ((unsigned)(arr)[1] << 8))

/* Global symbol entry */
typedef struct {
    char name[MAX_SYM_NAME];
    uint24 value;           /* Absolute address after linking */
    uint8 section;          /* Original section */
    int obj_index;          /* Which object file it came from */
    int hash_next;          /* Next symbol index in hash chain (-1 = end) */
} GlobalSymbol;

/* Object file info */
typedef struct {
    char filename[MAX_FILENAME];
    uint24 code_size;
    uint24 data_size;
    uint24 bss_size;
    uint24 num_symbols;
    uint24 num_relocs;
    uint24 num_externs;
    uint24 strtab_size;
    
    /* Base addresses assigned during linking */
    uint24 code_base;
    uint24 data_base;
    uint24 bss_base;
    
    /* File positions for reading sections */
    long code_pos;
    long data_pos;
    long sym_pos;
    long reloc_pos;
    long extern_pos;
    long strtab_pos;
} ObjectInfo;

/* Linker state */
typedef struct {
    ObjectInfo objects[MAX_OBJECTS];
    int num_objects;
    
    GlobalSymbol symbols[MAX_SYMBOLS];
    int num_symbols;
    int hash_buckets[HASH_SIZE]; /* Hash table: each bucket holds index into
                                    symbols[] or -1 if empty */
    
    LibraryInfo libraries[MAX_LIBRARIES];
    int num_libraries;
    
    char libdirs[MAX_LIBDIRS][MAX_FILENAME];
    int num_libdirs;
    
    uint24 base_addr;
    uint24 total_code;
    uint24 total_data;
    uint24 total_bss;
    
    char *output_file;
    char *map_file;
    int verbose;
    int errors;
} LinkerState;

/* Function prototypes */
static int load_object(LinkerState *ls, const char *filename);
static int load_object_at(LinkerState *ls, const char *filename, long offset);
static int add_libdir(LinkerState *ls, const char *dir);
static int add_library(LinkerState *ls, const char *filename);
static int find_and_add_library(LinkerState *ls, const char *name);
static int process_libraries(LinkerState *ls);
static int resolve_symbols(LinkerState *ls);
static int link_output(LinkerState *ls);
static int write_map(LinkerState *ls);
static GlobalSymbol *find_global(LinkerState *ls, const char *name);
static int add_global(LinkerState *ls, const char *name, uint24 value, 
                      uint8 section, int obj_index);

/* Case-insensitive string compare */
static int str_casecmp(const char *a, const char *b)
{
    while (*a && *b) {
        unsigned char ca = tolower((unsigned char)*a);
        unsigned char cb = tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

/* String copy with length limit */
static void str_copy(char *dest, const char *src, int max)
{
    int i;
    for (i = 0; i < max - 1 && src[i]; i++) {
        dest[i] = src[i];
    }
    dest[i] = '\0';
}

/* Case-insensitive hash (djb2 variant) */
static unsigned sym_hash(const char *name)
{
    unsigned h = 5381;
    while (*name) {
        unsigned char c = tolower((unsigned char)*name++);
        h = ((h << 5) + h) ^ c;
    }
    return h & (HASH_SIZE - 1);
}

/* Find a global symbol by name (hash table lookup) */
static GlobalSymbol *find_global(LinkerState *ls, const char *name)
{
    int idx = ls->hash_buckets[sym_hash(name)];
    while (idx >= 0) {
        if (str_casecmp(ls->symbols[idx].name, name) == 0) {
            return &ls->symbols[idx];
        }
        idx = ls->symbols[idx].hash_next;
    }
    return NULL;
}

/* Add a global symbol (with hash table insertion) */
static int add_global(LinkerState *ls, const char *name, uint24 value,
                      uint8 section, int obj_index)
{
    GlobalSymbol *existing;
    unsigned bucket;
    
    existing = find_global(ls, name);
    if (existing) {
        fprintf(stderr, "error: duplicate symbol '%s' in '%s' and '%s'\n",
                name, ls->objects[existing->obj_index].filename,
                ls->objects[obj_index].filename);
        ls->errors++;
        return -1;
    }
    
    if (ls->num_symbols >= MAX_SYMBOLS) {
        fprintf(stderr, "error: too many symbols\n");
        ls->errors++;
        return -1;
    }
    
    str_copy(ls->symbols[ls->num_symbols].name, name, MAX_SYM_NAME);
    ls->symbols[ls->num_symbols].value = value;
    ls->symbols[ls->num_symbols].section = section;
    ls->symbols[ls->num_symbols].obj_index = obj_index;
    
    /* Insert at head of hash chain */
    bucket = sym_hash(name);
    ls->symbols[ls->num_symbols].hash_next = ls->hash_buckets[bucket];
    ls->hash_buckets[bucket] = ls->num_symbols;
    
    ls->num_symbols++;
    
    return 0;
}

/* Load an object file from a specific offset (for library support) */
static int load_object_at(LinkerState *ls, const char *filename, long offset)
{
    FILE *fp;
    ObjHeader header;
    ObjSymbol sym;
    ObjectInfo *obj;
    char *strtab;
    uint24 name_off;
    uint24 value;
    int i;
    
    if (ls->num_objects >= MAX_OBJECTS) {
        fprintf(stderr, "error: too many object files\n");
        return -1;
    }
    
    fp = fopen(filename, "rb");
    if (!fp) {
        fprintf(stderr, "error: cannot open '%s'\n", filename);
        return -1;
    }
    
    /* Seek to offset for library support */
    if (offset > 0) {
        fseek(fp, offset, SEEK_SET);
    }
    
    /* Read header */
    if (fread(&header, sizeof(header), 1, fp) != 1) {
        fprintf(stderr, "error: cannot read header from '%s'\n", filename);
        fclose(fp);
        return -1;
    }
    
    /* Verify magic */
    if (header.magic[0] != 'E' || header.magic[1] != 'Z' ||
        header.magic[2] != '8' || header.magic[3] != 'O') {
        fprintf(stderr, "error: '%s' is not a valid object file\n", filename);
        fclose(fp);
        return -1;
    }
    
    /* Check version */
    if (header.version != 3) {
        fprintf(stderr, "error: '%s' has unsupported version %d\n", 
                filename, header.version);
        fclose(fp);
        return -1;
    }
    
    obj = &ls->objects[ls->num_objects];
    str_copy(obj->filename, filename, MAX_FILENAME);
    
    obj->code_size = READ24(header.code_size);
    obj->data_size = READ24(header.data_size);
    obj->bss_size = READ24(header.bss_size);
    obj->num_symbols = READ24(header.num_symbols);
    obj->num_relocs = READ24(header.num_relocs);
    obj->num_externs = READ24(header.num_externs);
    obj->strtab_size = READ24(header.strtab_size);
    
    /* Record file positions (relative to offset for libraries) */
    obj->code_pos = offset + sizeof(header);
    obj->data_pos = obj->code_pos + obj->code_size;
    obj->sym_pos = obj->data_pos + obj->data_size;
    obj->reloc_pos = obj->sym_pos + (obj->num_symbols * sizeof(ObjSymbol));
    obj->extern_pos = obj->reloc_pos + (obj->num_relocs * sizeof(ObjReloc));
    obj->strtab_pos = obj->extern_pos + (obj->num_externs * sizeof(ObjExtern));
    
    /* Read string table */
    strtab = NULL;
    if (obj->strtab_size > 0) {
        strtab = (char *)malloc(obj->strtab_size);
        if (!strtab) {
            fprintf(stderr, "error: out of memory\n");
            fclose(fp);
            return -1;
        }
        fseek(fp, obj->strtab_pos, SEEK_SET);
        if (fread(strtab, 1, obj->strtab_size, fp) != obj->strtab_size) {
            fprintf(stderr, "error: cannot read string table from '%s'\n", filename);
            free(strtab);
            fclose(fp);
            return -1;
        }
    }
    
    /* Read and register exported symbols */
    fseek(fp, obj->sym_pos, SEEK_SET);
    for (i = 0; i < (int)obj->num_symbols; i++) {
        if (fread(&sym, sizeof(sym), 1, fp) != 1) {
            fprintf(stderr, "error: cannot read symbol from '%s'\n", filename);
            if (strtab) free(strtab);
            fclose(fp);
            return -1;
        }
        
        name_off = READ24(sym.name_offset);
        value = READ24(sym.value);
        
        if (strtab && name_off < obj->strtab_size) {
            /* Value is section-relative; we'll make it absolute later */
            add_global(ls, &strtab[name_off], value, sym.section, ls->num_objects);
        }
    }
    
    if (strtab) free(strtab);
    fclose(fp);
    
    ls->num_objects++;
    
    if (ls->verbose) {
        printf("Loaded '%s': code=%u, data=%u, bss=%u\n",
               filename, (unsigned)obj->code_size, (unsigned)obj->data_size,
               (unsigned)obj->bss_size);
    }
    
    return 0;
}

/* Load an object file */
static int load_object(LinkerState *ls, const char *filename)
{
    return load_object_at(ls, filename, 0);
}

/* Add a library search directory */
static int add_libdir(LinkerState *ls, const char *dir)
{
    if (ls->num_libdirs >= MAX_LIBDIRS) {
        fprintf(stderr, "error: too many library directories\n");
        return -1;
    }
    str_copy(ls->libdirs[ls->num_libdirs], dir, MAX_FILENAME);
    ls->num_libdirs++;
    return 0;
}

/*
 * Search for a library by short name.
 * Given name "foo", searches each -L directory for "libfoo.a".
 * If found, calls add_library with the full path.
 * Also accepts a plain filename/path as a fallback.
 */
static int find_and_add_library(LinkerState *ls, const char *name)
{
    char path[MAX_FILENAME];
    FILE *fp;
    int i;
    
    /* Search -L directories for lib<name>.a */
    for (i = 0; i < ls->num_libdirs; i++) {
        int len = 0;
        const char *dir = ls->libdirs[i];
        
        /* Build path: dir/lib<name>.a */
        while (dir[len] && len < MAX_FILENAME - 1) {
            path[len] = dir[len];
            len++;
        }
        /* Ensure trailing separator */
        if (len > 0 && path[len - 1] != '/' && path[len - 1] != '\\') {
            if (len < MAX_FILENAME - 1) path[len++] = '/';
        }
        /* Append "lib" */
        if (len + 3 < MAX_FILENAME) {
            path[len++] = 'l';
            path[len++] = 'i';
            path[len++] = 'b';
        }
        /* Append name */
        {
            const char *p = name;
            while (*p && len < MAX_FILENAME - 1) {
                path[len++] = *p++;
            }
        }
        /* Append ".a" */
        if (len + 2 < MAX_FILENAME) {
            path[len++] = '.';
            path[len++] = 'a';
        }
        path[len] = '\0';
        
        /* Check if file exists */
        fp = fopen(path, "rb");
        if (fp) {
            fclose(fp);
            if (ls->verbose) {
                printf("Found library '%s' as '%s'\n", name, path);
            }
            return add_library(ls, path);
        }
    }
    
    /* Fallback: try the name as a direct path */
    fp = fopen(name, "rb");
    if (fp) {
        fclose(fp);
        return add_library(ls, name);
    }
    
    fprintf(stderr, "error: cannot find library '%s'\n", name);
    return -1;
}

/* Scan a library and record its objects (without loading them) */
static int add_library(LinkerState *ls, const char *filename)
{
    FILE *fp;
    ObjHeader header;
    LibraryInfo *lib;
    long file_size;
    long pos;
    uint24 code_size, data_size, bss_size;
    uint24 num_symbols, num_relocs, num_externs, strtab_size;
    uint24 obj_size;
    
    if (ls->num_libraries >= MAX_LIBRARIES) {
        fprintf(stderr, "error: too many libraries\n");
        return -1;
    }
    
    fp = fopen(filename, "rb");
    if (!fp) {
        fprintf(stderr, "error: cannot open library '%s'\n", filename);
        return -1;
    }
    
    lib = &ls->libraries[ls->num_libraries];
    str_copy(lib->filename, filename, MAX_FILENAME);
    lib->num_objects = 0;
    
    /* Get file size */
    fseek(fp, 0, SEEK_END);
    file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    /* Scan through library and record object positions */
    pos = 0;
    while (pos < file_size) {
        fseek(fp, pos, SEEK_SET);
        
        if (fread(&header, sizeof(header), 1, fp) != 1) {
            break;
        }
        
        /* Verify magic */
        if (header.magic[0] != 'E' || header.magic[1] != 'Z' ||
            header.magic[2] != '8' || header.magic[3] != 'O') {
            fprintf(stderr, "error: invalid object at offset %ld in '%s'\n", 
                    pos, filename);
            fclose(fp);
            return -1;
        }
        
        if (lib->num_objects >= MAX_LIB_OBJECTS) {
            fprintf(stderr, "error: too many objects in library '%s'\n", filename);
            fclose(fp);
            return -1;
        }
        
        /* Calculate object size */
        code_size = READ24(header.code_size);
        data_size = READ24(header.data_size);
        bss_size = READ24(header.bss_size);
        num_symbols = READ24(header.num_symbols);
        num_relocs = READ24(header.num_relocs);
        num_externs = READ24(header.num_externs);
        strtab_size = READ24(header.strtab_size);
        
        (void)bss_size;  /* BSS doesn't take file space */
        
        obj_size = sizeof(header) + code_size + data_size +
                   (num_symbols * sizeof(ObjSymbol)) +
                   (num_relocs * sizeof(ObjReloc)) +
                   (num_externs * sizeof(ObjExtern)) +
                   strtab_size;
        
        /* Record this object */
        lib->objects[lib->num_objects].offset = pos;
        lib->objects[lib->num_objects].obj_size = obj_size;
        lib->objects[lib->num_objects].loaded = 0;
        lib->num_objects++;
        
        pos += obj_size;
    }
    
    fclose(fp);
    
    ls->num_libraries++;
    
    if (ls->verbose) {
        printf("Scanned library '%s': %d object(s)\n", filename, lib->num_objects);
    }
    
    return 0;
}

/*
 * Collect undefined externals from an object file.
 * Uses an already-open FILE pointer to avoid repeated open/close.
 */
static int get_object_externals_fp(FILE *fp, ObjectInfo *obj,
                                   char externals[][MAX_SYM_NAME], int max_ext)
{
    ObjExtern ext;
    char *strtab;
    uint24 name_off;
    int i, count = 0;
    
    if (obj->strtab_size == 0 || obj->num_externs == 0) {
        return 0;
    }
    
    /* Read string table */
    strtab = (char *)malloc(obj->strtab_size);
    if (!strtab) {
        return 0;
    }
    fseek(fp, obj->strtab_pos, SEEK_SET);
    if (fread(strtab, 1, obj->strtab_size, fp) != obj->strtab_size) {
        free(strtab);
        return 0;
    }
    
    /* Read externals */
    fseek(fp, obj->extern_pos, SEEK_SET);
    for (i = 0; i < (int)obj->num_externs && count < max_ext; i++) {
        if (fread(&ext, sizeof(ext), 1, fp) != 1) break;
        
        name_off = READ24(ext.name_offset);
        if (name_off < obj->strtab_size) {
            str_copy(externals[count], &strtab[name_off], MAX_SYM_NAME);
            count++;
        }
    }
    
    free(strtab);
    return count;
}

/* Initialise a library symbol index */
static int lib_index_init(LibSymIndex *idx)
{
    int i;
    idx->entries = (LibSymEntry *)malloc(MAX_LIB_SYMS * sizeof(LibSymEntry));
    if (!idx->entries) return -1;
    idx->num_entries = 0;
    idx->max_entries = MAX_LIB_SYMS;
    for (i = 0; i < LIB_HASH_SIZE; i++) {
        idx->hash_buckets[i] = -1;
    }
    return 0;
}

/* Free a library symbol index */
static void lib_index_free(LibSymIndex *idx)
{
    if (idx->entries) {
        free(idx->entries);
        idx->entries = NULL;
    }
    idx->num_entries = 0;
}

/* Add a symbol to the library index */
static int lib_index_add(LibSymIndex *idx, const char *name,
                         int lib_idx, int obj_idx)
{
    unsigned bucket;
    LibSymEntry *e;
    
    if (idx->num_entries >= idx->max_entries) {
        return -1; /* Full */
    }
    
    e = &idx->entries[idx->num_entries];
    str_copy(e->name, name, MAX_LIBSYM_NAME);
    e->lib_idx = (uint8)lib_idx;
    e->obj_idx = (uint8)obj_idx;
    
    bucket = sym_hash(name);
    e->hash_next = idx->hash_buckets[bucket];
    idx->hash_buckets[bucket] = idx->num_entries;
    idx->num_entries++;
    
    return 0;
}

/*
 * Look up a symbol in the library index.
 * Returns a pointer to the matching entry whose library object has
 * not yet been loaded, or NULL if not found.
 */
static LibSymEntry *lib_index_find(LibSymIndex *idx, const char *name,
                                   LinkerState *ls)
{
    int i = idx->hash_buckets[sym_hash(name)];
    while (i >= 0) {
        LibSymEntry *e = &idx->entries[i];
        if (str_casecmp(e->name, name) == 0) {
            /* Check if this object is not yet loaded */
            if (!ls->libraries[e->lib_idx].objects[e->obj_idx].loaded) {
                return e;
            }
        }
        i = e->hash_next;
    }
    return NULL;
}

/*
 * Build the library symbol index by scanning all library objects
 * and recording their exported symbols.  Each library file is opened
 * once and all its objects' symbol/string tables are read in a
 * single pass.
 */
static int build_lib_index(LinkerState *ls, LibSymIndex *idx)
{
    int lib_idx, obj_idx;
    
    for (lib_idx = 0; lib_idx < ls->num_libraries; lib_idx++) {
        LibraryInfo *lib = &ls->libraries[lib_idx];
        FILE *fp;
        
        fp = fopen(lib->filename, "rb");
        if (!fp) continue;
        
        for (obj_idx = 0; obj_idx < lib->num_objects; obj_idx++) {
            ObjHeader header;
            ObjSymbol *sym_buf;
            char *strtab;
            uint24 code_size, data_size;
            uint24 num_symbols, num_relocs, num_externs, strtab_size;
            long sym_pos, strtab_pos;
            uint24 name_off;
            int s;
            
            if (lib->objects[obj_idx].loaded) continue;
            
            /* Read header */
            fseek(fp, lib->objects[obj_idx].offset, SEEK_SET);
            if (fread(&header, sizeof(header), 1, fp) != 1) continue;
            
            code_size = READ24(header.code_size);
            data_size = READ24(header.data_size);
            num_symbols = READ24(header.num_symbols);
            num_relocs = READ24(header.num_relocs);
            num_externs = READ24(header.num_externs);
            strtab_size = READ24(header.strtab_size);
            
            if (num_symbols == 0 || strtab_size == 0) continue;
            
            sym_pos = lib->objects[obj_idx].offset + sizeof(header) +
                      code_size + data_size;
            strtab_pos = sym_pos +
                         (num_symbols * sizeof(ObjSymbol)) +
                         (num_relocs * sizeof(ObjReloc)) +
                         (num_externs * sizeof(ObjExtern));
            
            /* Read string table */
            strtab = (char *)malloc(strtab_size);
            if (!strtab) continue;
            
            fseek(fp, strtab_pos, SEEK_SET);
            if (fread(strtab, 1, strtab_size, fp) != strtab_size) {
                free(strtab);
                continue;
            }
            
            /* Read all symbol entries */
            sym_buf = (ObjSymbol *)malloc(num_symbols * sizeof(ObjSymbol));
            if (!sym_buf) {
                free(strtab);
                continue;
            }
            
            fseek(fp, sym_pos, SEEK_SET);
            if (fread(sym_buf, sizeof(ObjSymbol), num_symbols, fp)
                    != num_symbols) {
                free(sym_buf);
                free(strtab);
                continue;
            }
            
            /* Add each exported symbol to the index */
            for (s = 0; s < (int)num_symbols; s++) {
                name_off = READ24(sym_buf[s].name_offset);
                if (name_off < strtab_size) {
                    lib_index_add(idx, &strtab[name_off],
                                  lib_idx, obj_idx);
                }
            }
            
            free(sym_buf);
            free(strtab);
        }
        
        fclose(fp);
    }
    
    return 0;
}

/*
 * Process libraries - selectively load objects that satisfy undefined
 * references.
 *
 * Builds a hash index of all exported library symbols once up front,
 * then resolves undefined references with O(1) hash lookups instead
 * of scanning every library object from disk.
 */
static int process_libraries(LinkerState *ls)
{
    LibSymIndex idx;
    char (*undefined)[MAX_SYM_NAME];
    char (*obj_ext)[MAX_SYM_NAME];
    int num_undefined;
    int i, j, k;
    int loaded_any;
    int total_loaded = 0;
    
    if (ls->num_libraries == 0) {
        return 0;  /* No libraries to process */
    }
    
    /* Build library symbol index (reads all libraries once) */
    if (lib_index_init(&idx) < 0) {
        fprintf(stderr, "error: out of memory for library index\n");
        return -1;
    }
    build_lib_index(ls, &idx);
    
    if (ls->verbose) {
        printf("Library index: %d symbols from %d library(s)\n",
               idx.num_entries, ls->num_libraries);
    }
    
    undefined = (char (*)[MAX_SYM_NAME])malloc(MAX_EXTERNS * MAX_SYM_NAME);
    if (!undefined) {
        fprintf(stderr, "error: out of memory\n");
        lib_index_free(&idx);
        return -1;
    }
    
    /* Allocate scratch buffer for per-object externals once */
    obj_ext = (char (*)[MAX_SYM_NAME])malloc(MAX_OBJ_EXTERNS * MAX_SYM_NAME);
    if (!obj_ext) {
        fprintf(stderr, "error: out of memory\n");
        free(undefined);
        lib_index_free(&idx);
        return -1;
    }
    
    /* Iterate until no more symbols are resolved */
    do {
        loaded_any = 0;
        num_undefined = 0;
        
        /* Collect all undefined externals from loaded objects.
         * Open each object file once, read its externals, close it. */
        for (i = 0; i < ls->num_objects && num_undefined < MAX_EXTERNS; i++) {
            int ext_count;
            FILE *ofp = fopen(ls->objects[i].filename, "rb");
            if (!ofp) continue;
            
            ext_count = get_object_externals_fp(ofp, &ls->objects[i],
                                                obj_ext, MAX_OBJ_EXTERNS);
            fclose(ofp);
            
            for (j = 0; j < ext_count && num_undefined < MAX_EXTERNS; j++) {
                /* Check if already defined */
                if (find_global(ls, obj_ext[j]) != NULL) {
                    continue;  /* Already satisfied */
                }
                
                /* Check if already in undefined list */
                for (k = 0; k < num_undefined; k++) {
                    if (str_casecmp(undefined[k], obj_ext[j]) == 0) break;
                }
                if (k == num_undefined) {
                    /* Add to undefined list */
                    str_copy(undefined[num_undefined], obj_ext[j], MAX_SYM_NAME);
                    num_undefined++;
                }
            }
        }
        
        if (num_undefined == 0) {
            break;  /* All symbols resolved */
        }
        
        /* Resolve each undefined symbol via the library index.
         * Each lookup is O(1) via the hash table. */
        for (i = 0; i < num_undefined; i++) {
            LibSymEntry *entry;
            LibraryInfo *lib;
            
            /* Skip if another library object already defined it
             * earlier in this iteration */
            if (find_global(ls, undefined[i]) != NULL) {
                continue;
            }
            
            entry = lib_index_find(&idx, undefined[i], ls);
            if (!entry) continue;
            
            lib = &ls->libraries[entry->lib_idx];
            
            if (ls->verbose) {
                printf("Loading from library '%s' for symbol '%s'\n",
                       lib->filename, undefined[i]);
            }
            
            if (load_object_at(ls, lib->filename,
                               lib->objects[entry->obj_idx].offset) == 0) {
                lib->objects[entry->obj_idx].loaded = 1;
                loaded_any = 1;
                total_loaded++;
            }
        }
        
    } while (loaded_any);
    
    free(obj_ext);
    free(undefined);
    lib_index_free(&idx);
    
    if (ls->verbose && total_loaded > 0) {
        printf("Loaded %d object(s) from libraries\n", total_loaded);
    }
    
    return 0;
}

/* Assign base addresses to all sections */
static int resolve_symbols(LinkerState *ls)
{
    int i;
    uint24 code_addr, data_addr, bss_addr;
    GlobalSymbol *sym;
    
    /* Calculate section layout */
    code_addr = ls->base_addr;
    
    for (i = 0; i < ls->num_objects; i++) {
        ls->objects[i].code_base = code_addr;
        code_addr += ls->objects[i].code_size;
    }
    ls->total_code = code_addr - ls->base_addr;
    
    data_addr = code_addr;
    for (i = 0; i < ls->num_objects; i++) {
        ls->objects[i].data_base = data_addr;
        data_addr += ls->objects[i].data_size;
    }
    ls->total_data = data_addr - code_addr;
    
    bss_addr = data_addr;
    for (i = 0; i < ls->num_objects; i++) {
        ls->objects[i].bss_base = bss_addr;
        bss_addr += ls->objects[i].bss_size;
    }
    ls->total_bss = bss_addr - data_addr;
    
    /* Update all global symbols to absolute addresses */
    for (i = 0; i < ls->num_symbols; i++) {
        sym = &ls->symbols[i];
        switch (sym->section) {
            case SECT_CODE:
                sym->value += ls->objects[sym->obj_index].code_base;
                break;
            case SECT_DATA:
                sym->value += ls->objects[sym->obj_index].data_base;
                break;
            case SECT_BSS:
                sym->value += ls->objects[sym->obj_index].bss_base;
                break;
        }
    }
    
    /* Add linker-defined symbols for C runtime initialization */
    add_global(ls, "__low_code", ls->base_addr, 0, LINKER_DEFINED);
    add_global(ls, "__len_code", ls->total_code, 0, LINKER_DEFINED);
    add_global(ls, "__low_data", ls->base_addr + ls->total_code, 0, LINKER_DEFINED);
    add_global(ls, "__len_data", ls->total_data, 0, LINKER_DEFINED);
    add_global(ls, "__low_bss", ls->base_addr + ls->total_code + ls->total_data, 0, LINKER_DEFINED);
    add_global(ls, "__len_bss", ls->total_bss, 0, LINKER_DEFINED);
    
    if (ls->verbose) {
        printf("Layout: CODE=%06X-%06X, DATA=%06X-%06X, BSS=%06X-%06X\n",
               (unsigned)ls->base_addr,
               (unsigned)(ls->base_addr + ls->total_code - 1),
               (unsigned)(ls->base_addr + ls->total_code),
               (unsigned)(ls->base_addr + ls->total_code + ls->total_data - 1),
               (unsigned)(ls->base_addr + ls->total_code + ls->total_data),
               (unsigned)(ls->base_addr + ls->total_code + ls->total_data + ls->total_bss - 1));
    }
    
    return 0;
}

/*
 * Link and produce output.
 *
 * Optimised: each object file is opened once (not twice), code/data
 * sections are read with fread (not fgetc), and the string table and
 * extern table are read into memory once per object so that external
 * name lookups during relocation don't need any further file I/O.
 */
static int link_output(LinkerState *ls)
{
    FILE *out;
    FILE *fp;
    ObjectInfo *obj;
    ObjReloc reloc;
    uint24 offset, target_addr;
    uint8 section, target_sect;
    unsigned ext_index;
    char ext_name[MAX_SYM_NAME];
    GlobalSymbol *sym;
    unsigned char *code_buf;
    unsigned char *data_buf;
    uint24 i, j;
    long patch_pos;
    uint24 existing;
    
    /* Cached per-object tables */
    char *strtab;
    ObjExtern *ext_tab;
    uint24 name_off;
    
    out = fopen(ls->output_file, "wb");
    if (!out) {
        fprintf(stderr, "error: cannot create '%s'\n", ls->output_file);
        return -1;
    }
    
    /* Allocate buffers for the entire output */
    code_buf = (unsigned char *)malloc(ls->total_code ? ls->total_code : 1);
    data_buf = (unsigned char *)malloc(ls->total_data ? ls->total_data : 1);
    if (!code_buf || !data_buf) {
        fprintf(stderr, "error: out of memory\n");
        if (code_buf) free(code_buf);
        if (data_buf) free(data_buf);
        fclose(out);
        return -1;
    }
    memset(code_buf, 0, ls->total_code ? ls->total_code : 1);
    memset(data_buf, 0, ls->total_data ? ls->total_data : 1);
    
    /*
     * Single pass per object file: open once, read code + data with
     * fread, cache string table and extern table, apply relocations,
     * then close.
     */
    for (i = 0; i < (uint24)ls->num_objects; i++) {
        obj = &ls->objects[i];
        strtab = NULL;
        ext_tab = NULL;
        
        fp = fopen(obj->filename, "rb");
        if (!fp) {
            fprintf(stderr, "error: cannot reopen '%s'\n", obj->filename);
            free(code_buf);
            free(data_buf);
            fclose(out);
            return -1;
        }
        
        /* --- Read code section with fread (was fgetc loop) --- */
        if (obj->code_size > 0) {
            fseek(fp, obj->code_pos, SEEK_SET);
            fread(&code_buf[obj->code_base - ls->base_addr],
                  1, obj->code_size, fp);
        }
        
        /* --- Read data section with fread (was fgetc loop) --- */
        if (obj->data_size > 0) {
            fseek(fp, obj->data_pos, SEEK_SET);
            fread(&data_buf[obj->data_base - ls->base_addr - ls->total_code],
                  1, obj->data_size, fp);
        }
        
        /* --- Cache string table for relocation lookups --- */
        if (obj->strtab_size > 0) {
            strtab = (char *)malloc(obj->strtab_size);
            if (strtab) {
                fseek(fp, obj->strtab_pos, SEEK_SET);
                if (fread(strtab, 1, obj->strtab_size, fp)
                        != obj->strtab_size) {
                    free(strtab);
                    strtab = NULL;
                }
            }
        }
        
        /* --- Cache extern table for relocation lookups --- */
        if (obj->num_externs > 0) {
            ext_tab = (ObjExtern *)malloc(obj->num_externs * sizeof(ObjExtern));
            if (ext_tab) {
                fseek(fp, obj->extern_pos, SEEK_SET);
                if (fread(ext_tab, sizeof(ObjExtern), obj->num_externs, fp)
                        != obj->num_externs) {
                    free(ext_tab);
                    ext_tab = NULL;
                }
            }
        }
        
        /* --- Apply relocations using cached tables (no more file I/O) --- */
        fseek(fp, obj->reloc_pos, SEEK_SET);
        
        for (j = 0; j < obj->num_relocs; j++) {
            if (fread(&reloc, sizeof(reloc), 1, fp) != 1) break;
            
            offset = READ24(reloc.offset);
            section = reloc.section;
            target_sect = reloc.target_sect;
            ext_index = READ16(reloc.ext_index);
            
            /* Determine target address */
            if (target_sect == 0) {
                /* External reference - look up from cached tables */
                if (!ext_tab || !strtab ||
                    ext_index >= (unsigned)obj->num_externs) {
                    fprintf(stderr,
                            "error: cannot resolve external %u in '%s'\n",
                            ext_index, obj->filename);
                    ls->errors++;
                    continue;
                }
                
                name_off = READ24(ext_tab[ext_index].name_offset);
                if (name_off >= obj->strtab_size) {
                    fprintf(stderr,
                            "error: bad extern name offset %u in '%s'\n",
                            (unsigned)name_off, obj->filename);
                    ls->errors++;
                    continue;
                }
                str_copy(ext_name, &strtab[name_off], MAX_SYM_NAME);
                
                sym = find_global(ls, ext_name);
                if (!sym) {
                    fprintf(stderr,
                            "error: undefined symbol '%s' referenced in '%s'\n",
                            ext_name, obj->filename);
                    ls->errors++;
                    continue;
                }
                
                target_addr = sym->value;
            } else {
                /* Local section reference - get section base */
                switch (target_sect) {
                    case SECT_CODE:
                        target_addr = obj->code_base;
                        break;
                    case SECT_DATA:
                        target_addr = obj->data_base;
                        break;
                    case SECT_BSS:
                        target_addr = obj->bss_base;
                        break;
                    default:
                        fprintf(stderr, "error: invalid target section %d\n",
                                target_sect);
                        ls->errors++;
                        continue;
                }
            }
            
            /* Find patch location and apply relocation */
            if (section == SECT_CODE) {
                patch_pos = obj->code_base - ls->base_addr + offset;
                if (patch_pos + 2 < (long)ls->total_code) {
                    /* Read existing value (section-relative offset) */
                    existing = code_buf[patch_pos] |
                               ((uint24)code_buf[patch_pos + 1] << 8) |
                               ((uint24)code_buf[patch_pos + 2] << 16);
                    
                    /* Add base address */
                    target_addr += existing;
                    
                    /* Write absolute address */
                    code_buf[patch_pos] = target_addr & 0xFF;
                    code_buf[patch_pos + 1] = (target_addr >> 8) & 0xFF;
                    code_buf[patch_pos + 2] = (target_addr >> 16) & 0xFF;
                }
            } else if (section == SECT_DATA) {
                patch_pos = obj->data_base - ls->base_addr - ls->total_code + offset;
                if (patch_pos + 2 < (long)ls->total_data) {
                    existing = data_buf[patch_pos] |
                               ((uint24)data_buf[patch_pos + 1] << 8) |
                               ((uint24)data_buf[patch_pos + 2] << 16);
                    
                    target_addr += existing;
                    
                    data_buf[patch_pos] = target_addr & 0xFF;
                    data_buf[patch_pos + 1] = (target_addr >> 8) & 0xFF;
                    data_buf[patch_pos + 2] = (target_addr >> 16) & 0xFF;
                }
            }
        }
        
        /* Free cached tables and close the single file handle */
        if (ext_tab) free(ext_tab);
        if (strtab) free(strtab);
        fclose(fp);
    }
    
    if (ls->errors > 0) {
        free(code_buf);
        free(data_buf);
        fclose(out);
        return -1;
    }
    
    /* Write output */
    if (ls->total_code > 0) {
        fwrite(code_buf, 1, ls->total_code, out);
    }
    if (ls->total_data > 0) {
        fwrite(data_buf, 1, ls->total_data, out);
    }
    
    free(code_buf);
    free(data_buf);
    fclose(out);
    
    if (ls->verbose) {
        printf("Output: %s (%u bytes)\n", ls->output_file,
               (unsigned)(ls->total_code + ls->total_data));
    }
    
    return 0;
}

/* Write map file */
static int write_map(LinkerState *ls)
{
    FILE *fp;
    int i;
    
    fp = fopen(ls->map_file, "w");
    if (!fp) {
        fprintf(stderr, "error: cannot create map file '%s'\n", ls->map_file);
        return -1;
    }
    
    fprintf(fp, "eZ80 Linker Map File\n");
    fprintf(fp, "====================\n\n");
    
    fprintf(fp, "Memory Layout:\n");
    fprintf(fp, "  CODE: %06X - %06X (%u bytes)\n",
            (unsigned)ls->base_addr,
            (unsigned)(ls->base_addr + ls->total_code - 1),
            (unsigned)ls->total_code);
    fprintf(fp, "  DATA: %06X - %06X (%u bytes)\n",
            (unsigned)(ls->base_addr + ls->total_code),
            (unsigned)(ls->base_addr + ls->total_code + ls->total_data - 1),
            (unsigned)ls->total_data);
    fprintf(fp, "  BSS:  %06X - %06X (%u bytes)\n\n",
            (unsigned)(ls->base_addr + ls->total_code + ls->total_data),
            (unsigned)(ls->base_addr + ls->total_code + ls->total_data + ls->total_bss - 1),
            (unsigned)ls->total_bss);
    
    fprintf(fp, "Object Files:\n");
    for (i = 0; i < ls->num_objects; i++) {
        fprintf(fp, "  %s\n", ls->objects[i].filename);
        fprintf(fp, "    CODE: %06X (%u bytes)\n",
                (unsigned)ls->objects[i].code_base,
                (unsigned)ls->objects[i].code_size);
        fprintf(fp, "    DATA: %06X (%u bytes)\n",
                (unsigned)ls->objects[i].data_base,
                (unsigned)ls->objects[i].data_size);
        fprintf(fp, "    BSS:  %06X (%u bytes)\n",
                (unsigned)ls->objects[i].bss_base,
                (unsigned)ls->objects[i].bss_size);
    }
    fprintf(fp, "\n");
    
    fprintf(fp, "Symbols:\n");
    fprintf(fp, "  %-24s %-8s %s\n", "Name", "Address", "Object");
    fprintf(fp, "  %-24s %-8s %s\n", "----", "-------", "------");
    for (i = 0; i < ls->num_symbols; i++) {
        fprintf(fp, "  %-24s %06X   %s\n",
                ls->symbols[i].name,
                (unsigned)ls->symbols[i].value,
                ls->symbols[i].obj_index == LINKER_DEFINED ? "(linker)" :
                ls->objects[ls->symbols[i].obj_index].filename);
    }
    
    fclose(fp);
    
    if (ls->verbose) {
        printf("Map file: %s\n", ls->map_file);
    }
    
    return 0;
}

/* Print usage */
static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [options] <object-files...>\n", prog);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -o <file>   Output filename (default: a.out)\n");
    fprintf(stderr, "  -b <addr>   Base address in hex (default: 000000)\n");
    fprintf(stderr, "  -m <file>   Generate map file\n");
    fprintf(stderr, "  -L <dir>    Add library search directory\n");
    fprintf(stderr, "  -l<n> | -l <n>  Link library lib<n>.a\n");
    fprintf(stderr, "  -v          Verbose output\n");
    fprintf(stderr, "  -h          Show this help\n");
}

int main(int argc, char *argv[])
{
    LinkerState ls;
    int i;
    char *endptr;
    
    memset(&ls, 0, sizeof(ls));
    ls.output_file = "a.out";
    ls.base_addr = 0;
    
    /* Initialise hash table buckets to empty (-1) */
    for (i = 0; i < HASH_SIZE; i++) {
        ls.hash_buckets[i] = -1;
    }
    
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }
    
    /* Parse arguments */
    for (i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            switch (argv[i][1]) {
                case 'o':
                    if (i + 1 >= argc) {
                        fprintf(stderr, "error: -o requires filename\n");
                        return 1;
                    }
                    ls.output_file = argv[++i];
                    break;
                    
                case 'b':
                    if (i + 1 >= argc) {
                        fprintf(stderr, "error: -b requires address\n");
                        return 1;
                    }
                    ls.base_addr = (uint24)strtol(argv[++i], &endptr, 16);
                    break;
                    
                case 'm':
                    if (i + 1 >= argc) {
                        fprintf(stderr, "error: -m requires filename\n");
                        return 1;
                    }
                    ls.map_file = argv[++i];
                    break;
                    
                case 'L':
                    if (i + 1 >= argc) {
                        fprintf(stderr, "error: -L requires directory\n");
                        return 1;
                    }
                    if (add_libdir(&ls, argv[++i]) < 0) {
                        return 1;
                    }
                    break;
                    
                case 'l':
                    {
                        const char *libname;
                        if (argv[i][2] != '\0') {
                            /* -lc form: name follows immediately */
                            libname = &argv[i][2];
                        } else {
                            /* -l c form: name is next argument */
                            if (i + 1 >= argc) {
                                fprintf(stderr, "error: -l requires library name\n");
                                return 1;
                            }
                            libname = argv[++i];
                        }
                        if (find_and_add_library(&ls, libname) < 0) {
                            return 1;
                        }
                    }
                    break;
                    
                case 'v':
                    ls.verbose = 1;
                    break;
                    
                case 'h':
                    usage(argv[0]);
                    return 0;
                    
                default:
                    fprintf(stderr, "error: unknown option '-%c'\n", argv[i][1]);
                    return 1;
            }
        } else {
            /* Object file */
            if (load_object(&ls, argv[i]) < 0) {
                return 1;
            }
        }
    }
    
    if (ls.num_objects == 0) {
        fprintf(stderr, "error: no input files\n");
        return 1;
    }
    
    /* Process libraries - selectively load needed objects */
    if (process_libraries(&ls) < 0) {
        return 1;
    }
    
    /* Resolve symbols and assign addresses */
    resolve_symbols(&ls);
    
    if (ls.errors > 0) {
        fprintf(stderr, "Link failed with %d error(s)\n", ls.errors);
        return 1;
    }
    
    /* Generate output */
    if (link_output(&ls) < 0) {
        return 1;
    }
    
    /* Generate map file if requested */
    if (ls.map_file) {
        write_map(&ls);
    }
    
    if (ls.verbose) {
        printf("Link successful\n");
    }
    
    return ls.errors > 0 ? 1 : 0;
}
