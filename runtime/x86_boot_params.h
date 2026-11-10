/*
 * Builds a minimal Linux/x86 zero page (struct boot_params) for the
 * 64-bit boot protocol. No VM is created and no kernel is executed.
 * Field offsets: Linux Documentation/arch/x86/zero-page.rst.
 */
#ifndef LYNXDROID_X86_BOOT_PARAMS_H
#define LYNXDROID_X86_BOOT_PARAMS_H

#include <stddef.h>
#include <stdint.h>
#include "guest_memory.h"
#include "x86_linux_stage.h"

#define LYNX_BOOT_PARAMS_SIZE 4096u
#define LYNX_BOOT_PARAMS_ADDRESS 0x7000u
#define LYNX_CMDLINE_ADDRESS 0x80000u
#define LYNX_CMDLINE_CAPACITY 2048u
#define LYNX_LOADER_TYPE 0xffu
#define LYNX_HEAP_END 0xde00u
#define LYNX_E820_MAX 128u

/* Linux boot protocol: 64-bit entry point sits 0x200 into the kernel. */
#define LYNX_KERNEL_ENTRY_OFFSET 0x200u

struct lynx_boot_setup {
    uint64_t entry_address;   /* physical address for RIP */
    uint32_t boot_params_addr; /* physical address for RSI */
    uint32_t cmdline_address;
    uint8_t e820_entries;
};

static inline void lynx_put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void lynx_put32(uint8_t *p, uint32_t v) {
    for (unsigned int i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i));
}
static inline void lynx_put64(uint8_t *p, uint64_t v) {
    for (unsigned int i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (8 * i));
}

/* One reserved entry plus usable RAM, matching a single contiguous backing buffer. */
static inline int lynx_build_e820(uint8_t *bp, uint64_t ram_bytes) {
    if (ram_bytes <= 0x100000u) return -1;
    const uint64_t low = 0x9fc00u; /* conventional memory below the EBDA area */
    uint8_t *table = bp + 0x2d0;
    lynx_put64(table, 0);
    lynx_put64(table + 8, low);
    lynx_put32(table + 16, 1); /* E820_TYPE_RAM */
    lynx_put64(table + 20, 0x100000u);
    lynx_put64(table + 28, ram_bytes - 0x100000u);
    lynx_put32(table + 36, 1);
    bp[0x1e8] = 2;
    return 0;
}

/*
 * Fill a 4096-byte zero page and the command line, then copy both into RAM.
 * The caller still needs paging, a GDT, segment registers and devices.
 */
static inline int lynx_linux_prepare(struct lynx_guest_memory *ram, const uint8_t *image,
                                     size_t length, const char *cmdline,
                                     struct lynx_boot_setup *setup) {
    if (!ram || !ram->host || !setup || !cmdline) return -1;
    struct lynx_linux_image info;
    if (lynx_linux_stage(ram, image, length, &info) < 0) return -1;
    size_t cmdline_length = 0;
    while (cmdline[cmdline_length] != '\0') {
        if (++cmdline_length >= LYNX_CMDLINE_CAPACITY) return -1;
    }
    uint32_t limit = info.cmdline_max ? info.cmdline_max : 255u;
    if (cmdline_length >= limit) return -1;
    if (!lynx_guest_range_valid(ram, LYNX_BOOT_PARAMS_ADDRESS, LYNX_BOOT_PARAMS_SIZE) ||
        !lynx_guest_range_valid(ram, LYNX_CMDLINE_ADDRESS, cmdline_length + 1)) return -1;
    uint8_t zero_page[LYNX_BOOT_PARAMS_SIZE];
    for (size_t i = 0; i < sizeof(zero_page); ++i) zero_page[i] = 0;
    /* Copy the setup header (0x1f1 onwards) so the kernel sees its own fields. */
    size_t header_end = 0x202u + image[0x201];
    if (header_end > LYNX_LINUX_HEADER_MIN + 2u) header_end = LYNX_LINUX_HEADER_MIN + 2u;
    if (lynx_guest_read(ram, LYNX_LINUX_SETUP_ADDRESS + 0x1f1, zero_page + 0x1f1,
                        header_end - 0x1f1) < 0) return -1;
    zero_page[0x210] = LYNX_LOADER_TYPE;
    zero_page[0x211] |= 0x80; /* CAN_USE_HEAP */
    lynx_put16(zero_page + 0x224, (uint16_t)(LYNX_HEAP_END - 0x200));
    lynx_put32(zero_page + 0x228, LYNX_CMDLINE_ADDRESS);
    if (lynx_build_e820(zero_page, ram->size) < 0) return -1;
    if (lynx_guest_write(ram, LYNX_BOOT_PARAMS_ADDRESS, zero_page, sizeof(zero_page)) < 0 ||
        lynx_guest_write(ram, LYNX_CMDLINE_ADDRESS, cmdline, cmdline_length + 1) < 0) return -1;
    struct lynx_boot_setup result = {
        .entry_address = (uint64_t)LYNX_LINUX_KERNEL_ADDRESS + LYNX_KERNEL_ENTRY_OFFSET,
        .boot_params_addr = LYNX_BOOT_PARAMS_ADDRESS,
        .cmdline_address = LYNX_CMDLINE_ADDRESS,
        .e820_entries = 2,
    };
    *setup = result;
    return 0;
}

#endif
