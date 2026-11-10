/* Synthetic RAM only: no VM is created and no guest code is executed. */
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "guest_memory.h"

int main(void) {
    uint8_t buffer[16] = {0};
    uint8_t snapshot[16], result[4];
    const uint8_t data[] = {1, 2, 3, 4};
    struct lynx_guest_memory ram = {buffer, sizeof(buffer)};
    assert(lynx_guest_write(&ram, 12, data, sizeof(data)) == 0);
    assert(lynx_guest_read(&ram, 12, result, sizeof(result)) == 0);
    assert(memcmp(data, result, sizeof(data)) == 0);
    assert(lynx_guest_write(&ram, 16, NULL, 0) == 0);
    assert(lynx_guest_read(&ram, 16, NULL, 0) == 0);
    memcpy(snapshot, buffer, sizeof(buffer));
    assert(lynx_guest_write(&ram, 13, data, sizeof(data)) == -1);
    assert(lynx_guest_write(&ram, UINT64_MAX, data, sizeof(data)) == -1);
    assert(lynx_guest_write(&ram, 0, data, SIZE_MAX) == -1);
    assert(lynx_guest_write(&ram, 0, NULL, 1) == -1);
    assert(lynx_guest_write(&ram, 17, NULL, 0) == -1);
    assert(lynx_guest_read(&ram, 15, result, sizeof(result)) == -1);
    assert(lynx_guest_read(&ram, 0, NULL, 1) == -1);
    assert(memcmp(snapshot, buffer, sizeof(buffer)) == 0);
    assert(lynx_guest_write(&ram, 0, data, sizeof(data)) == 0);
    assert(lynx_guest_write(&ram, 1, buffer, 4) == 0); /* overlap is safe */
    assert(memcmp(buffer + 1, data, sizeof(data)) == 0);
    assert(lynx_guest_read(NULL, 0, result, 1) == -1);
    ram.host = NULL;
    assert(lynx_guest_write(&ram, 0, data, 1) == -1);
    return 0;
}
