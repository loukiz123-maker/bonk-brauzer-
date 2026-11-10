/*
 * Bounded 8254 programmable-interval-timer subset for the LynxDroid
 * prototype. Only periodic modes 2 and 3 on channel 0 produce ticks.
 * Ticks are derived from elapsed host nanoseconds, so behaviour is
 * deterministic and testable without a VM. No interrupts are delivered.
 */
#ifndef LYNXDROID_I8254_H
#define LYNXDROID_I8254_H

#include <stdint.h>

#define LYNX_PIT_BASE_FREQUENCY 1193182ull
#define LYNX_PIT_MAX_DIVISOR 65536ull

struct lynx_pit {
    uint8_t control;          /* last accepted control word */
    uint8_t mode;             /* channel 0 mode */
    uint8_t access;           /* channel 0 RW field: 1 LSB, 2 MSB, 3 LSB+MSB */
    uint8_t waiting_high;     /* low byte written, high byte expected */
    uint8_t divisor_low;
    uint32_t divisor;         /* 0 means 65536 on real hardware */
    uint64_t cycles;          /* accumulated 1193182 Hz cycles */
    uint64_t phase;           /* sub-cycle remainder scaled by 1e9 */
    uint64_t ticks;           /* periodic ticks already produced */
};

static inline void lynx_pit_reset(struct lynx_pit *pit) {
    pit->control = 0;
    pit->mode = 0;
    pit->access = 0;
    pit->waiting_high = 0;
    pit->divisor_low = 0;
    pit->divisor = 0;
    pit->cycles = 0;
    pit->phase = 0;
    pit->ticks = 0;
}

/* Writes to port 0x43; unsupported encodings are rejected. */
static inline int lynx_pit_write_control(struct lynx_pit *pit, uint8_t value) {
    unsigned int channel = value >> 6;
    unsigned int access = (value >> 4) & 0x03u;
    unsigned int mode = (value >> 1) & 0x07u;
    if (channel == 3) return -1;        /* read-back command unsupported */
    if (access == 0) return -1;         /* counter latch unsupported */
    if (mode == 6 || mode == 7) return -1;
    if (value & 0x01) return -1;        /* BCD counting unsupported */
    if (channel != 0) return 0;         /* accepted but no timing modelled */
    pit->control = value;
    pit->mode = (uint8_t)mode;
    pit->access = (uint8_t)access;
    pit->waiting_high = 0;
    return 0;
}

/* Writes to port 0x40..0x42. Only channel 0 timing is modelled. */
static inline int lynx_pit_write_data(struct lynx_pit *pit, unsigned int channel, uint8_t value) {
    if (channel > 2) return -1;
    if (channel != 0) return 0;
    if (pit->access == 0) return -1;    /* no control word yet */
    if (pit->access == 1) {
        pit->divisor = value ? value : LYNX_PIT_MAX_DIVISOR;
        return 0;
    }
    if (pit->access == 2) {
        pit->divisor = value ? (uint32_t)value << 8 : LYNX_PIT_MAX_DIVISOR;
        return 0;
    }
    if (!pit->waiting_high) {
        pit->divisor_low = value;
        pit->waiting_high = 1;
        return 0;
    }
    uint32_t divisor = ((uint32_t)value << 8) | pit->divisor_low;
    pit->divisor = divisor ? divisor : (uint32_t)LYNX_PIT_MAX_DIVISOR;
    pit->waiting_high = 0;
    return 0;
}

/*
 * Advance by elapsed nanoseconds and return the number of periodic ticks.
 * Only modes 2 and 3 are periodic; other modes never produce ticks here.
 */
static inline uint64_t lynx_pit_advance(struct lynx_pit *pit, uint64_t elapsed_ns) {
    if (pit->divisor == 0 || (pit->mode != 2 && pit->mode != 3)) return 0;
    uint64_t whole_seconds = elapsed_ns / 1000000000ull;
    uint64_t remainder_ns = elapsed_ns % 1000000000ull;
    pit->cycles += whole_seconds * LYNX_PIT_BASE_FREQUENCY;
    pit->phase += remainder_ns * LYNX_PIT_BASE_FREQUENCY;
    pit->cycles += pit->phase / 1000000000ull;
    pit->phase %= 1000000000ull;
    uint64_t ticks = pit->cycles / pit->divisor;
    pit->cycles %= pit->divisor;
    pit->ticks += ticks;
    return ticks;
}

#endif
