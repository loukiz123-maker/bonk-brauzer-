/* Synthetic PIC exercises only; no VM, no Linux and no Android. */
#include <assert.h>
#include <stdint.h>
#include "i8259.h"

static void init_master(struct lynx_pic *pic, uint8_t base) {
    assert(lynx_pic_write_command(pic, 0x11) == 0); /* ICW1: edge, ICW4 */
    assert(lynx_pic_write_data(pic, base) == 0);    /* ICW2 */
    assert(lynx_pic_write_data(pic, 0x04) == 0);    /* ICW3 */
    assert(lynx_pic_write_data(pic, 0x01) == 0);    /* ICW4: 8086 */
    assert(pic->init_step == 0 && pic->vector_base == base);
}

int main(void) {
    struct lynx_pic pic;
    lynx_pic_reset(&pic);
    assert(pic.imr == 0xff);
    assert(lynx_pic_pending(&pic) == -1);
    assert(lynx_pic_raise(&pic, 8) == -1);
    init_master(&pic, 0x20);
    assert(lynx_pic_write_data(&pic, 0x00) == 0); /* unmask all */
    assert(lynx_pic_acknowledge(&pic) == -1);     /* nothing pending */

    assert(lynx_pic_raise(&pic, 3) == 0);
    assert(lynx_pic_raise(&pic, 1) == 0);
    assert(lynx_pic_pending(&pic) == 1);          /* fixed priority */
    assert(lynx_pic_acknowledge(&pic) == 0x21);
    assert((pic.isr & 0x02) != 0 && (pic.irr & 0x02) == 0);
    assert(lynx_pic_pending(&pic) == 3);          /* masked by ISR */
    assert(lynx_pic_acknowledge(&pic) == 0x23);
    assert(lynx_pic_eoi(&pic) == 0 && (pic.isr & 0x02) == 0);
    assert(lynx_pic_pending(&pic) == -1);         /* both requests in service */
    assert(lynx_pic_eoi(&pic) == 0);
    assert(lynx_pic_eoi(&pic) == -1);

    assert(lynx_pic_raise(&pic, 5) == 0);
    assert(lynx_pic_write_data(&pic, 0x20) == 0); /* OCW1 mask IRQ5 */
    assert(lynx_pic_pending(&pic) == -1);
    assert(lynx_pic_set_irq(&pic, 5, 0) == 0 && pic.irr == 0);
    assert(lynx_pic_set_irq(&pic, 5, 1) == 0 && pic.irr != 0);
    assert(lynx_pic_write_data(&pic, 0x00) == 0);
    assert(lynx_pic_acknowledge(&pic) == 0x25);
    assert(lynx_pic_specific_eoi(&pic, 5) == 0 && pic.isr == 0);

    uint8_t value = 0;
    assert(lynx_pic_raise(&pic, 4) == 0);
    assert(lynx_pic_read(&pic, 1, &value) == 0 && (value & 0x10) != 0);
    assert(lynx_pic_write_command(&pic, 0x0b) == 0); /* OCW3: read ISR */
    assert(lynx_pic_read(&pic, 1, &value) == 0 && value == 0);
    assert(lynx_pic_acknowledge(&pic) == 0x24);
    assert(lynx_pic_read(&pic, 1, &value) == 0 && (value & 0x10) != 0);
    assert(lynx_pic_write_command(&pic, 0x08) == 0); /* OCW3: read IRR */
    assert(lynx_pic_read(&pic, 1, &value) == 0 && value == 0);
    assert(lynx_pic_write_command(&pic, 0x20) == 0); /* non-specific EOI */
    assert(pic.isr == 0);
    assert(lynx_pic_write_command(&pic, 0x0c) == -1); /* poll mode */
    assert(lynx_pic_write_command(&pic, 0x48) == -1); /* special mask */
    assert(lynx_pic_write_command(&pic, 0xa0) == -1); /* rotate */
    assert(lynx_pic_write_command(&pic, 0xe0) == -1); /* set priority */
    assert(lynx_pic_raise(&pic, 0) == 0);
    assert(lynx_pic_read(&pic, 0, &value) == 0 && pic.imr == 0x00);

    struct lynx_pic_pair pair;
    lynx_pic_pair_reset(&pair);
    init_master(&pair.master, 0x20);
    assert(lynx_pic_write_data(&pair.master, 0x00) == 0);
    init_master(&pair.slave, 0x28);
    assert(lynx_pic_write_data(&pair.slave, 0x00) == 0);
    assert(lynx_pic_pair_raise(&pair, 11) == 0);
    assert((pair.slave.irr & (1u << 3)) != 0);
    assert((pair.master.irr & (1u << LYNX_PIC_CASCADE_IRQ)) != 0);
    assert(lynx_pic_pair_raise(&pair, 16) == -1);
    return 0;
}
