/*
 * Offline staging of a modern Linux/x86 bzImage into guest RAM.
 * This is NOT a bootloader: no boot_params, E820, vCPU setup or devices.
 * Format reference: Linux Documentation/arch/x86/boot.rst.
 */
#ifndef LYNXDROID_X86_LINUX_STAGE_H
#define LYNXDROID_X86_LINUX_STAGE_H

#include <stddef.h>
#include <stdint.h>
#include "guest_memory.h"

#define LYNX_LINUX_HEADER_MIN 0x264u
#define LYNX_LINUX_SETUP_ADDRESS 0x10000u
#define LYNX_LINUX_KERNEL_ADDRESS 0x100000u
#define LYNX_LINUX_MAX_SETUP_BYTES 0x8000u

struct lynx_linux_image {
    uint16_t protocol;
    uint16_t xloadflags;
    uint32_t cmdline_max;
    uint32_t init_size;
    uint32_t kernel_alignment;
    uint64_t preferred_address;
    uint8_t relocatable;
    size_t setup_bytes;
    size_t kernel_bytes;
};

static inline uint16_t lynx_le16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t lynx_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint64_t lynx_le64(const uint8_t *p) {
    return (uint64_t)lynx_le32(p) | ((uint64_t)lynx_le32(p + 4) << 32);
}

/* Recognize a deliberately narrow subset, without asserting authenticity. */
static inline int lynx_linux_inspect(const uint8_t *image, size_t length,
                                     struct lynx_linux_image *info) {
    if (!image || !info || length < LYNX_LINUX_HEADER_MIN) return -1;
    if (lynx_le16(image + 0x1fe) != 0xaa55 ||
        image[0x202] != 'H' || image[0x203] != 'd' ||
        image[0x204] != 'r' || image[0x205] != 'S') return -1;
    uint16_t version = lynx_le16(image + 0x206);
    if (version < 0x020c || !(image[0x211] & 1) || !(lynx_le16(image + 0x236) & 1))
        return -1; /* require protocol >= 2.12, LOAD_HIGH and 64-bit kernel */
    unsigned int setup_sectors = image[0x1f1] ? image[0x1f1] : 4;
    size_t setup_bytes = ((size_t)setup_sectors + 1) * 512;
    uint64_t kernel_bytes = (uint64_t)lynx_le32(image + 0x1f4) * 16;
    if (setup_bytes > LYNX_LINUX_MAX_SETUP_BYTES || setup_bytes > length ||
        kernel_bytes == 0 || kernel_bytes > length - setup_bytes) return -1;
    struct lynx_linux_image result = {
        .protocol = version,
        .xloadflags = lynx_le16(image + 0x236),
        .cmdline_max = lynx_le32(image + 0x238),
        .init_size = lynx_le32(image + 0x260),
        .kernel_alignment = lynx_le32(image + 0x230),
        .preferred_address = lynx_le64(image + 0x258),
        .relocatable = image[0x234],
        .setup_bytes = setup_bytes,
        .kernel_bytes = (size_t)kernel_bytes,
    };
    *info = result;
    return 0;
}

/* Checked address range overlap, avoiding wraparound. */
static inline int lynx_ranges_overlap(uintptr_t a, size_t a_size, uintptr_t b, size_t b_size) {
    if (a_size > UINTPTR_MAX - a || b_size > UINTPTR_MAX - b) return 1;
    return a < b + b_size && b < a + a_size;
}

/*
 * Stage setup and protected-mode bytes at the conventional addresses.
 * Validate ALL ranges, RAM requirements and source aliasing before writing.
 * Caller must still construct boot_params and a compatible guest environment.
 */
static inline int lynx_linux_stage(struct lynx_guest_memory *ram, const uint8_t *image,
                                   size_t length, struct lynx_linux_image *info) {
    struct lynx_linux_image parsed;
    if (!ram || !ram->host || !info || lynx_linux_inspect(image, length, &parsed) < 0) return -1;
    /* init_size applies at the kernel runtime address, not necessarily 1 MiB. */
    uint64_t runtime = parsed.preferred_address;
    if (parsed.relocatable) {
        uint64_t alignment = parsed.kernel_alignment;
        if (!alignment || (alignment & (alignment - 1))) return -1;
        if (runtime < LYNX_LINUX_KERNEL_ADDRESS) runtime = LYNX_LINUX_KERNEL_ADDRESS;
        if (runtime > UINT64_MAX - (alignment - 1)) return -1;
        runtime = (runtime + alignment - 1) & ~(alignment - 1);
    }
    if (runtime < LYNX_LINUX_KERNEL_ADDRESS || !parsed.init_size ||
        !lynx_guest_range_valid(ram, LYNX_LINUX_SETUP_ADDRESS, parsed.setup_bytes) ||
        !lynx_guest_range_valid(ram, LYNX_LINUX_KERNEL_ADDRESS, parsed.kernel_bytes) ||
        !lynx_guest_range_valid(ram, runtime, parsed.init_size) ||
        lynx_ranges_overlap((uintptr_t)ram->host, ram->size, (uintptr_t)image, length) ||
        lynx_ranges_overlap((uintptr_t)ram->host, ram->size, (uintptr_t)info, sizeof(*info)) ||
        lynx_ranges_overlap((uintptr_t)image, length, (uintptr_t)info, sizeof(*info)) ||
        lynx_ranges_overlap((uintptr_t)ram->host, ram->size, (uintptr_t)ram, sizeof(*ram))) return -1;
    if (lynx_guest_write(ram, LYNX_LINUX_SETUP_ADDRESS, image, parsed.setup_bytes) < 0 ||
        lynx_guest_write(ram, LYNX_LINUX_KERNEL_ADDRESS,
                         image + parsed.setup_bytes, parsed.kernel_bytes) < 0) return -1;
    *info = parsed;
    return 0;
}

#endif
