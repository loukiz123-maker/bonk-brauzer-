/* Minimal bounded polling-only 8250-style register subset for experiments.
 * No interrupts, FIFO, host input, timing, or production device compatibility.
 */
#ifndef LYNXDROID_SERIAL8250_H
#define LYNXDROID_SERIAL8250_H

#include <linux/kvm.h>
#include <stddef.h>
#include <stdint.h>

#define LYNX_SERIAL_BASE 0x3f8u
#define LYNX_SERIAL_CAPACITY 256u

struct lynx_serial {
    uint8_t lcr;
    uint8_t divisor_low;
    uint8_t divisor_high;
    uint8_t scratch;
    uint8_t output[LYNX_SERIAL_CAPACITY];
    size_t output_size;
};

/* Only operations whose effects we actually implement are accepted. */
static inline int lynx_serial_write(struct lynx_serial *serial, unsigned int reg, uint8_t value) {
    switch (reg) {
    case 0:
        if (serial->lcr & 0x80) serial->divisor_low = value;
        else {
            if (serial->output_size >= LYNX_SERIAL_CAPACITY) return -1;
            serial->output[serial->output_size++] = value;
        }
        return 0;
    case 1:
        if (serial->lcr & 0x80) { serial->divisor_high = value; return 0; }
        return value == 0 ? 0 : -1; /* interrupts are not implemented */
    case 2: return value == 0 ? 0 : -1; /* no FIFO */
    case 3:
        if (value & 0x40) return -1; /* no break signalling */
        serial->lcr = value;
        return 0;
    case 4: return value == 0 ? 0 : -1; /* no modem control */
    case 7: serial->scratch = value; return 0;
    default: return -1;
    }
}

static inline int lynx_serial_read(const struct lynx_serial *serial, unsigned int reg, uint8_t *value) {
    switch (reg) {
    case 0:
        if (!(serial->lcr & 0x80)) return -1; /* no receive data */
        *value = serial->divisor_low;
        return 0;
    case 1: *value = (serial->lcr & 0x80) ? serial->divisor_high : 0; return 0;
    case 2: *value = 1; return 0; /* no interrupt pending; no FIFO */
    case 3: *value = serial->lcr; return 0;
    case 4: *value = 0; return 0;
    case 5: *value = 0x60; return 0; /* transmit register and shift register empty */
    case 6: *value = 0; return 0;
    case 7: *value = serial->scratch; return 0;
    default: return -1;
    }
}

/* Single-byte IN/OUT KVM exits only; reject unsupported operations. */
static inline int lynx_serial_io(struct lynx_serial *serial, struct kvm_run *run, size_t map_size) {
    if (map_size < sizeof(*run) || run->exit_reason != KVM_EXIT_IO ||
        run->io.port < LYNX_SERIAL_BASE || run->io.port >= LYNX_SERIAL_BASE + 8 ||
        run->io.size != 1 || run->io.count != 1 ||
        run->io.data_offset < sizeof(*run) || run->io.data_offset >= map_size) return -1;
    uint8_t *data = (uint8_t *)run + (size_t)run->io.data_offset;
    unsigned int reg = run->io.port - LYNX_SERIAL_BASE;
    if (run->io.direction == KVM_EXIT_IO_OUT) return lynx_serial_write(serial, reg, *data);
    if (run->io.direction == KVM_EXIT_IO_IN) return lynx_serial_read(serial, reg, data);
    return -1;
}

#endif
