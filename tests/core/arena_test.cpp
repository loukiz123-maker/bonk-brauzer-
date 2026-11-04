#include <cstdint>
#include <set>

#include "test_framework.hpp"

#include "bonk/core/arena.hpp"

using namespace bonk;

namespace {
bool is_aligned(const void* pointer, std::size_t alignment) {
  return reinterpret_cast<std::uintptr_t>(pointer) % alignment == 0;
}
}  // namespace

BONK_TEST(arena, minimal_block_size_is_enforced) {
  Arena arena(1);
  CHECK(arena.block_size() >= 256);
  CHECK_EQ(arena.bytes_used(), std::size_t{0});
  CHECK_EQ(arena.blocks(), std::size_t{1});
}

BONK_TEST(arena, allocations_are_aligned) {
  Arena arena(1024);
  for (std::size_t alignment = 1; alignment <= 128; alignment *= 2) {
    for (std::size_t size : {std::size_t{1}, std::size_t{3}, std::size_t{8}, std::size_t{63}, std::size_t{512}}) {
      void* pointer = arena.allocate(size, alignment);
      CHECK(is_aligned(pointer, alignment));
    }
  }
}

BONK_TEST(arena, allocations_do_not_overlap) {
  Arena arena(4096);
  std::set<std::uintptr_t> blocks;
  std::uintptr_t previous_end = 0;
  for (int i = 0; i < 200; ++i) {
    void* pointer = arena.allocate(8, 8);
    const auto start = reinterpret_cast<std::uintptr_t>(pointer);
    CHECK(blocks.insert(start).second);  // never hands out the same address twice
    CHECK(start >= previous_end);
    previous_end = start + 8;
  }
  CHECK_EQ(arena.blocks(), std::size_t{1});  // 200 * 8 bytes fit in one 4 KiB block
  CHECK_EQ(arena.bytes_used(), std::size_t{1600});
}

BONK_TEST(arena, grows_with_new_blocks) {
  Arena arena(512);
  for (int i = 0; i < 100; ++i) (void)arena.allocate(64, 8);
  CHECK(arena.blocks() > 1);
}

BONK_TEST(arena, oversized_allocation_gets_its_own_block) {
  Arena arena(512);
  void* big = arena.allocate(1 << 20, 64);
  CHECK(is_aligned(big, 64));
  CHECK(arena.blocks() >= 2);
  CHECK(arena.bytes_used() >= (1u << 20));
}

BONK_TEST(arena, reset_rewinds_and_keeps_first_block) {
  Arena arena(512);
  (void)arena.allocate(256, 8);
  (void)arena.allocate(2048, 8);
  const std::size_t blocks_before = arena.blocks();
  REQUIRE(blocks_before >= 2);

  arena.reset();
  CHECK_EQ(arena.blocks(), std::size_t{1});
  CHECK_EQ(arena.bytes_used(), std::size_t{0});

  // Usable after reset.
  void* after = arena.allocate(128, 32);
  CHECK(is_aligned(after, 32));
  CHECK_EQ(arena.bytes_used(), std::size_t{128});
}

BONK_TEST(arena, make_constructs_objects) {
  struct Pixel {
    int x;
    int y;
    int sum() const { return x + y; }
  };

  Arena arena(1024);
  Pixel* pixel = arena.make<Pixel>(3, 4);
  CHECK_EQ(pixel->sum(), 7);

  int* zeroed = arena.make<int>(0);
  CHECK_EQ(*zeroed, 0);
}

BONK_TEST(arena, move_transfers_ownership) {
  Arena source(1024);
  (void)source.allocate(64, 8);
  const std::size_t used = source.bytes_used();
  const std::size_t blocks = source.blocks();

  Arena moved(std::move(source));
  CHECK_EQ(moved.bytes_used(), used);
  CHECK_EQ(moved.blocks(), blocks);
  CHECK_EQ(source.blocks(), std::size_t{0});  // moved-from arena is inert

  Arena assigned(512);
  assigned = std::move(moved);
  CHECK_EQ(assigned.bytes_used(), used);
}

BONK_TEST(arena, many_small_allocations_stay_fast_and_dense) {
  // 1 MiB of 16-byte objects out of 64 KiB blocks: 16 blocks, no waste.
  Arena arena(64 * 1024);
  for (int i = 0; i < 65536; ++i) (void)arena.allocate(16, 16);
  CHECK_EQ(arena.blocks(), std::size_t{16});
}
