# eZ80 Assembler and Linker

A portable assembler and linker for the Zilog eZ80 processor, created for use with the Deep C compiler on the AgonLight/Console 8.

## Features

- **Full eZ80 ADL mode support** - 24-bit addressing and registers
- **Complete instruction set** - All documented eZ80 instructions including:
- **Flexible syntax** - Supports common assembler conventions
- **Relocatable object files** - Link multiple modules together
- **Selective library loading** - Only includes needed objects from libraries
- **Map file generation** - For debugging and analysis

## Building

The project requires only a C89-compatible compiler. To build all three tools:

```bash
cc -o as main.c ez80asm.c ez80instr.c ez80dir.c
cc -o ld ld.c
cc -o objdump objdump.c
```

This builds three executables:
- `as` - The assembler
- `ld` - The linker
- `objdump` - Object file inspection tool

## Usage

### Assembler

```bash
as [options] <source.asm>
```

**Options:**
- `-o <file>` - Output filename (default: source.o)
- `-v` - Verbose output
- `-h` - Show help

**Example:**
```bash
as -v -o program.o program.asm
```

### Linker

```bash
ld [options] <object-files...>
```

**Options:**
- `-o <file>` - Output filename (default: a.out)
- `-b <addr>` - Base address in hex (default: 000000)
- `-m <file>` - Generate map file
- `-l<library>` - Link with library file lib<library>.a
- `-L <directory>` - Add directory to search path for libraries
- `-v` - Verbose output
- `-h` - Show help

**Example:**
```bash
ld -v -o program.bin -b 40000 -L /lib/ -m program.map main.o utils.o -lc -lm
```

### Object Dump

```bash
objdump <object-file>
```

Displays the contents of an object file including headers, code, symbols, relocations, and external references.

## Assembly Language Syntax

### Directives

| Directive | Description |
|-----------|-------------|
| `assume adl=1` | Set ADL mode (24-bit) - required |
| `section code` | Switch to code section |
| `section data` | Switch to data section |
| `section bss` | Switch to BSS section |
| `org <addr>` | Set origin address |
| `equ <value>` | Define constant |
| `xdef <symbol>` | Export symbol |
| `xref <symbol>` | Import external symbol |
| `db <bytes>` | Define bytes |
| `dw <words>` | Define 16-bit words |
| `dl <longs>` | Define 24-bit values |
| `ds <count>` | Reserve space |
| `incbin "<file>"` | Include binary file |
| `include "<file>"` | Include source file |
| `end` | End of source |

### Supported Instructions

All standard eZ80 instructions:

- **Load/Store**: LD with all addressing modes, including 24-bit operations
- **Arithmetic**: ADD, ADC, SUB, SBC, INC, DEC, NEG, MLT
- **Logic**: AND, OR, XOR, CPL, BIT, SET, RES
- **Rotation**: RLC, RRC, RL, RR, SLA, SRA, SRL
- **Control**: JP, JR, CALL, RET, RETI, RETN, RST, DJNZ
- **Stack**: PUSH, POP, EX
- **I/O**: IN, OUT, IN0, OUT0, block I/O
- **eZ80 specific**: LEA, PEA, TST, TSTIO, MLT, SLP

### Addressing Modes

```asm
    ld a, 42            ; Immediate
    ld a, b             ; Register
    ld a, (hl)          ; Register indirect
    ld a, (ix+5)        ; Indexed
    ld a, ($1234)       ; Direct
    ld hl, (ix+10)      ; 24-bit indexed load
    lea hl, ix+20       ; Load effective address
```

### Number Formats

```asm
    ld a, 42            ; Decimal
    ld a, $2A           ; Hexadecimal ($ prefix)
    ld a, 0x2A          ; Hexadecimal (0x prefix)
    ld a, 2Ah           ; Hexadecimal (h suffix)
    ld a, %00101010     ; Binary
    ld a, 'X'           ; Character
```

### Labels and Symbols

```asm
    xdef _main          ; Export symbol
    xref _printf        ; Import external

_main:
    ld hl, message
    call _printf
    ret

message:
    db "Hello", 0
```

Local labels begin with `@` and are only visible between two global labels. This allows reuse of common label names like `@loop` or `@done` within different routines:

```asm
func1:
    ld b, 10
@loop:
    djnz @loop          ; Jumps to func1's @loop
    ret

func2:
    ld b, 5
@loop:
    djnz @loop          ; Jumps to func2's @loop
    ret
```

## Creating Libraries

Libraries are simply concatenated object files:

```bash
as module1.asm
as module2.asm
as module3.asm
cat module1.o module2.o module3.o > mylib.a
```

The linker will only include objects from the library that are actually needed to resolve undefined symbols.

## Object File Format

The assembler produces relocatable object files with:
- Code, data, and BSS sections
- Exported symbol table
- Relocation entries
- External reference table

Use `objdump` to inspect object files:

```bash
./objdump program.o
```

## Linker-Defined Symbols

The linker automatically defines these symbols for runtime initialization:

| Symbol | Description |
|--------|-------------|
| `__low_code` | Start address of code section |
| `__len_code` | Length of code section |
| `__low_data` | Start address of data section |
| `__len_data` | Length of data section |
| `__low_bss` | Start address of BSS section |
| `__len_bss` | Length of BSS section |

## Suffix Support

The assembler has limited support for eZ80 instruction suffixes:

- `.LIL` suffix is supported on `RST` instructions
- `.S` suffix is supported on `ADD`, `ADC`, and `SBC` instructions

## Limitations

- Only ADL mode is supported (no Z80 compatibility mode)
- Macros are not currently supported
- Conditional assembly is not implemented

## License

This is free and unencumbered software released into the public domain.

Anyone is free to copy, modify, publish, use, compile, sell, or distribute this
software, either in source code form or as a compiled binary, for any purpose,
commercial or non-commercial, and by any means.

In jurisdictions that recognize copyright laws, the author or authors of this
software dedicate any and all copyright interest in the software to the public
domain. We make this dedication for the benefit of the public at large and to
the detriment of our heirs and successors. We intend this dedication to be an
overt act of relinquishment in perpetuity of all present and future rights to
this software under copyright law.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

For more information, please refer to <http://unlicense.org/>
