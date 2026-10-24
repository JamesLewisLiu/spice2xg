#pragma once

#include <cstddef>
#include <mutex>

namespace games::gitadora::legacy::movie {

// Share page-aligned slices between game.dll's three persistent startup movies
// and libmovie's transient sources. A reservation is not one allocation.
class BufferPool {
public:
  static constexpr std::size_t slot_size = 448u * 1024u * 1024u;
  static constexpr std::size_t slot_count = 2;
  // Preserve the startup arena; enlarge only the concurrent playback arena.
  static constexpr std::size_t slot_capacity(std::size_t index) noexcept {
    return index == 0 ? 192u * 1024u * 1024u : index == 1 ? slot_size : 0;
  }
  static constexpr std::size_t allocation_count = 16;

  // A narrow OS seam permits deterministic reserve/commit failure tests.
  // Custom functions must implement the same ownership contract and must not
  // reenter the pool. Production uses only the default Windows operations.
  struct MemoryOperations {
    void *(*reserve)(std::size_t) noexcept;
    bool (*commit)(void *, std::size_t) noexcept;
    bool (*decommit)(void *, std::size_t) noexcept;
    bool (*release)(void *) noexcept;
  };
  BufferPool() noexcept;
  explicit BufferPool(MemoryOperations operations) noexcept;
  BufferPool(const BufferPool &) = delete;
  BufferPool &operator=(const BufferPool &) = delete;
  ~BufferPool();

  // Reserves all slots as one transaction. Failure leaves it unavailable,
  // allowing fallback; any failed rollback is retained for a cleanup retry.
  bool reserve() noexcept;

  // First-fit allocation within either reservation; commits only whole pages
  // exclusively owned by this slice. Returns nullptr on exhaustion/failure.
  void *allocate(std::size_t bytes) noexcept;

  // Returns true for every address belonging to this pool, including an
  // interior or duplicate release. Only an exact live slice decommits memory;
  // this prevents an invalid pointer from reaching the CRT allocator.
  // As with native allocators, a stale pointer cannot be distinguished after
  // its base has been reused for another allocation.
  bool release(void *ptr) noexcept;

  bool contains(const void *ptr) const noexcept;
  bool reserved() const noexcept;
  void *reserved_base(std::size_t slot) const noexcept;

private:
  void *allocate_from(std::size_t bytes, std::size_t first_slot,
                      std::size_t end_slot) noexcept;

  struct Slot {
    void *base = nullptr;
    std::size_t capacity = 0;
  };
  struct Allocation {
    void *base = nullptr;
    std::size_t committed = 0;
  };

  mutable std::mutex mutex_;
  MemoryOperations operations_;
  std::size_t page_size_ = 0;
  bool ready_ = false;
  Slot slots_[slot_count]{};
  Allocation allocations_[allocation_count]{};
};

} // namespace games::gitadora::legacy::movie
