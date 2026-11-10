/* Synthetic kvm_run buffers only; no KVM device is opened. */
#include <assert.h>
#include <linux/kvm.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "kvm_io.h"

int main(void) {
    const size_t length = 8192, data_offset = 4096;
    struct kvm_run *run = calloc(1, length);
    assert(run != NULL);
    assert(sizeof(*run) < data_offset);
    run->exit_reason = KVM_EXIT_IO;
    run->io.direction = KVM_EXIT_IO_OUT;
    run->io.port = LYNX_DEBUG_PORT;
    run->io.size = 1;
    run->io.count = 1;
    run->io.data_offset = data_offset;
    uint8_t *data = (uint8_t *)run + data_offset;
    const char *expected = "OK\n";
    size_t received = 0;
    data[0] = 'O';
    assert(lynx_consume_output(run, length, expected, 3, &received) == 0 && received == 1);
    memcpy(data, "K\n", 2);
    run->io.count = 2;
    assert(lynx_consume_output(run, length, expected, 3, &received) == 0 && received == 3);
    assert(lynx_consume_output(run, length, expected, 3, &received) == -1 && received == 3);
    received = 0;
    data[0] = 'X';
    run->io.count = 1;
    assert(lynx_consume_output(run, length, expected, 3, &received) == -1 && received == 0);
    data[0] = 'O';
    run->io.data_offset = UINT64_MAX;
    assert(lynx_consume_output(run, length, expected, 3, &received) == -1);
    run->io.data_offset = length - 1;
    run->io.count = UINT32_MAX;
    assert(lynx_consume_output(run, length, expected, 3, &received) == -1);
    run->io.data_offset = data_offset;
    run->io.count = 1;
    run->io.port = 0x60;
    assert(lynx_consume_output(run, length, expected, 3, &received) == -1);
    run->io.port = LYNX_DEBUG_PORT;
    run->io.direction = KVM_EXIT_IO_IN;
    assert(lynx_consume_output(run, length, expected, 3, &received) == -1);
    run->io.direction = KVM_EXIT_IO_OUT;
    run->io.size = 4;
    assert(lynx_consume_output(run, length, expected, 3, &received) == -1);
    assert(lynx_consume_output(run, 1, expected, 3, &received) == -1);
    assert(received == 0);
    free(run);
    return 0;
}
