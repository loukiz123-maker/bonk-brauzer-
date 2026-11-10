/* Synthetic timer arithmetic only; no VM and no interrupt delivery. */
#include <assert.h>
#include <stdint.h>
#include "i8254.h"

int main(void) {
    struct lynx_pit pit;
    lynx_pit_reset(&pit);
    assert(lynx_pit_advance(&pit, 1000000000ull) == 0); /* not programmed */

    assert(lynx_pit_write_control(&pit, 0x36) == 0); /* ch0, LSB+MSB, mode 3 */
    assert(pit.mode == 3 && pit.access == 3);
    assert(lynx_pit_write_data(&pit, 0, 0x9c) == 0 && pit.waiting_high == 1);
    assert(lynx_pit_write_data(&pit, 0, 0x2e) == 0 && pit.waiting_high == 0);
    assert(pit.divisor == 0x2e9c);
    /* 1193182 / 0x2e9c ticks per second. */
    uint64_t ticks = lynx_pit_advance(&pit, 1000000000ull);
    assert(ticks == LYNX_PIT_BASE_FREQUENCY / 0x2e9c);
    assert(pit.cycles < pit.divisor);

    /* Many small advances must not drift relative to one large advance. */
    struct lynx_pit fine;
    lynx_pit_reset(&fine);
    assert(lynx_pit_write_control(&fine, 0x34) == 0); /* ch0, LSB+MSB, mode 2 */
    assert(lynx_pit_write_data(&fine, 0, 0x00) == 0);
    assert(lynx_pit_write_data(&fine, 0, 0x10) == 0);
    assert(fine.divisor == 0x1000);
    uint64_t total = 0;
    for (int i = 0; i < 1000; ++i) total += lynx_pit_advance(&fine, 1000000ull);
    assert(total == 291); /* 1193182 cycles per second over 4096-cycle periods */
    assert(fine.ticks == total);
    struct lynx_pit single;
    lynx_pit_reset(&single);
    assert(lynx_pit_write_control(&single, 0x34) == 0);
    assert(lynx_pit_write_data(&single, 0, 0x00) == 0);
    assert(lynx_pit_write_data(&single, 0, 0x10) == 0);
    assert(lynx_pit_advance(&single, 1000000000ull) == total); /* no drift */

    /* A one-millisecond advance below one period yields no tick. */
    struct lynx_pit slow;
    lynx_pit_reset(&slow);
    assert(lynx_pit_write_control(&slow, 0x36) == 0);
    assert(lynx_pit_write_data(&slow, 0, 0xff) == 0);
    assert(lynx_pit_write_data(&slow, 0, 0xff) == 0);
    assert(slow.divisor == 0xffff);
    assert(lynx_pit_advance(&slow, 1000000ull) == 0);

    /* Zero divisor means 65536; LSB-only and MSB-only access modes work. */
    struct lynx_pit lsb;
    lynx_pit_reset(&lsb);
    assert(lynx_pit_write_control(&lsb, 0x16) == 0); /* LSB only, mode 3 */
    assert(lynx_pit_write_data(&lsb, 0, 0) == 0);
    assert(lsb.divisor == LYNX_PIT_MAX_DIVISOR);
    struct lynx_pit msb;
    lynx_pit_reset(&msb);
    assert(lynx_pit_write_control(&msb, 0x26) == 0); /* MSB only, mode 3 */
    assert(lynx_pit_write_data(&msb, 0, 0x02) == 0);
    assert(msb.divisor == 0x200);

    /* Unsupported encodings are rejected without changing the divisor. */
    assert(lynx_pit_write_control(&pit, 0xc0) == -1); /* read-back */
    assert(lynx_pit_write_control(&pit, 0x00) == -1); /* latch */
    assert(lynx_pit_write_control(&pit, 0x3e) == -1); /* mode 7 */
    assert(lynx_pit_write_control(&pit, 0x37) == -1); /* BCD */
    assert(pit.divisor == 0x2e9c);
    assert(lynx_pit_write_data(&pit, 0, 1) == 0); /* channel 0 model only */
    assert(lynx_pit_write_data(&pit, 3, 1) == -1);
    assert(lynx_pit_write_data(&pit, 1, 1) == 0);
    struct lynx_pit unprogrammed;
    lynx_pit_reset(&unprogrammed);
    assert(lynx_pit_write_data(&unprogrammed, 0, 5) == -1);
    return 0;
}
