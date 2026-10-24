#include "legacy_movie_pool.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <limits>

namespace games::gitadora::legacy::movie {
namespace {

void *reserve_pages(std::size_t size) noexcept {
  return VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS);
}
bool commit_pages(void *base, std::size_t size) noexcept {
  return VirtualAlloc(base, size, MEM_COMMIT, PAGE_READWRITE) == base;
}
bool decommit_pages(void *base, std::size_t size) noexcept {
  return VirtualFree(base, size, MEM_DECOMMIT) != FALSE;
}
bool release_pages(void *base) noexcept {
  return VirtualFree(base, 0, MEM_RELEASE) != FALSE;
}

std::size_t page_round(std::size_t bytes, std::size_t page) noexcept {
  if (page == 0 || bytes > (std::numeric_limits<std::size_t>::max)() - page + 1)
    return 0;
  return ((bytes + page - 1) / page) * page;
}

bool in_slot(const void *ptr, const void *base, std::size_t capacity) noexcept {
  if (!ptr || !base)
    return false;
  const auto address = reinterpret_cast<std::uintptr_t>(ptr);
  const auto begin = reinterpret_cast<std::uintptr_t>(base);
  return address >= begin && address - begin < capacity;
}

} // namespace

BufferPool::BufferPool() noexcept
    : operations_{reserve_pages, commit_pages, decommit_pages, release_pages} {}

BufferPool::BufferPool(MemoryOperations operations) noexcept
    : operations_(operations) {}

bool BufferPool::reserve() noexcept {
  try {
    std::lock_guard guard(mutex_);
    if (ready_)
      return true;
    if (!operations_.reserve || !operations_.commit || !operations_.decommit ||
        !operations_.release)
      return false;

    // Clean up a prior partial failure before trying to reserve again.
    for (auto &slot : slots_) {
      if (slot.base && !operations_.release(slot.base))
        return false;
      slot = {};
    }

    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const auto page_size = static_cast<std::size_t>(info.dwPageSize);
    if (!page_size)
      return false;

    void *reserved[slot_count]{};
    for (std::size_t i = 0; i < slot_count; ++i) {
      reserved[i] = operations_.reserve(slot_capacity(i));
      if (!reserved[i]) {
        for (std::size_t j = 0; j < slot_count; ++j) {
          if (!reserved[j])
            continue;
          if (!operations_.release(reserved[j]))
            slots_[j] = {reserved[j], slot_capacity(j)};
        }
        return false;
      }
    }
    for (std::size_t i = 0; i < slot_count; ++i)
      slots_[i] = {reserved[i], slot_capacity(i)};
    page_size_ = page_size;
    ready_ = true;
    return true;
  } catch (...) {
    return false;
  }
}

void *BufferPool::allocate(std::size_t bytes) noexcept {
  return allocate_from(bytes, 0, slot_count);
}

void *BufferPool::allocate_from(std::size_t bytes, std::size_t first_slot,
                                std::size_t end_slot) noexcept {
  if (bytes == 0 || bytes > slot_size)
    return nullptr;
  try {
    std::lock_guard guard(mutex_);
    if (!ready_)
      return nullptr;
    const auto committed = page_round(bytes, page_size_);
    if (!committed || committed > slot_size)
      return nullptr;
    Allocation *record = nullptr;
    for (auto &allocation : allocations_) {
      if (!allocation.base) {
        record = &allocation;
        break;
      }
    }
    if (!record)
      return nullptr;
    for (std::size_t slot_index = first_slot; slot_index < end_slot;
         ++slot_index) {
      auto &slot = slots_[slot_index];
      if (!slot.base || committed > slot.capacity)
        continue;
      std::size_t offset = 0;
      while (offset <= slot.capacity - committed) {
        auto *candidate = static_cast<unsigned char *>(slot.base) + offset;
        const auto start = reinterpret_cast<std::uintptr_t>(candidate);
        std::size_t next = offset;
        for (const auto &allocation : allocations_) {
          if (!in_slot(allocation.base, slot.base, slot.capacity))
            continue;
          const auto begin = reinterpret_cast<std::uintptr_t>(allocation.base);
          if (start < begin + allocation.committed &&
              begin < start + committed) {
            const auto end = begin -
                             reinterpret_cast<std::uintptr_t>(slot.base) +
                             allocation.committed;
            if (end > next)
              next = end;
          }
        }
        if (next != offset) {
          offset = next;
          continue;
        }
        if (operations_.commit(candidate, committed)) {
          *record = {candidate, committed};
          return candidate;
        }
        // A commit failure need not be local to one page; don't scan
        // hundreds of MiB of empty address space trying every page.
        break;
      }
    }
  } catch (...) {
  }
  return nullptr;
}

bool BufferPool::release(void *ptr) noexcept {
  if (!ptr)
    return false;
  try {
    std::lock_guard guard(mutex_);
    for (auto &slot : slots_) {
      if (!in_slot(ptr, slot.base, slot.capacity))
        continue;
      for (auto &allocation : allocations_) {
        if (ptr == allocation.base && allocation.committed) {
          if (operations_.decommit(allocation.base, allocation.committed))
            allocation = {};
          break;
        }
      }
      return true;
    }
  } catch (...) {
  }
  return false;
}

bool BufferPool::contains(const void *ptr) const noexcept {
  if (!ptr)
    return false;
  try {
    std::lock_guard guard(mutex_);
    for (const auto &slot : slots_)
      if (in_slot(ptr, slot.base, slot.capacity))
        return true;
  } catch (...) {
  }
  return false;
}

bool BufferPool::reserved() const noexcept {
  try {
    std::lock_guard guard(mutex_);
    return ready_;
  } catch (...) {
    return false;
  }
}

void *BufferPool::reserved_base(std::size_t slot) const noexcept {
  if (slot >= slot_count)
    return nullptr;
  try {
    std::lock_guard guard(mutex_);
    return slots_[slot].base;
  } catch (...) {
    return nullptr;
  }
}

BufferPool::~BufferPool() {
  std::lock_guard guard(mutex_);
  for (auto &slot : slots_) {
    if (!slot.base)
      continue;
    operations_.release(slot.base);
    slot = {};
  }
}

} // namespace games::gitadora::legacy::movie
