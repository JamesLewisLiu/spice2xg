#pragma once

#include <array>
#include <cstddef>
#include <mutex>
#include <windows.h>

namespace games::gitadora::legacy::movie {

// Whole-file video buffers only. Allocate on demand, outside the legacy CRT
// heaps, and return the complete address range on release. No startup arena.
// MEM_TOP_DOWN keeps large buffers away from the low-address heaps on LAA hosts.
class VideoMemory {
public:
  static constexpr size_t minimum_size = 1024u * 1024u;
  static constexpr size_t capacity = 64;
  enum class Release { foreign, released, failed };
  struct Operations {
    void *(*allocate)(size_t) noexcept;
    bool (*release)(void *) noexcept;
  };

  VideoMemory() noexcept : operations_{allocate_pages, release_pages} {}
  explicit VideoMemory(Operations operations) noexcept : operations_(operations) {}

  void *allocate(size_t bytes) {
    if (bytes < minimum_size || bytes > 0xffffffe0u)
      return nullptr;
    std::lock_guard guard(mutex_);
    for (auto &entry : entries_) {
      if (!entry) {
        entry = operations_.allocate(bytes);
        return entry;
      }
    }
    return nullptr; // Caller retains the original allocator fallback.
  }

  Release release(void *buffer) {
    if (!buffer)
      return Release::foreign;
    std::lock_guard guard(mutex_);
    for (auto &entry : entries_) {
      if (entry == buffer) {
        // Keep ownership on OS failure: never pass VirtualAlloc memory to free.
        if (!operations_.release(buffer))
          return Release::failed;
        entry = nullptr;
        return Release::released;
      }
    }
    return Release::foreign;
  }

private:
  static void *allocate_pages(size_t bytes) noexcept {
    return VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN,
                        PAGE_READWRITE);
  }
  static bool release_pages(void *buffer) noexcept {
    return VirtualFree(buffer, 0, MEM_RELEASE) != FALSE;
  }
  // Live startup caches follow the original host's process lifetime. Do not
  // release them from a static destructor while native decoder threads exist.
  Operations operations_;
  std::array<void *, capacity> entries_{};
  std::mutex mutex_;
};

} // namespace games::gitadora::legacy::movie
