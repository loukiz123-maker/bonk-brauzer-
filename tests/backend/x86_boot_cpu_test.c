/* Synthetic tables only: no vCPU, kernel or Android execution. */
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "x86_boot_cpu.h"

static uint64_t rd64(const uint8_t *at) {
    uint64_t value = 0;
    for (unsigned int i = 0; i < 8; ++i) value |= (uint64_t)at[i] << (8 * i);
    return value;
}

int main(void) {
    uint8_t *backing = calloc(1, 8 * 1024 * 1024);
    assert(backing);
    struct lynx_guest_memory ram = {backing, 8 * 1024 * 1024};
    struct lynx_boot_setup boot = {0x100200u, LYNX_BOOT_PARAMS_ADDRESS, 0x80000u, 2};
    struct lynx_cpu_state state = {0};
    assert(lynx_cpu_prepare(&ram, &boot, &state) == 0);
    assert(state.gdt_base == LYNX_GDT_ADDRESS && state.gdt_limit == 23);
    assert(state.cs == 0x10 && state.ds == 0x18 && state.cr3 == LYNX_PML4_ADDRESS);
    assert((state.cr0 & (LYNX_CR0_PE | LYNX_CR0_PG)) == (LYNX_CR0_PE | LYNX_CR0_PG));
    assert(state.cr4 & LYNX_CR4_PAE && state.efer & LYNX_EFER_LME);
    assert(state.rip == 0x100200u && state.rsi == LYNX_BOOT_PARAMS_ADDRESS);
    assert(state.rflags == 2 && state.gdt_limit == 23);
    const uint8_t *gdt = backing + LYNX_GDT_ADDRESS;
    for (int i = 0; i < 8; ++i) assert(gdt[i] == 0);
    assert(gdt[13] == 0x9a && gdt[14] == 0xa0);
    assert(gdt[21] == 0x92 && gdt[22] == 0xc0);
    assert(rd64(backing + LYNX_PML4_ADDRESS) == (LYNX_PDPT_ADDRESS | 3u));
    for (unsigned int i = 0; i < LYNX_PD_COUNT; ++i)
        assert(rd64(backing + LYNX_PDPT_ADDRESS + 8u * i) ==
               (((uint64_t)LYNX_PD_ADDRESS + i * 4096u) | 3u));
    assert(rd64(backing + LYNX_PDPT_ADDRESS + 8u * LYNX_PD_COUNT) == 0);
    for (unsigned int table = 0; table < LYNX_PD_COUNT; ++table) {
        for (unsigned int index = 0; index < 512; index += 97) {
            uint64_t expected = ((uint64_t)table * 512u + index) * LYNX_TWO_MIB;
            assert(rd64(backing + LYNX_PD_ADDRESS + table * 4096u + 8u * index) ==
                   (expected | 0x83u));
        }
    }
    assert(rd64(backing + LYNX_PD_ADDRESS + 8u * 511) == (0x3fe00000u | 0x83u));
    assert(rd64(backing + LYNX_PD_ADDRESS + 3u * 4096u + 8u * 511) ==
           (0xffe00000u | 0x83u));
    struct lynx_guest_memory too_big = {backing, LYNX_IDENTITY_LIMIT + 1};
    assert(lynx_cpu_prepare(&too_big, &boot, &state) == -1);
    struct lynx_guest_memory too_small = {backing, 1024 * 1024};
    assert(lynx_cpu_prepare(&too_small, &boot, &state) == -1);
    assert(lynx_cpu_prepare(NULL, &boot, &state) == -1);
    assert(lynx_cpu_prepare(&ram, NULL, &state) == -1);
    assert(lynx_cpu_prepare(&ram, &boot, NULL) == -1);
    free(backing);
    return 0;
}
