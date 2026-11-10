/*
 * Bounded 8259A interrupt-controller subset for the LynxDroid prototype.
 * Supports initialisation, masking, IRR/ISR, fixed priority and EOI.
 * No edge/level configuration, rotation, polling mode or SMP routing, and
 * nothing here has been validated while booting Linux or Android.
 */
#ifndef LYNXDROID_I8259_H
#define LYNXDROID_I8259_H

#include <stdint.h>

#define LYNX_PIC_MASTER_BASE 0x20u
#define LYNX_PIC_SLAVE_BASE 0xa0u
#define LYNX_PIC_IRQ_COUNT 8u
#define LYNX_PIC_CASCADE_IRQ 2u
#define LYNX_PIC_SPURIOUS_VECTOR 0x07u

struct lynx_pic {
    uint8_t irr;
    uint8_t isr;
    uint8_t imr;
    uint8_t vector_base;
    uint8_t init_step; /* 0 = idle, otherwise next ICW number */
    uint8_t expect_icw3;
    uint8_t expect_icw4;
    uint8_t auto_eoi;
    uint8_t read_isr;
    uint8_t cascade_mask;
};

struct lynx_pic_pair {
    struct lynx_pic master;
    struct lynx_pic slave;
    uint8_t initialized;
};

static inline void lynx_pic_reset(struct lynx_pic *pic) {
    pic->irr = 0;
    pic->isr = 0;
    pic->imr = 0xff; /* inputs masked until initialised */
    pic->vector_base = 0;
    pic->init_step = 0;
    pic->expect_icw3 = 0;
    pic->expect_icw4 = 0;
    pic->auto_eoi = 0;
    pic->read_isr = 0;
    pic->cascade_mask = 0;
}

static inline void lynx_pic_pair_reset(struct lynx_pic_pair *pair) {
    lynx_pic_reset(&pair->master);
    lynx_pic_reset(&pair->slave);
    pair->initialized = 0;
}

/* Fixed priority: the lowest-numbered pending, unmasked, unserviced request wins. */
static inline int lynx_pic_pending(const struct lynx_pic *pic) {
    uint8_t ready = (uint8_t)(pic->irr & (uint8_t)~pic->imr & (uint8_t)~pic->isr);
    for (unsigned int irq = 0; irq < LYNX_PIC_IRQ_COUNT; ++irq) {
        if (ready & (uint8_t)(1u << irq)) return (int)irq;
    }
    return -1;
}

/* Returns the vector to deliver, moving the request from IRR to ISR. */
static inline int lynx_pic_acknowledge(struct lynx_pic *pic) {
    int irq = lynx_pic_pending(pic);
    if (irq < 0) return -1;
    pic->irr = (uint8_t)(pic->irr & ~(uint8_t)(1u << irq));
    if (!pic->auto_eoi) pic->isr = (uint8_t)(pic->isr | (uint8_t)(1u << irq));
    return (int)pic->vector_base + irq;
}

static inline int lynx_pic_raise(struct lynx_pic *pic, unsigned int irq) {
    if (irq >= LYNX_PIC_IRQ_COUNT) return -1;
    pic->irr = (uint8_t)(pic->irr | (uint8_t)(1u << irq));
    return 0;
}

static inline int lynx_pic_set_irq(struct lynx_pic *pic, unsigned int irq, int level) {
    if (irq >= LYNX_PIC_IRQ_COUNT) return -1;
    if (level) return lynx_pic_raise(pic, irq);
    pic->irr = (uint8_t)(pic->irr & ~(uint8_t)(1u << irq));
    return 0;
}

/* Non-specific EOI clears the highest-priority in-service request. */
static inline int lynx_pic_eoi(struct lynx_pic *pic) {
    for (unsigned int irq = 0; irq < LYNX_PIC_IRQ_COUNT; ++irq) {
        if (pic->isr & (uint8_t)(1u << irq)) {
            pic->isr = (uint8_t)(pic->isr & ~(uint8_t)(1u << irq));
            return 0;
        }
    }
    return -1;
}

static inline int lynx_pic_specific_eoi(struct lynx_pic *pic, unsigned int irq) {
    if (irq >= LYNX_PIC_IRQ_COUNT) return -1;
    pic->isr = (uint8_t)(pic->isr & ~(uint8_t)(1u << irq));
    return 0;
}

/* Writes to the command port: ICW1 during init, otherwise OCW2/OCW3. */
static inline int lynx_pic_write_command(struct lynx_pic *pic, uint8_t value) {
    if (value & 0x10) { /* ICW1 */
        lynx_pic_reset(pic);
        pic->init_step = 2;
        pic->expect_icw3 = (uint8_t)(((value >> 1) & 1u) ^ 1u); /* SNGL=0 needs ICW3 */
        pic->expect_icw4 = (uint8_t)(value & 1u);
        if (value & 0x08) return -1; /* level triggered not implemented */
        return 0;
    }
    if ((value & 0x18) == 0x08) { /* OCW3 */
        if (value & 0x04) return -1; /* poll command not implemented */
        pic->read_isr = (uint8_t)(value & 1u);
        return (value & 0x40) ? -1 : 0; /* special mask mode unsupported */
    }
    if ((value & 0x18) == 0x00) { /* OCW2 */
        uint8_t command = (uint8_t)(value >> 5);
        if (command == 0x1) return lynx_pic_eoi(pic);
        if (command == 0x3) return lynx_pic_specific_eoi(pic, (unsigned int)(value & 0x07));
        return -1; /* rotation and auto-EOI commands unsupported */
    }
    return -1;
}

static inline int lynx_pic_write_data(struct lynx_pic *pic, uint8_t value) {
    switch (pic->init_step) {
    case 2:
        pic->vector_base = (uint8_t)(value & 0xf8);
        pic->init_step = pic->expect_icw3 ? 3 : (pic->expect_icw4 ? 4 : 0);
        return 0;
    case 3:
        pic->cascade_mask = value;
        pic->init_step = pic->expect_icw4 ? 4 : 0;
        return 0;
    case 4:
        pic->auto_eoi = (uint8_t)((value >> 1) & 1u);
        pic->init_step = 0;
        return (value & 0x0f) == 0x01 ? 0 : -1; /* x86 8086 mode only */
    default: /* OCW1 */
        pic->imr = value;
        return 0;
    }
}

static inline int lynx_pic_read(struct lynx_pic *pic, int command_port, uint8_t *value) {
    if (command_port) {
        *value = pic->read_isr ? pic->isr : pic->irr;
        return 0;
    }
    *value = pic->imr;
    return 0;
}

/* Cascade: a slave request appears on the master as IRQ2. */
static inline int lynx_pic_pair_raise(struct lynx_pic_pair *pair, unsigned int irq) {
    if (irq >= LYNX_PIC_IRQ_COUNT * 2u) return -1;
    if (irq < LYNX_PIC_IRQ_COUNT) return lynx_pic_raise(&pair->master, irq);
    if (lynx_pic_raise(&pair->slave, irq - LYNX_PIC_IRQ_COUNT) < 0) return -1;
    return lynx_pic_raise(&pair->master, LYNX_PIC_CASCADE_IRQ);
}

#endif
