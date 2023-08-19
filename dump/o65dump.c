/*
 * Copyright (C) 2023 Southern Storm Software, Pty Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "o65file.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int dump_file(const char *filename);

int main(int argc, char *argv[])
{
    int arg;
    int exit_val = 0;

    /* Need at least one command-line argument */
    if (argc <= 1) {
        fprintf(stderr, "Usage: %s file1 ...\n", argv[0]);
        return 1;
    }

    /* Process each of the files in turn */
    for (arg = 1; arg < argc; ++arg) {
        if (arg > 1)
            printf("\n");
        if (argc > 2)
            printf("%s:\n\n", argv[1]);
        if (!dump_file(argv[1]))
            exit_val = 1;
    }
    return exit_val;
}

static void file_error(FILE *file, const char *filename)
{
    if (feof(file))
        fprintf(stderr, "%s: unexpected EOF\n", filename);
    else
        perror(filename);
    fclose(file);
}

static void dump_string(const uint8_t *data, int len)
{
    while (len > 0) {
        int ch = *data++;
        if (ch >= ' ' && ch <= 0x7E)
            putc(ch, stdout);
        else if (ch != 0)
            printf("\\x%02x", ch);
        --len;
    }
}

static int dump_nul_string(FILE *file)
{
    int ch;
    for (;;) {
        ch = getc(file);
        if (ch < 0)
            return -1;
        else if (ch == 0)
            break;
        else if (ch >= ' ' && ch <= 0x7E)
            putc(ch, stdout);
        else if (ch != 0)
            printf("\\x%02x", ch);
    }
    return 1;
}

static void dump_hex(const uint8_t *data, int len)
{
    while (len > 0) {
        int ch = *data++;
         printf(" %02x", ch);
        --len;
    }
}

static void dump_hex_line
    (const o65_header_t *header, o65_size_t addr, const uint8_t *data, int len)
{
    if ((header->mode & O65_MODE_32BIT) != 0)
        printf("    %08lx:", (unsigned long)addr);
    else
        printf("    %04lx:", (unsigned long)addr);
    dump_hex(data, len);
    printf("\n");
}

static void dump_option(const o65_option_t *option)
{
    printf("    ");
    switch (option->type) {
    case O65_OPT_FILENAME:
        printf("Filename: ");
        dump_string(option->data, option->len - 2);
        break;

    case O65_OPT_OS:
        printf("Operating System Information:");
        dump_hex(option->data, option->len - 2);
        break;

    case O65_OPT_PROGRAM:
        printf("Assembler/Linker: ");
        dump_string(option->data, option->len - 2);
        break;

    case O65_OPT_AUTHOR:
        printf("Author: ");
        dump_string(option->data, option->len - 2);
        break;

    case O65_OPT_CREATED:
        printf("Created: ");
        dump_string(option->data, option->len - 2);
        break;

    default:
        printf("Option %d:", option->type);
        dump_hex(option->data, option->len - 2);
        break;
    }
    printf("\n");
}

static int dump_segment
    (FILE *file, const char *name, const o65_header_t *header,
     o65_size_t base, o65_size_t len)
{
    uint8_t buf[16];

    /* Print the size of the segment */
    printf("\n%s: %lu bytes\n", name, (unsigned long)len);

    /* Dump the contents of the segment */
    while (len >= 16U) {
        if (fread(buf, 1, 16, file) != 16)
            return -1;
        dump_hex_line(header, base, buf, 16);
        base += 16;
        len -= 16;
    }
    if (len > 0U) {
        if (fread(buf, 1, len, file) != len)
            return -1;
        dump_hex_line(header, base, buf, len);
    }
    return 1;
}

static int dump_undefined_symbols(FILE *file, const o65_header_t *header)
{
    uint8_t buf[4];
    o65_size_t index;
    o65_size_t count;
    int result;

    /* Read the number of undefined symbols */
    if ((header->mode & O65_MODE_32BIT) == 0) {
        if (fread(buf, 1, 2, file) != 2)
            return -1;
        count = o65_read_uint16(buf);
    } else {
        if (fread(buf, 1, 4, file) != 2)
            return -1;
        count = o65_read_uint32(buf);
    }

    /* This is easy if there are no undefined symbols */
    if (count == 0) {
        printf("\nUndefined Symbols: none\n");
        return 1;
    }

    /* Dump the names of the undefined symbols */
    printf("\nUndefined Symbols:\n");
    for (index = 0; index < count; ++index) {
        printf("    %lu: ", (unsigned long)index);
        result = dump_nul_string(file);
        if (result <= 0)
            return result;
        printf("\n");
    }
    return 1;
}

static int dump_relocs
    (FILE *file, const char *name, const o65_header_t *header,
     o65_size_t addr)
{
    o65_reloc_t reloc;
    int result;

    /* Relocations actually start at the segment base - 1 */
    --addr;

    /* Read and dump all relocations for the segment */
    printf("\n%s.relocs:\n", name);
    for (;;) {
        /* Read the next relocation entry */
        result = o65_read_reloc(file, header, &reloc);
        if (result <= 0)
            return result;
        else if (reloc.offset == 0)
            break;

        /* Determine the next address to be relocated */
        if (reloc.offset == 255) {
            /* 255 indicates "skip ahead by 254 bytes" */
            addr += 254;
            continue;
        } else {
            addr += reloc.offset;
        }
        if ((header->mode & O65_MODE_32BIT) != 0)
            printf("    %08lx: ", (unsigned long)addr);
        else
            printf("    %04lx: ", (unsigned long)addr);

        /* Print the segment that the relocation destination points to */
        if ((reloc.type & O65_RELOC_SEGID) == O65_SEGID_UNDEF) {
            printf("undef %lu", (unsigned long)reloc.undefid);
        } else {
            char segname[O65_NAME_MAX];
            o65_get_segment_name(reloc.type & O65_RELOC_SEGID, segname);
            printf("%s", segname);
        }

        /* Print the relocation type plus any extra information */
        printf(", ");
        switch (reloc.type & O65_RELOC_TYPE) {
        case O65_RELOC_WORD:        printf("WORD"); break;
        case O65_RELOC_LOW:         printf("LOW"); break;
        case O65_RELOC_SEGADR:      printf("SEGADR"); break;

        case O65_RELOC_HIGH:
            if ((header->mode & O65_MODE_PAGED) == 0)
                printf("HIGH %02x", reloc.extra);
            else
                printf("HIGH");
            break;

        case O65_RELOC_SEG:
            printf("SEG %04x", reloc.extra);
            break;

        default:
            printf("RELOC-%02x", reloc.type & O65_RELOC_TYPE);
            break;
        }
        printf("\n");
    }
    return 1;
}

static int dump_exported_symbols(FILE *file, const o65_header_t *header)
{
    char segname[O65_NAME_MAX];
    uint8_t buf[4];
    o65_size_t index;
    o65_size_t count;
    o65_size_t value;
    int result;
    int ch;

    /* Read the number of exported symbols */
    if ((header->mode & O65_MODE_32BIT) == 0) {
        if (fread(buf, 1, 2, file) != 2)
            return -1;
        count = o65_read_uint16(buf);
    } else {
        if (fread(buf, 1, 4, file) != 2)
            return -1;
        count = o65_read_uint32(buf);
    }

    /* This is easy if there are no undefined symbols */
    if (count == 0) {
        printf("\nExported Symbols: none\n");
        return 1;
    }

    /* Dump the names of the undefined symbols */
    printf("\nExported Symbols:\n");
    for (index = 0; index < count; ++index) {
        /* Dump the name of the symbol */
        printf("    ");
        result = dump_nul_string(file);
        if (result <= 0)
            return result;

        /* Dump the segment identifier for the symbol */
        if ((ch = getc(file)) == EOF)
            return -1;
        o65_get_segment_name(ch, segname);
        printf(", %s", segname);

        /* Dump the value for the symbol */
        if ((header->mode & O65_MODE_32BIT) == 0) {
            if (fread(buf, 1, 2, file) != 2)
                return -1;
            value = o65_read_uint16(buf);
            printf(", 0x%04lx\n", (unsigned long)value);
        } else {
            if (fread(buf, 1, 4, file) != 2)
                return -1;
            value = o65_read_uint32(buf);
            printf(", 0x%08lx\n", (unsigned long)value);
        }
    }
    return 1;
}

static int dump_image(FILE *file, const o65_header_t *header)
{
    o65_option_t option;
    char cpu[O65_NAME_MAX];
    int result;
    int have_options;

    /* Dump the fields in the header */
    printf("Header:\n");
    printf("    mode  = 0x%04x (", header->mode);
    o65_get_cpu_name(header->mode, cpu);
    printf("%s", cpu);
    if (header->mode & O65_MODE_PAGED)
        printf(", pagewise relocation");
    if (header->mode & O65_MODE_32BIT)
        printf(", 32-bit addresses");
    else
        printf(", 16-bit addresses");
    if (header->mode & O65_MODE_OBJ)
        printf(", obj");
    else
        printf(", exe");
    if (header->mode & O65_MODE_SIMPLE)
        printf(", simple");
    if (header->mode & O65_MODE_CHAIN)
        printf(", chain");
    if (header->mode & O65_MODE_BSSZERO)
        printf(", bsszero");
    switch (header->mode & O65_MODE_ALIGN) {
    case O65_MODE_ALIGN_8:   printf(", byte alignment"); break;
    case O65_MODE_ALIGN_16:  printf(", word alignment"); break;
    case O65_MODE_ALIGN_32:  printf(", long alignment"); break;
    case O65_MODE_ALIGN_256: printf(", page alignment"); break;
    }
    printf(")\n");
    if ((header->mode & O65_MODE_32BIT) != 0) {
        printf("    tbase = 0x%08lx\n", (unsigned long)(header->tbase));
        printf("    tlen  = 0x%08lx\n", (unsigned long)(header->tlen));
        printf("    dbase = 0x%08lx\n", (unsigned long)(header->dbase));
        printf("    dlen  = 0x%08lx\n", (unsigned long)(header->dlen));
        printf("    bbase = 0x%08lx\n", (unsigned long)(header->bbase));
        printf("    blen  = 0x%08lx\n", (unsigned long)(header->blen));
        printf("    zbase = 0x%08lx\n", (unsigned long)(header->zbase));
        printf("    zlen  = 0x%08lx\n", (unsigned long)(header->zlen));
        printf("    stack = 0x%08lx\n", (unsigned long)(header->stack));
    } else {
        printf("    tbase = 0x%04x\n", header->tbase);
        printf("    tlen  = 0x%04x\n", header->tlen);
        printf("    dbase = 0x%04x\n", header->dbase);
        printf("    dlen  = 0x%04x\n", header->dlen);
        printf("    bbase = 0x%04x\n", header->bbase);
        printf("    blen  = 0x%04x\n", header->blen);
        printf("    zbase = 0x%04x\n", header->zbase);
        printf("    zlen  = 0x%04x\n", header->zlen);
        printf("    stack = 0x%04x\n", header->stack);
    }

    /* Read and dump the header options */
    have_options = 0;
    for (;;) {
        result = o65_read_option(file, &option);
        if (result <= 0)
            return result;
        if (option.len == 0)
            break;
        if (!have_options) {
            printf("\nOptions:\n");
            have_options = 1;
        }
        dump_option(&option);
    }

    /* Dump the contents of the text and data segments */
    result = dump_segment(file, ".text", header, header->tbase, header->tlen);
    if (result <= 0)
        return result;
    result = dump_segment(file, ".data", header, header->dbase, header->dlen);
    if (result <= 0)
        return result;

    /* Dump any undefined symbols */
    result = dump_undefined_symbols(file, header);
    if (result <= 0)
        return result;

    /* Dump the relocation tables for the text and data segments */
    result = dump_relocs(file, ".text", header, header->tbase);
    if (result <= 0)
        return result;
    result = dump_relocs(file, ".data", header, header->dbase);
    if (result <= 0)
        return result;

    /* Dump the list of exported symbols */
    return dump_exported_symbols(file, header);
}

static int dump_file(const char *filename)
{
    FILE *file;
    o65_header_t header;
    int result;

    /* Try to open the file */
    if ((file = fopen(filename, "rb")) == NULL) {
        perror(filename);
        return 0;
    }

    /* Dump the file's contents.  There may be multiple chained images. */
    do {
        /* Read and validate the ".o65" file header */
        result = o65_read_header(file, &header);
        if (result < 0) {
            file_error(file, filename);
            return 0;
        } else if (result == 0) {
            fprintf(stderr, "%s: not in .o65 format\n", filename);
            fclose(file);
            return 0;
        }

        /* Dump the contents of this image in the chain. */
        result = dump_image(file, &header);
        if (result < 0) {
            file_error(file, filename);
            return 0;
        } else if (result == 0) {
            fprintf(stderr, "%s: invalid format\n", filename);
            fclose(file);
            return 0;
        }

        /* Print a separator if there is another image in the chain. */
        if ((header.mode & O65_MODE_CHAIN) != 0) {
            printf("\n");
        }
    } while ((header.mode & O65_MODE_CHAIN) != 0);

    /* Done */
    fclose(file);
    return 1;
}
