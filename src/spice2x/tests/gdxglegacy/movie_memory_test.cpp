#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include "games/gitadora/legacy_movie_memory.h"

using games::gitadora::legacy::movie::VideoMemory;
void require(bool value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}
MEMORY_BASIC_INFORMATION query(void *p) {
  MEMORY_BASIC_INFORMATION info{};
  require(VirtualQuery(p, &info, sizeof(info)) == sizeof(info), "VirtualQuery");
  return info;
}
bool fail_free = false;
unsigned attempts = 0;
void *fail_allocate(size_t) noexcept { ++attempts; return nullptr; }
void *pages(size_t bytes) noexcept {
  return VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN,
                      PAGE_READWRITE);
}
bool free_pages(void *p) noexcept {
  return !fail_free && VirtualFree(p, 0, MEM_RELEASE);
}
int main() {
  VideoMemory memory;
  require(!memory.allocate(0) && !memory.allocate(VideoMemory::minimum_size - 1),
          "small requests retain CRT path");
  require(memory.release(nullptr) == VideoMemory::Release::foreign,
          "null remains native");
  std::array<void *, VideoMemory::capacity> live{};
  for (auto &p : live) {
    p = memory.allocate(VideoMemory::minimum_size);
    require(p && query(p).AllocationBase == p && query(p).State == MEM_COMMIT,
            "each request owns a complete committed VA allocation");
  }
  require(!memory.allocate(VideoMemory::minimum_size), "full tracking table falls back");
  for (auto p : live) {
    require(memory.release(p) == VideoMemory::Release::released,
            "every tracked allocation releases");
    require(query(p).State == MEM_FREE, "release returns VA, not just physical pages");
  }
  auto *foreign = std::malloc(4096);
  require(foreign && memory.release(foreign) == VideoMemory::Release::foreign,
          "CRT allocation is never passed to VirtualFree");
  std::free(foreign);
  VideoMemory unavailable({fail_allocate, free_pages});
  require(!unavailable.allocate(VideoMemory::minimum_size) && attempts == 1,
          "OS failure leaves caller's native fallback available");
  VideoMemory retry({pages, free_pages});
  auto *p = retry.allocate(VideoMemory::minimum_size);
  require(p != nullptr, "retry fixture allocation");
  fail_free = true;
  require(retry.release(p) == VideoMemory::Release::failed && query(p).State == MEM_COMMIT,
          "failed release keeps ownership and excludes CRT free");
  fail_free = false;
  require(retry.release(p) == VideoMemory::Release::released && query(p).State == MEM_FREE,
          "failed release can be retried");
  std::atomic<unsigned> completed{0};
  std::array<std::thread, 4> threads;
  for (auto &thread : threads) thread = std::thread([&] {
    for (unsigned i = 0; i < 32; ++i) {
      auto *buffer = static_cast<unsigned char *>(memory.allocate(VideoMemory::minimum_size));
      require(buffer != nullptr, "concurrent allocation");
      buffer[0] = 0x37;
      buffer[VideoMemory::minimum_size - 1] = 0x73;
      require(memory.release(buffer) == VideoMemory::Release::released, "concurrent release");
      ++completed;
    }
  });
  for (auto &thread : threads) thread.join();
  require(completed == 128, "all concurrent requests completed");
  std::puts("PASS demand video VA release, table/OS failure, ownership retry and concurrency");
}
