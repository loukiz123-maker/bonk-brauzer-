/*
 * Builds the CPU hand-off state required by the Linux/x86 64-bit boot
 * protocol: a flat GDT and identity page tables. No VM is created, no
 * kernel is executed and no registers are written to a real vCPU.
 * Reference: Linux Documentation/arch/x86/boot.rst.
 */
#ifndef LYNXDROID_X86_BOOT_CPU_H
#define LYNXDROID_X86_BOOT_CPU_H

#include <stddef.h>
#include <stdint.h>
#include "x86_boot_params.h"

#define LYNX_GDT_ADDRESS 0xa00u
#define LYNX_PML4_ADDRESS 0x1000u
#define LYNX_PDPT_ADDRESS 0x2000u
#define LYNX_PD_ADDRESS 0x3000u
#define LYNX_PD_COUNT 4u
#define LYNX_TABLES_END (LYNX_PD_ADDRESS + LYNX_PD_COUNT * 4096u)
#define LYNX_PAGE_SIZE 4096u
#define LYNX_TWO_MIB 0x200000ull
#define LYNX_IDENTITY_LIMIT 0x100000000ull
#define LYNX_BOOT_CS 0x10u
#define LYNX_BOOT_DS 0x18u
#define LYNX_CR0_PE (1ull << 0)
#define LYNX_CR0_PG (1ull << 31)
#define LYNX_CR4_PAE (1ull << 5)
#define LYNX_EFER_LME (1ull << 8)

_Static_assert(LYNX_TABLES_END <= LYNX_BOOT_PARAMS_ADDRESS,
               "page tables must not overlap boot_params");
_Static_assert(LYNX_GDT_ADDRESS + 24u <= LYNX_LINUX_SETUP_ADDRESS,
               "GDT must stay below the setup region");

struct lynx_cpu_state {
    uint64_t gdt_base;
    uint16_t gdt_limit;
    uint16_t cs;
    uint16_t ds;
    uint64_t cr0;
    uint64_t cr3;
    uint64_t cr4;
    uint64_t efer;
    uint64_t rip;
    uint64_t rsi;
    uint64_t rflags;
};

/* 64-bit code segment: executable, readable, 4 GiB flat. */
static inline void lynx_gdt_code(uint8_t *entry) {
    lynx_put16(entry + 0, 0xffff);
    lynx_put16(entry + 2, 0x0000);
    entry[4] = 0x00;
    entry[5] = 0x9a; /* present, ring 0, code, readable */
    entry[6] = 0xa0; /* 4 KiB granularity, long mode */
    entry[7] = 0x00;
}

/* 64-bit data segment: writable, 4 GiB flat. */
static inline void lynx_gdt_data(uint8_t *entry) {
    lynx_put16(entry + 0, 0xffff);
    lynx_put16(entry + 2, 0x0000);
    entry[4] = 0x00;
    entry[5] = 0x92; /* present, ring 0, data, writable */
    entry[6] = 0xc0; /* 4 KiB granularity, 32-bit default */
    entry[7] = 0x00;
}

/*
 * Identity-map the first 4 GiB using 2 MiB pages. Guest RAM must fit in
 * that window; larger guests need a different table builder.
 */
static inline int lynx_cpu_prepare(struct lynx_guest_memory *ram,
                                   const struct lynx_boot_setup *boot,
                                   struct lynx_cpu_state *state) {
    if (!ram || !ram->host || !boot || !state) return -1;
    if (ram->size < LYNX_TWO_MIB || ram->size > LYNX_IDENTITY_LIMIT) return -1;
    if (!lynx_guest_range_valid(ram, LYNX_GDT_ADDRESS, 8u * 3u) ||
        !lynx_guest_range_valid(ram, LYNX_PML4_ADDRESS, (size_t)LYNX_TABLES_END - LYNX_PML4_ADDRESS))
        return -1;

    uint8_t gdt[24];
    for (size_t i = 0; i < sizeof(gdt); ++i) gdt[i] = 0;
    lynx_gdt_code(gdt + 8);
    lynx_gdt_data(gdt + 16);
    if (lynx_guest_write(ram, LYNX_GDT_ADDRESS, gdt, sizeof(gdt)) < 0) return -1;

    uint8_t pml4[LYNX_PAGE_SIZE];
    uint8_t pdpt[LYNX_PAGE_SIZE];
    uint8_t pd[LYNX_PD_COUNT * LYNX_PAGE_SIZE];
    for (size_t i = 0; i < sizeof(pml4); ++i) pml4[i] = 0;
    for (size_t i = 0; i < sizeof(pdpt); ++i) pdpt[i] = 0;
    for (size_t i = 0; i < sizeof(pd); ++i) pd[i] = 0;
    lynx_put64(pml4 + 0, LYNX_PDPT_ADDRESS | 3u); /* present, writable */
    for (unsigned int i = 0; i < LYNX_PD_COUNT; ++i)
        lynx_put64(pdpt + 8u * i, (uint64_t)(LYNX_PD_ADDRESS + i * LYNX_PAGE_SIZE) | 3u);
    for (unsigned int table = 0; table < LYNX_PD_COUNT; ++table) {
        for (unsigned int index = 0; index < 512; ++index) {
            uint64_t address = ((uint64_t)table * 512u + index) * LYNX_TWO_MIB;
            lynx_put64(pd + table * LYNX_PAGE_SIZE + 8u * index, address | 0x83u); /* PS|RW|P */
        }
    }
    if (lynx_guest_write(ram, LYNX_PML4_ADDRESS, pml4, sizeof(pml4)) < 0 ||
        lynx_guest_write(ram, LYNX_PDPT_ADDRESS, pdpt, sizeof(pdpt)) < 0 ||
        lynx_guest_write(ram, LYNX_PD_ADDRESS, pd, sizeof(pd)) < 0) return -1;

    struct lynx_cpu_state result = {
        .gdt_base = LYNX_GDT_ADDRESS,
        .gdt_limit = (uint16_t)(8u * 3u - 1u),
        .cs = LYNX_BOOT_CS,
        .ds = LYNX_BOOT_DS,
        .cr0 = LYNX_CR0_PE | LYNX_CR0_PG,
        .cr3 = LYNX_PML4_ADDRESS,
        .cr4 = LYNX_CR4_PAE,
        .efer = LYNX_EFER_LME,
        .rip = boot->entry_address,
        .rsi = boot->boot_params_addr,
        .rflags = 0x2,
    };
    *state = result;
    return 0;
}

#endif
