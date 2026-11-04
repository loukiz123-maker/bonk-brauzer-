#include "bonk/core/arena.hpp"

#include <cstdint>
#include <cstdlib>

namespace bonk {

// Blocks are linked backwards through `previous`; the bump pointer walks
// forward inside the payload. The header is allocated together with the payload
// in one malloc call, so a block is exactly one allocation and one free.
//
// `alignas(16)` makes sizeof(Block) a multiple of 16, so the payload starts on
// the same alignment malloc gives us and ordinary allocations need no padding —
// a 64 KiB block then holds exactly 4096 objects of 16 bytes.
struct alignas(16) Arena::Block {
  Block* previous = nullptr;
  std::size_t capacity = 0;  // payload bytes
  std::size_t used = 0;      // payload bytes consumed

  [[nodiscard]] unsigned char* payload() noexcept {
    return reinterpret_cast<unsigned char*>(this) + sizeof(Block);
  }
};

namespace {
constexpr std::size_t kMinBlockSize = 256;
}  // namespace

Arena::Block* Arena::new_block(std::size_t payload_size, Block* previous) {
  void* memory = std::malloc(sizeof(Block) + payload_size);
  if (memory == nullptr) {
    // Cannot get 64 KiB for a document: there is nothing sane left to do.
    std::abort();
  }
  auto* block = static_cast<Block*>(memory);
  block->previous = previous;
  block->capacity = payload_size;
  block->used = 0;
  return block;
}

Arena::Arena(std::size_t block_size) noexcept : block_size_(block_size < kMinBlockSize ? kMinBlockSize : block_size) {
  first_ = new_block(block_size_, nullptr);
  current_ = first_;
}

Arena::~Arena() {
  Block* block = current_;
  while (block != nullptr) {
    Block* previous = block->previous;
    std::free(block);
    block = previous;
  }
}

Arena::Arena(Arena&& other) noexcept
    : first_(other.first_), current_(other.current_), block_size_(other.block_size_), used_(other.used_) {
  other.first_ = nullptr;
  other.current_ = nullptr;
  other.block_size_ = 0;
  other.used_ = 0;
}

Arena& Arena::operator=(Arena&& other) noexcept {
  if (this == &other) return *this;
  Block* block = current_;
  while (block != nullptr) {
    Block* previous = block->previous;
    std::free(block);
    block = previous;
  }
  first_ = other.first_;
  current_ = other.current_;
  block_size_ = other.block_size_;
  used_ = other.used_;
  other.first_ = nullptr;
  other.current_ = nullptr;
  other.block_size_ = 0;
  other.used_ = 0;
  return *this;
}

void* Arena::allocate(std::size_t size, std::size_t alignment) {
  if (alignment == 0 || (alignment & (alignment - 1)) != 0) alignment = alignof(std::max_align_t);
  if (size == 0) size = 1;

  Block* block = current_;
  if (block != nullptr) {
    const auto base = reinterpret_cast<std::uintptr_t>(block->payload());
    const auto mask = static_cast<std::uintptr_t>(alignment) - 1;
    const std::uintptr_t aligned = (base + block->used + mask) & ~mask;
    const std::size_t offset = aligned - base;
    if (offset <= block->capacity && size <= block->capacity - offset) {
      block->used = offset + size;
      used_ += size;
      return reinterpret_cast<void*>(aligned);
    }
  }

  // Not enough room: a new block is at least `block_size_`, and is grown to fit
  // the request plus worst-case alignment padding when the request is larger.
  std::size_t payload = block_size_;
  const std::size_t needed = size + alignment;
  if (needed > payload) payload = needed;

  block = new_block(payload, current_);
  if (first_ == nullptr) first_ = block;
  current_ = block;

  const auto base = reinterpret_cast<std::uintptr_t>(block->payload());
  const auto mask = static_cast<std::uintptr_t>(alignment) - 1;
  const std::uintptr_t aligned = (base + mask) & ~mask;
  const std::size_t offset = aligned - base;
  block->used = offset + size;
  used_ += size;
  return reinterpret_cast<void*>(aligned);
}

std::size_t Arena::blocks() const noexcept {
  std::size_t count = 0;
  for (const Block* block = current_; block != nullptr; block = block->previous) ++count;
  return count;
}

void Arena::reset() noexcept {
  if (first_ == nullptr) {
    first_ = new_block(block_size_, nullptr);
    current_ = first_;
  }
  Block* block = current_;
  while (block != nullptr && block != first_) {
    Block* previous = block->previous;
    std::free(block);
    block = previous;
  }
  first_->previous = nullptr;
  first_->used = 0;
  current_ = first_;
  used_ = 0;
}

}  // namespace bonk
