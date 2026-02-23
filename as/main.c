/*
 * eZ80 ADL Mode Assembler - Main Entry Point
 * C89 compatible, 24-bit integers
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ez80asm.h"

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [options] input.asm\n", prog);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -o file    Output object file (default: input.o)\n");
    fprintf(stderr, "  -v         Verbose output\n");
    fprintf(stderr, "  -h         Show this help\n");
}

static void change_extension(char *dest, const char *src, 
                             const char *newext, int maxlen)
{
    const char *dot;
    int baselen;
    int i;
    
    dot = NULL;
    for (i = 0; src[i]; i++) {
        if (src[i] == '.') dot = &src[i];
        if (src[i] == '/' || src[i] == '\\') dot = NULL;
    }
    
    if (dot) {
        baselen = dot - src;
    } else {
        baselen = i;
    }
    
    if (baselen > maxlen - 4) baselen = maxlen - 4;
    
    for (i = 0; i < baselen; i++) {
        dest[i] = src[i];
    }
    dest[i] = '\0';
    
    /* Append new extension */
    i = baselen;
    while (*newext && i < maxlen - 1) {
        dest[i++] = *newext++;
    }
    dest[i] = '\0';
}

int main(int argc, char **argv)
{
    AsmState as;
    const char *input_file;
    char output_file[256];
    int verbose;
    int i;
    int result;
    
    input_file = NULL;
    output_file[0] = '\0';
    verbose = 0;
    
    /* Parse arguments */
    for (i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            if (strcmp(argv[i], "-o") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "error: -o requires argument\n");
                    return 1;
                }
                i++;
                strncpy(output_file, argv[i], sizeof(output_file) - 1);
                output_file[sizeof(output_file) - 1] = '\0';
            }
            else if (strcmp(argv[i], "-v") == 0) {
                verbose = 1;
            }
            else if (strcmp(argv[i], "-h") == 0) {
                usage(argv[0]);
                return 0;
            }
            else {
                fprintf(stderr, "error: unknown option '%s'\n", argv[i]);
                usage(argv[0]);
                return 1;
            }
        }
        else {
            if (input_file) {
                fprintf(stderr, "error: multiple input files\n");
                return 1;
            }
            input_file = argv[i];
        }
    }
    
    if (!input_file) {
        fprintf(stderr, "error: no input file\n");
        usage(argv[0]);
        return 1;
    }
    
    /* Generate default output filename if not specified */
    if (output_file[0] == '\0') {
        change_extension(output_file, input_file, ".o", sizeof(output_file));
    }
    
    /* Initialize assembler */
    if (asm_init(&as) < 0) {
        fprintf(stderr, "error: failed to initialize assembler\n");
        return 1;
    }
    
    as.verbose = verbose;
    
    /* Assemble file */
    result = asm_file(&as, input_file);
    
    if (result == 0) {
        result = asm_output(&as, output_file);
    }
    
    if (result != 0) {
        fprintf(stderr, "Assembly failed with %d error(s)\n", as.errors);
    }
    else if (verbose) {
        printf("Assembly successful\n");
    }
    
    asm_free(&as);
    
    return result != 0 ? 1 : 0;
}
