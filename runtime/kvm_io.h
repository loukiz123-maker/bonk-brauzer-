/* Bounded handling for the demo guest's debug-port output. Not a device model. */
#ifndef LYNXDROID_KVM_IO_H
#define LYNXDROID_KVM_IO_H

#include <linux/kvm.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define LYNX_DEBUG_PORT 0xe9u

/*
 * Accept exactly a prefix of the expected message, one or more bytes at a time.
 * Every offset and count is checked before touching the shared mapping. Unknown
 * ports, directions, sizes, extra data or unexpected bytes are rejected.
 */
static int lynx_consume_output(const struct kvm_run *run, size_t map_size,
                               const char *expected, size_t expected_size,
                               size_t *received) {
    if (map_size < sizeof(*run) || run->exit_reason != KVM_EXIT_IO ||
        run->io.direction != KVM_EXIT_IO_OUT || run->io.port != LYNX_DEBUG_PORT ||
        run->io.size != 1 || run->io.count == 0 || *received > expected_size) {
        return -1;
    }
    uint64_t offset = run->io.data_offset;
    size_t count = run->io.count;
    if (offset > map_size || count > map_size - (size_t)offset ||
        count > expected_size - *received) {
        return -1;
    }
    const uint8_t *bytes = (const uint8_t *)run + (size_t)offset;
    if (memcmp(bytes, expected + *received, count) != 0) return -1;
    *received += count;
    return 0;
}

#endif
