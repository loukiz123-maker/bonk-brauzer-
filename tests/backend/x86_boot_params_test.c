/* Synthetic zero page only: no vCPU, kernel or Android execution. */
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "x86_boot_params.h"

static void le16(uint8_t *at, uint16_t n) { at[0] = n; at[1] = n >> 8; }
static void le32(uint8_t *at, uint32_t n) {
    at[0] = n; at[1] = n >> 8; at[2] = n >> 16; at[3] = n >> 24;
}
static uint32_t rd32(const uint8_t *at) {
    return (uint32_t)at[0] | ((uint32_t)at[1] << 8) | ((uint32_t)at[2] << 16) |
           ((uint32_t)at[3] << 24);
}

static uint8_t *make_image(size_t *size) {
    const size_t setup = 5 * 512, code = 64;
    *size = setup + code;
    uint8_t *image = calloc(1, *size);
    if (!image) return NULL;
    le32(image + 0x1f4, 4);
    le16(image + 0x1fe, 0xaa55);
    memcpy(image + 0x202, "HdrS", 4);
    le16(image + 0x206, 0x020c);
    image[0x201] = 0x60;
    image[0x211] = 1;
    le16(image + 0x236, 1);
    le32(image + 0x230, 0x1000);
    image[0x234] = 1;
    le32(image + 0x238, 256);
    le32(image + 0x260, 65536);
    return image;
}

int main(void) {
    size_t image_size = 0;
    uint8_t *image = make_image(&image_size);
    uint8_t *backing = calloc(1, 4 * 1024 * 1024);
    assert(image && backing);
    struct lynx_guest_memory ram = {backing, 4 * 1024 * 1024};
    struct lynx_boot_setup setup = {0};
    uint8_t zero_page[LYNX_BOOT_PARAMS_SIZE];
    assert(lynx_linux_prepare(&ram, image, image_size, "console=ttyS0", &setup) == 0);
    assert(setup.entry_address == 0x100200u && setup.boot_params_addr == 0x7000u);
    assert(lynx_guest_read(&ram, LYNX_BOOT_PARAMS_ADDRESS, zero_page, sizeof(zero_page)) == 0);
    assert(zero_page[0x1fe] == 0x55 && zero_page[0x1ff] == 0xaa);
    assert(zero_page[0x202] == 'H' && zero_page[0x210] == 0xff);
    assert((zero_page[0x211] & 0x80) != 0 && (zero_page[0x211] & 1) != 0);
    assert(rd32(zero_page + 0x228) == LYNX_CMDLINE_ADDRESS);
    assert(memcmp(backing + LYNX_CMDLINE_ADDRESS, "console=ttyS0", 14) == 0);
    assert(backing[LYNX_CMDLINE_ADDRESS + 13] == '\0');
    assert(zero_page[0x1e8] == 2);
    assert(rd32(zero_page + 0x2d0 + 16) == 1);
    assert(lynx_linux_prepare(&ram, image, image_size, NULL, &setup) == -1);
    assert(lynx_linux_prepare(&ram, image, image_size, "", &setup) == 0);
    char too_long[300];
    memset(too_long, 'a', sizeof(too_long) - 1);
    too_long[sizeof(too_long) - 1] = '\0';
    assert(lynx_linux_prepare(&ram, image, image_size, too_long, &setup) == -1);
    memset(too_long, 'a', 255);
    too_long[255] = '\0';
    assert(lynx_linux_prepare(&ram, image, image_size, too_long, &setup) == 0);
    le32(image + 0x238, 0);
    memset(too_long, 'a', 255);
    too_long[255] = '\0';
    assert(lynx_linux_prepare(&ram, image, image_size, too_long, &setup) == -1);
    struct lynx_guest_memory small = {backing, 0x90000u};
    le32(image + 0x238, 256);
    assert(lynx_linux_prepare(&small, image, image_size, "quiet", &setup) == -1);
    struct lynx_guest_memory tiny = {backing, 0x100000u};
    assert(lynx_linux_prepare(&tiny, image, image_size, "quiet", &setup) == -1);
    assert(lynx_build_e820(zero_page, 0x100000u) == -1);
    assert(lynx_linux_prepare(NULL, image, image_size, "quiet", &setup) == -1);
    free(image);
    free(backing);
    return 0;
}
