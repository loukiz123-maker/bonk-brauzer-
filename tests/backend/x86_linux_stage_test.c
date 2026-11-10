/* Synthetic byte buffers only; this test does not boot Linux or Android. */
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "x86_linux_stage.h"

static void le16(uint8_t *at, uint16_t n) { at[0] = n; at[1] = n >> 8; }
static void le32(uint8_t *at, uint32_t n) {
    at[0] = n; at[1] = n >> 8; at[2] = n >> 16; at[3] = n >> 24;
}

int main(void) {
    const size_t setup = 5 * 512, code = 64, image_size = setup + code;
    uint8_t *image = calloc(1, image_size);
    uint8_t *backing = calloc(1, 2 * 1024 * 1024);
    assert(image && backing);
    struct lynx_guest_memory ram = {backing, 2 * 1024 * 1024};
    struct lynx_linux_image info = {0};
    le32(image + 0x1f4, 4); /* protected code in 16-byte units */
    le16(image + 0x1fe, 0xaa55);
    memcpy(image + 0x202, "HdrS", 4);
    le16(image + 0x206, 0x020c);
    image[0x211] = 1;
    le16(image + 0x236, 1);
    le32(image + 0x238, 2048);
    le32(image + 0x230, 0x1000);
    image[0x234] = 1; /* relocatable */
    le32(image + 0x260, 65536);
    memset(image + setup, 0xa5, code);
    assert(lynx_linux_inspect(image, image_size, &info) == 0);
    assert(info.setup_bytes == setup && info.kernel_bytes == code && info.protocol == 0x20c);
    assert(info.init_size == 65536 && info.cmdline_max == 2048);
    assert(lynx_linux_stage(&ram, image, image_size, &info) == 0);
    assert(memcmp(backing + LYNX_LINUX_SETUP_ADDRESS, image, setup) == 0);
    assert(memcmp(backing + LYNX_LINUX_KERNEL_ADDRESS, image + setup, code) == 0);
    memset(backing, 0, ram.size);
    le32(image + 0x260, UINT32_MAX); /* not enough guest RAM */
    assert(lynx_linux_stage(&ram, image, image_size, &info) == -1);
    assert(backing[LYNX_LINUX_SETUP_ADDRESS] == 0 && backing[LYNX_LINUX_KERNEL_ADDRESS] == 0);
    le32(image + 0x260, 65536);
    le32(image + 0x258, 0x1f8000); /* runtime extends beyond RAM */
    assert(lynx_linux_stage(&ram, image, image_size, &info) == -1);
    assert(backing[LYNX_LINUX_SETUP_ADDRESS] == 0);
    le32(image + 0x258, 0);
    le32(image + 0x230, 3); /* invalid alignment */
    assert(lynx_linux_stage(&ram, image, image_size, &info) == -1);
    le32(image + 0x230, 0x1000);
    assert(lynx_linux_stage(&ram, image, image_size - 1, &info) == -1);
    assert(backing[LYNX_LINUX_SETUP_ADDRESS] == 0);
    assert(lynx_linux_stage(&ram, backing, ram.size, &info) == -1);
    assert(lynx_linux_inspect(image, 10, &info) == -1);
    le16(image + 0x206, 0x020b);
    assert(lynx_linux_inspect(image, image_size, &info) == -1);
    le16(image + 0x206, 0x020c);
    le16(image + 0x236, 0);
    assert(lynx_linux_inspect(image, image_size, &info) == -1);
    le16(image + 0x236, 1);
    image[0x211] = 0;
    assert(lynx_linux_inspect(image, image_size, &info) == -1);
    image[0x211] = 1;
    image[0x1f1] = 255;
    assert(lynx_linux_inspect(image, image_size, &info) == -1);
    image[0x1f1] = 0;
    le32(image + 0x1f4, UINT32_MAX);
    assert(lynx_linux_inspect(image, image_size, &info) == -1);
    free(image);
    free(backing);
    return 0;
}
