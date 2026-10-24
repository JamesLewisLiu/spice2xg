#include "legacy_movie.h"
#include "legacy_movie_pool.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>

#include "util/logging.h"

namespace games::gitadora::legacy::movie {
namespace {
constexpr DWORD NEW_THUNK = 0xa57c;
constexpr DWORD FREE_THUNK = 0xa5ab;
struct Profile {
  DWORD timestamp, allocate, failed_load_free;
  DWORD game_timestamp, game_size, preload_allocate, preload_free, game_new,
      game_free;
  const char *name;
};
// Official IDA: full-file allocation in sub_10004C00 (CN) / sub_10004C20
// (JP), XFileLoad failure, and CAsyncFilter destructor at 0x10002860.
constexpr Profile PROFILES[] = {
    {0x4ccf6d0b, 0x4c67, 0x4ce1, 0x4ccf6e05, 0x930000, 0x90ef, 0x917c, 0xcbfd3,
     0xcbfd8, "CN"},
    {0x4d476d42, 0x4c87, 0x4d01, 0x4d476e40, 0x933000, 0x913f, 0x91cc, 0xce9c3,
     0xce9c8, "JP"},
};
using NativeNew = void *(__cdecl *)(size_t);
using NativeFree = void(__cdecl *)(void *);
NativeNew original_new = nullptr;
NativeFree original_free = nullptr;
NativeNew original_preload_new = nullptr;
NativeFree original_preload_free = nullptr;
BufferPool *pool = nullptr;
HMODULE attached_module = nullptr;
HMODULE attached_game = nullptr;
std::mutex attach_mutex;
std::once_flag prepare_once;

void *__cdecl allocate_video(size_t bytes) {
  if (void *buffer = pool->allocate(bytes)) {
    log_info("gdxg-movie", "Video buffer {} bytes at {} (reserved slot)", bytes,
             fmt::ptr(buffer));
    return buffer;
  }
  log_warning("gdxg-movie",
              "Video pool unavailable for {} bytes; using native allocator",
              bytes);
  return original_new(bytes);
}

void __cdecl release_video(void *buffer) {
  if (!pool->release(buffer)) {
    original_free(buffer);
  } else {
    log_info("gdxg-movie", "Released video buffer {} (slot retained)",
             fmt::ptr(buffer));
  }
}

void *__cdecl allocate_preload(size_t bytes) {
  if (void *buffer = pool->allocate(bytes)) {
    log_info("gdxg-movie", "Startup cache {} bytes at {} (reserved slice)",
             bytes, fmt::ptr(buffer));
    return buffer;
  }
  log_warning(
      "gdxg-movie",
      "Startup cache pool unavailable for {} bytes; using native allocator",
      bytes);
  return original_preload_new(bytes);
}

void __cdecl release_preload(void *buffer) {
  if (!pool->release(buffer))
    original_preload_free(buffer);
  else
    log_info("gdxg-movie", "Released failed startup cache {}",
             fmt::ptr(buffer));
}

template <size_t N>
bool matches(const BYTE *address, const BYTE (&expected)[N]) {
  return std::memcmp(address, expected, N) == 0;
}

bool calls(const BYTE *base, DWORD rva, DWORD target) {
  int32_t relative;
  std::memcpy(&relative, base + rva + 1, sizeof(relative));
  return base[rva] == 0xe8 && int64_t(rva) + 5 + relative == target;
}

// Only the signed CALL displacement changes. x86 arithmetic wraps modulo 2^32.
void redirect(BYTE *call, const void *target) {
  const auto relative = uint32_t(reinterpret_cast<uintptr_t>(target) -
                                 reinterpret_cast<uintptr_t>(call + 5));
  std::memcpy(call + 1, &relative, sizeof(relative));
}
} // namespace

void prepare() {
  std::call_once(prepare_once, [] {
#if defined(_M_IX86) || defined(__i386__)
    // Intentional process lifetime, matching the native graph/AVS contract.
    // Do not release reservations from static destruction or host shutdown.
    pool = new (std::nothrow) BufferPool;
    if (pool && pool->reserve()) {
      log_info("gdxg-movie", "Reserved 192 + 448 MiB shared movie ranges (640 "
                             "MiB total); commit on demand");
    } else {
      delete pool;
      pool = nullptr;
      log_warning("gdxg-movie",
                  "Cannot reserve 192 + 448 MiB; keeping native allocator");
    }
#endif
  });
}

bool attach(HMODULE module, HMODULE game) {
#if defined(_M_IX86) || defined(__i386__)
  std::lock_guard guard(attach_mutex);
  if (attached_module) {
    return attached_module == module && attached_game == game;
  }
  prepare();
  if (!pool) {
    return false;
  }
  const auto reject = [] {
    // No callsite has changed yet: return the address space to Windows if
    // the requested DLL cannot use it, rather than starving native heaps.
    delete pool;
    pool = nullptr;
    return false;
  };
  if (!module) {
    return reject();
  }
  auto *base = reinterpret_cast<BYTE *>(module);
  auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
      dos->e_lfanew > 0x1000) {
    return reject();
  }
  auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS32 *>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE ||
      nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
      nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
      nt->OptionalHeader.SizeOfImage != 0x52000) {
    log_warning("gdxg-movie",
                "Unsupported libmovie PE; keeping native allocator");
    return reject();
  }
  const Profile *profile = nullptr;
  for (const auto &candidate : PROFILES) {
    if (candidate.timestamp == nt->FileHeader.TimeDateStamp) {
      profile = &candidate;
    }
  }
  if (!profile) {
    log_warning("gdxg-movie",
                "Unknown libmovie build; keeping native allocator");
    return reject();
  }
  if (!game)
    return reject();
  auto *game_base = reinterpret_cast<BYTE *>(game);
  auto *game_dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(game_base);
  if (game_dos->e_magic != IMAGE_DOS_SIGNATURE || game_dos->e_lfanew <= 0 ||
      game_dos->e_lfanew > 0x1000)
    return reject();
  auto *game_nt = reinterpret_cast<const IMAGE_NT_HEADERS32 *>(
      game_base + game_dos->e_lfanew);
  if (game_nt->Signature != IMAGE_NT_SIGNATURE ||
      game_nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
      game_nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
      game_nt->OptionalHeader.SizeOfImage != profile->game_size ||
      game_nt->FileHeader.TimeDateStamp != profile->game_timestamp) {
    log_warning("gdxg-movie", "Unsupported game/libmovie pair; returning "
                              "reservations to native heaps");
    return reject();
  }
  const BYTE new_thunk[] = {0xe9, 0x91, 0xff, 0xff, 0xff};
  const BYTE free_thunk[] = {0xe9, 0xba, 0xfe, 0xff, 0xff};
  const BYTE new_entry[] = {0x55, 0x8b, 0xec, 0x83, 0xec, 0x0c};
  const BYTE after_new[] = {0x8b, 0xe8, 0x83, 0xc4, 0x04, 0x85, 0xed};
  const BYTE destructor[] = {0x8b, 0x86, 0xec, 0x19, 0x00, 0x00,
                             0x85, 0xc0, 0x74, 0x09, 0x50};
  const BYTE preload_new_thunk[] = {0xe9, 0x92, 0x15, 0x00, 0x00};
  const BYTE preload_free_thunk[] = {0xe9, 0x91, 0xfe, 0xff, 0xff};
  const BYTE preload_after_index[] = {0xc1, 0xe1, 0x04, 0x89, 0x81};
  if (!matches(base + NEW_THUNK, new_thunk) ||
      !matches(base + FREE_THUNK, free_thunk) ||
      !matches(base + 0xa512, new_entry) ||
      !matches(base + profile->allocate + 5, after_new) ||
      !matches(base + 0x2887, destructor) ||
      !calls(base, profile->allocate, NEW_THUNK) ||
      !calls(base, profile->failed_load_free, FREE_THUNK) ||
      !calls(base, 0x2892, FREE_THUNK) ||
      !matches(game_base + profile->game_new, preload_new_thunk) ||
      !matches(game_base + profile->game_free, preload_free_thunk) ||
      !matches(game_base + profile->game_new + 0x1597, new_entry) ||
      !calls(game_base, profile->preload_allocate, profile->game_new) ||
      !calls(game_base, profile->preload_free, profile->game_free) ||
      game_base[profile->preload_allocate - 1] != 0x50 ||
      game_base[profile->preload_free - 1] != 0x50 ||
      game_base[profile->preload_allocate + 5] != 0x8b ||
      game_base[profile->preload_allocate + 6] != 0x0d ||
      !matches(game_base + profile->preload_allocate + 11,
               preload_after_index) ||
      game_base[profile->preload_free + 5] != 0x8b ||
      game_base[profile->preload_free + 6] != 0x0d) {
    log_warning("gdxg-movie", "Movie/preloader allocation/free instructions "
                              "differ; keeping native allocators");
    return reject();
  }
  const std::array<BYTE *, 5> sites = {
      base + profile->allocate, base + profile->failed_load_free, base + 0x2892,
      game_base + profile->preload_allocate, game_base + profile->preload_free};
  std::array<DWORD, 5> protection{};
  // Acquire all write permissions before touching any callsite. On failure,
  // the native allocator and both of its matching releases remain intact.
  for (size_t i = 0; i < sites.size(); ++i) {
    if (!VirtualProtect(sites[i], 5, PAGE_EXECUTE_READWRITE, &protection[i])) {
      for (size_t j = i; j-- > 0;) {
        DWORD unused;
        VirtualProtect(sites[j], 5, protection[j], &unused);
      }
      log_warning("gdxg-movie",
                  "Cannot protect movie callsites; keeping native allocator");
      return reject();
    }
  }
  original_new = reinterpret_cast<NativeNew>(base + NEW_THUNK);
  original_free = reinterpret_cast<NativeFree>(base + FREE_THUNK);
  original_preload_new =
      reinterpret_cast<NativeNew>(game_base + profile->game_new);
  original_preload_free =
      reinterpret_cast<NativeFree>(game_base + profile->game_free);
  redirect(sites[1], reinterpret_cast<const void *>(release_video));
  redirect(sites[2], reinterpret_cast<const void *>(release_video));
  redirect(sites[4], reinterpret_cast<const void *>(release_preload));
  redirect(sites[0], reinterpret_cast<const void *>(allocate_video));
  redirect(sites[3], reinterpret_cast<const void *>(allocate_preload));
  // Reverse order matters when the allocation and failure sites share a page.
  for (size_t i = sites.size(); i-- > 0;) {
    DWORD unused;
    if (!VirtualProtect(sites[i], 5, protection[i], &unused)) {
      log_warning("gdxg-movie",
                  "Cannot restore movie code protection (error {})",
                  GetLastError());
    }
    FlushInstructionCache(GetCurrentProcess(), sites[i], 5);
  }
  attached_module = module;
  attached_game = game;
  log_info("gdxg-movie", "{} startup cache and video hooks active at {} / {}",
           profile->name, fmt::ptr(pool->reserved_base(0)),
           fmt::ptr(pool->reserved_base(1)));
  return true;
#else
  return false;
#endif
}
} // namespace games::gitadora::legacy::movie
