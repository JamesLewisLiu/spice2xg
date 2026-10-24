#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "games/gitadora/legacy_movie_pool.h"

using games::gitadora::legacy::movie::BufferPool;

namespace {
void require(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

MEMORY_BASIC_INFORMATION query(const void *address) {
  MEMORY_BASIC_INFORMATION info{};
  require(VirtualQuery(address, &info, sizeof(info)) == sizeof(info),
          "VirtualQuery succeeds");
  return info;
}

unsigned reserve_calls = 0;
unsigned fail_reserve_call = 0;
bool fail_commit = false;
bool fail_decommit = false;
void *last_reserved = nullptr;
void *last_released = nullptr;

void *test_reserve(std::size_t bytes) noexcept {
  if (++reserve_calls == fail_reserve_call)
    return nullptr;
  last_reserved = VirtualAlloc(nullptr, bytes, MEM_RESERVE, PAGE_NOACCESS);
  return last_reserved;
}
bool test_commit(void *base, std::size_t bytes) noexcept {
  return !fail_commit &&
         VirtualAlloc(base, bytes, MEM_COMMIT, PAGE_READWRITE) == base;
}
bool test_decommit(void *base, std::size_t bytes) noexcept {
  return !fail_decommit && VirtualFree(base, bytes, MEM_DECOMMIT) != FALSE;
}
bool test_release(void *base) noexcept {
  last_released = base;
  return VirtualFree(base, 0, MEM_RELEASE) != FALSE;
}

void failure_cases() {
  const BufferPool::MemoryOperations operations{test_reserve, test_commit,
                                                test_decommit, test_release};
  void *bases[BufferPool::slot_count]{};
  {
    BufferPool pool(operations);
    require(!pool.reserved() && !pool.allocate(1),
            "unreserved pool cannot allocate");
    fail_reserve_call = 2;
    require(!pool.reserve() && !pool.reserved(),
            "second reservation failure leaves pool unavailable");
    require(last_released && last_released == last_reserved &&
                query(last_released).State == MEM_FREE,
            "first reservation released when second fails");
    require(!pool.allocate(1) && !pool.contains(last_released),
            "failed pool never exposes partial storage");

    fail_reserve_call = 0;
    require(pool.reserve(), "reservation retries successfully after failure");
    for (std::size_t i = 0; i < BufferPool::slot_count; ++i)
      bases[i] = pool.reserved_base(i);
    fail_commit = true;
    require(!pool.allocate(4096), "commit failure returns allocation failure");
    require(query(bases[0]).State == MEM_RESERVE &&
                query(bases[1]).State == MEM_RESERVE,
            "commit failure does not consume any slot");
    fail_commit = false;
    void *allocation = pool.allocate(1);
    require(allocation == bases[0], "commit retry reuses available slot");
    fail_decommit = true;
    require(pool.release(allocation),
            "decommit failure still protects pointer from CRT");
    require(query(allocation).State == MEM_COMMIT,
            "failed decommit does not mark allocation reusable");
    auto *second = pool.allocate(1);
    require(second != allocation && second != nullptr &&
                query(second).State == MEM_COMMIT,
            "failed decommit cannot overlap another live allocation");
    fail_decommit = false;
    require(pool.release(allocation), "decommit can be retried");
    // The second live slice is deliberately left for RAII teardown.
  }
  for (void *base : bases)
    require(query(base).State == MEM_FREE,
            "destructor frees reservations including live allocation");
  BufferPool invalid(BufferPool::MemoryOperations{});
  require(!invalid.reserve(), "invalid operations fail safely");
}
} // namespace

int main() {
  failure_cases();
  BufferPool pool;
  require(pool.reserve(),
          "192 and 448 MiB reservations succeed (640 MiB total)");
  require(pool.reserved(), "pool reports fully reserved");
  for (std::size_t i = 0; i < BufferPool::slot_count; ++i) {
    const auto info = query(pool.reserved_base(i));
    require(info.State == MEM_RESERVE &&
                info.AllocationProtect == PAGE_NOACCESS &&
                info.RegionSize == BufferPool::slot_capacity(i),
            "slots begin reserved and inaccessible");
  }
  require(pool.reserved_base(BufferPool::slot_count) == nullptr,
          "invalid slot index rejected");
  require(pool.allocate(BufferPool::slot_size + 1) == nullptr,
          "oversize request rejected");
  require(pool.allocate(0) == nullptr, "zero request rejected");
  require(pool.allocate((std::numeric_limits<std::size_t>::max)()) == nullptr,
          "overflow-size request rejected");

  auto *first = static_cast<unsigned char *>(pool.allocate(1));
  SYSTEM_INFO system_info{};
  GetSystemInfo(&system_info);
  require(query(first).State == MEM_COMMIT &&
              query(first + system_info.dwPageSize).State == MEM_RESERVE,
          "one byte request commits exactly one page");
  auto *second = static_cast<unsigned char *>(pool.allocate(1));
  auto *third = static_cast<unsigned char *>(pool.allocate(1));
  require(first && second && third && first != second && second != third &&
              first != third,
          "three live slices are distinct");
  auto *fourth = static_cast<unsigned char *>(pool.allocate(1));
  require(fourth != nullptr,
          "a fourth allocation can use remaining arena space");
  require(pool.contains(first) && pool.contains(first + 1),
          "base and interior addresses are owned");
  require(pool.release(first + 1), "interior release is handled safely");
  require(query(first).State == MEM_COMMIT,
          "interior release does not decommit live allocation");
  first[0] = 0x5a;
  require(pool.reserve() && first[0] == 0x5a &&
              query(first).State == MEM_COMMIT,
          "idempotent reserve preserves live allocation");
  require(pool.release(first), "first release decommits exact base");
  require(pool.release(first), "duplicate release is safely handled");
  require(query(first).State == MEM_RESERVE,
          "released slot remains reserved and decommitted");
  require(pool.release(second) && pool.release(third) && pool.release(fourth),
          "all neighboring slices can be released independently");

  auto *full =
      static_cast<unsigned char *>(pool.allocate(BufferPool::slot_capacity(0)));
  require(full == first, "released reservation reused for full-slot request");
  require(full[0] == 0, "decommitted pages are zeroed on recommit");
  full[BufferPool::slot_capacity(0) - 1] = 0xa5;
  require(query(full + BufferPool::slot_capacity(0) - 1).State == MEM_COMMIT,
          "full slot last byte is committed and writable");
  require(pool.release(full), "full slot release succeeds");
  require(query(full).State == MEM_RESERVE, "full slot decommits");
  auto *large =
      static_cast<unsigned char *>(pool.allocate(BufferPool::slot_size));
  require(large == pool.reserved_base(1),
          "448 MiB allocation skips smaller first arena");
  large[BufferPool::slot_size - 1] = 0x7a;
  require(query(large + BufferPool::slot_size - 1).State == MEM_COMMIT,
          "larger arena last byte committed");
  require(pool.release(large), "larger arena decommits safely");

  require(!pool.contains(nullptr) && !pool.release(nullptr),
          "null pointer not owned");
  require(!pool.release(reinterpret_cast<void *>(0x1234)),
          "foreign pointer is rejected");

  std::atomic<unsigned> successes{0};
  std::mutex active_mutex;
  std::vector<std::pair<std::uintptr_t, std::size_t>> active;
  std::vector<std::thread> workers;
  for (unsigned i = 0; i < 8; ++i) {
    workers.emplace_back([&] {
      for (unsigned iteration = 0; iteration < 200; ++iteration) {
        void *ptr = nullptr;
        for (unsigned spin = 0; !ptr && spin < 1000000; ++spin) {
          ptr = pool.allocate(64 * 1024 + iteration);
          if (!ptr)
            std::this_thread::yield();
        }
        require(ptr != nullptr, "concurrent allocation eventually succeeds");
        const auto begin = reinterpret_cast<std::uintptr_t>(ptr);
        const std::size_t bytes = 64 * 1024 + iteration;
        {
          std::lock_guard guard(active_mutex);
          for (const auto &[other, other_bytes] : active)
            require(begin + bytes <= other || other + other_bytes <= begin,
                    "concurrent live allocations never overlap");
          active.emplace_back(begin, bytes);
        }
        ++successes;
        require(pool.contains(ptr), "worker allocation remains owned");
        static_cast<volatile unsigned char *>(ptr)[0] = 0x3c;
        {
          std::lock_guard guard(active_mutex);
          for (auto it = active.begin(); it != active.end(); ++it) {
            if (it->first == begin) {
              active.erase(it);
              break;
            }
          }
        }
        require(pool.release(ptr), "worker release succeeds");
      }
    });
  }
  for (auto &worker : workers)
    worker.join();
  require(successes == 8 * 200, "all concurrent workers completed");
  std::puts("PASS 192 + 448 MiB reservation, page commit/decommit, "
            "ownership guards, boundaries, reuse, and concurrent stress");
}
