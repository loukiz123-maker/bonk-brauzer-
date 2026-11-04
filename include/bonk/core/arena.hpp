// Bonk Brauzer — bump ("arena") allocator.
//
// Why an arena and not one heap allocation per node: a DOM tree, its computed
// styles and its layout boxes all die together when the document is destroyed
// or re-laid out. A bump allocator turns that into a single `reset()` — no
// per-node free lists, no fragmentation, no refcounting, and it makes
// allocation a pointer increment (~5 ns instead of ~50 ns for malloc).
//
// Rules for payloads: trivially destructible, or destroyed explicitly by the
// owner before reset(). `make<T>` with a non-trivial T compiles, but the
// destructor will *not* run — use it for POD-ish nodes only.
#pragma once

#include <cstddef>
#include <new>
#include <utility>

namespace bonk {

class Arena {
 public:
  explicit Arena(std::size_t block_size = 64u * 1024u) noexcept;
  ~Arena();

  Arena(Arena&& other) noexcept;
  Arena& operator=(Arena&& other) noexcept;
  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;

  /// Returns `size` bytes aligned to `alignment` (must be a power of two).
  /// Never returns nullptr; the process aborts on allocation failure by design
  /// (a browser tab that cannot allocate 64 KiB cannot continue meaningfully).
  [[nodiscard]] void* allocate(std::size_t size, std::size_t alignment = alignof(std::max_align_t));

  /// Placement-construct a T inside the arena.
  template <class T, class... Args>
  [[nodiscard]] T* make(Args&&... args) {
    void* memory = allocate(sizeof(T), alignof(T));
    return ::new (memory) T(std::forward<Args>(args)...);
  }

  [[nodiscard]] std::size_t bytes_used() const noexcept { return used_; }
  [[nodiscard]] std::size_t blocks() const noexcept;
  [[nodiscard]] std::size_t block_size() const noexcept { return block_size_; }

  /// Releases every block except the first one and rewinds the bump pointer.
  void reset() noexcept;

 private:
  struct Block;

  /// Allocates one block; aborts on OOM (see allocate()).
  [[nodiscard]] static Block* new_block(std::size_t payload_size, Block* previous);

  Block* first_ = nullptr;
  Block* current_ = nullptr;
  std::size_t block_size_ = 0;
  std::size_t used_ = 0;  // payload bytes handed out since the last reset()
};

}  // namespace bonk
