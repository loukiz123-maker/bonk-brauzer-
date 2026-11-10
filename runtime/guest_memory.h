/* Bounded accesses to a single contiguous guest RAM region, not an MMU. */
#ifndef LYNXDROID_GUEST_MEMORY_H
#define LYNXDROID_GUEST_MEMORY_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct lynx_guest_memory {
    uint8_t *host;
    size_t size;
};

/* Subtraction after validating the offset avoids address + length overflow. */
static inline int lynx_guest_range_valid(const struct lynx_guest_memory *memory,
                                         uint64_t address, size_t length) {
    if (memory == NULL || memory->host == NULL || address > memory->size) return 0;
    return length <= memory->size - (size_t)address;
}

static inline int lynx_guest_write(struct lynx_guest_memory *memory, uint64_t address,
                                   const void *source, size_t length) {
    if (!lynx_guest_range_valid(memory, address, length) || (length != 0 && source == NULL))
        return -1;
    if (length != 0) memmove(memory->host + (size_t)address, source, length);
    return 0;
}

static inline int lynx_guest_read(const struct lynx_guest_memory *memory, uint64_t address,
                                  void *destination, size_t length) {
    if (!lynx_guest_range_valid(memory, address, length) || (length != 0 && destination == NULL))
        return -1;
    if (length != 0) memmove(destination, memory->host + (size_t)address, length);
    return 0;
}

#endif
