/* Synthetic device accesses only; does not require /dev/kvm. */
#include <assert.h>
#include <linux/kvm.h>
#include <stdint.h>
#include <stdlib.h>
#include "serial8250.h"

int main(void) {
    struct lynx_serial serial = {0};
    uint8_t value = 0;
    assert(lynx_serial_read(&serial, 5, &value) == 0 && value == 0x60);
    assert(lynx_serial_read(&serial, 0, &value) == 0 && value == 0); /* empty RBR */
    assert(lynx_serial_write(&serial, 1, 1) == -1); /* no IRQs */
    assert(lynx_serial_write(&serial, 2, 1) == -1); /* no FIFO */
    assert(lynx_serial_write(&serial, 4, 1) == -1); /* no modem control */
    assert(lynx_serial_write(&serial, 3, 0x80) == 0);
    assert(lynx_serial_write(&serial, 0, 0x12) == 0);
    assert(lynx_serial_write(&serial, 1, 0x34) == 0);
    assert(lynx_serial_read(&serial, 0, &value) == 0 && value == 0x12);
    assert(lynx_serial_read(&serial, 1, &value) == 0 && value == 0x34);
    assert(serial.output_size == 0);
    assert(lynx_serial_write(&serial, 3, 0) == 0);
    assert(lynx_serial_write(&serial, 7, 0x42) == 0);
    assert(lynx_serial_read(&serial, 7, &value) == 0 && value == 0x42);
    assert(lynx_serial_write(&serial, 0, 'A') == 0 && serial.output[0] == 'A');
    serial.output_size = LYNX_SERIAL_CAPACITY;
    assert(lynx_serial_write(&serial, 0, 'Z') == -1);
    serial.output_size = 0;

    /* Host input is polled through LSR and consumed in order. */
    assert(lynx_serial_read(&serial, 5, &value) == 0 && (value & 0x01) == 0);
    assert(lynx_serial_push_input(&serial, 'h') == 0);
    assert(lynx_serial_push_input(&serial, 'i') == 0);
    assert(lynx_serial_read(&serial, 5, &value) == 0 && (value & 0x01) != 0);
    assert(lynx_serial_read(&serial, 0, &value) == 0 && value == 'h');
    assert(lynx_serial_read(&serial, 0, &value) == 0 && value == 'i');
    assert(lynx_serial_read(&serial, 5, &value) == 0 && (value & 0x01) == 0);
    assert(lynx_serial_read(&serial, 0, &value) == 0 && value == 0);
    for (unsigned int i = 0; i < LYNX_SERIAL_CAPACITY; ++i)
        assert(lynx_serial_push_input(&serial, (uint8_t)i) == 0);
    assert(lynx_serial_push_input(&serial, 0xff) == -1);
    for (unsigned int i = 0; i < LYNX_SERIAL_CAPACITY; ++i) {
        assert(lynx_serial_read(&serial, 0, &value) == 0 && value == (uint8_t)i);
    }
    assert(serial.input_size == 0);
    assert(lynx_serial_push_input(&serial, 'x') == 0);
    serial.lcr = 0x80; /* divisor access must not consume input */
    assert(lynx_serial_read(&serial, 0, &value) == 0 && value == serial.divisor_low);
    serial.lcr = 0;
    assert(lynx_serial_read(&serial, 0, &value) == 0 && value == 'x');

    size_t length = 8192, data_offset = 4096;
    struct kvm_run *run = calloc(1, length);
    assert(run != NULL && sizeof(*run) < data_offset);
    run->exit_reason = KVM_EXIT_IO;
    run->io.port = LYNX_SERIAL_BASE;
    run->io.direction = KVM_EXIT_IO_OUT;
    run->io.size = 1;
    run->io.count = 1;
    run->io.data_offset = data_offset;
    uint8_t *data = (uint8_t *)run + data_offset;
    *data = 'U';
    assert(lynx_serial_io(&serial, run, length) == 0);
    assert(serial.output_size == 1 && serial.output[0] == 'U');
    run->io.direction = KVM_EXIT_IO_IN;
    run->io.port = LYNX_SERIAL_BASE + 5;
    assert(lynx_serial_io(&serial, run, length) == 0 && *data == 0x60);
    assert(lynx_serial_push_input(&serial, 'k') == 0);
    run->io.port = LYNX_SERIAL_BASE + 5;
    assert(lynx_serial_io(&serial, run, length) == 0 && (*data & 0x01) != 0);
    run->io.port = LYNX_SERIAL_BASE;
    assert(lynx_serial_io(&serial, run, length) == 0 && *data == 'k');
    run->io.port = LYNX_SERIAL_BASE + 5;
    run->io.data_offset = UINT64_MAX;
    assert(lynx_serial_io(&serial, run, length) == -1);
    run->io.data_offset = 0;
    assert(lynx_serial_io(&serial, run, length) == -1);
    run->io.data_offset = data_offset;
    run->io.port = LYNX_SERIAL_BASE + 8;
    assert(lynx_serial_io(&serial, run, length) == -1);
    run->io.port = LYNX_SERIAL_BASE;
    run->io.count = 2;
    assert(lynx_serial_io(&serial, run, length) == -1);
    assert(serial.output_size == 1);
    free(run);
    return 0;
}
