#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include "games/gitadora/legacy_movie.h"
#include "games/gitadora/legacy_movie_pool.h"
#include "util/logging.h"

std::string_view log_get_datetime() { return "test"; }
std::string_view log_get_datetime(std::time_t) { return "test"; }
void logger::push(std::string data, logger::Style, bool) {
  std::fputs(data.c_str(), stdout);
}

namespace {
void require(bool value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}
unsigned native_allocations = 0, native_frees = 0;
unsigned preload_native_allocations = 0, preload_native_frees = 0;
void *__cdecl preload_native_new(size_t bytes) {
  ++preload_native_allocations;
  return std::malloc(bytes);
}
void __cdecl preload_native_free(void *p) {
  ++preload_native_frees;
  std::free(p);
}
void *__cdecl native_new(size_t bytes) {
  ++native_allocations;
  return std::malloc(bytes);
}
void __cdecl native_free(void *p) {
  ++native_frees;
  std::free(p);
}
void branch(BYTE *site, BYTE opcode, const void *target) {
  site[0] = opcode;
  uint32_t displacement = uint32_t(reinterpret_cast<uintptr_t>(target) -
                                   reinterpret_cast<uintptr_t>(site + 5));
  std::memcpy(site + 1, &displacement, 4);
}
// Turn a validated callsite into a tiny cdecl test consumer. The original
// instruction/displacement remains intact and is executed by the CPU.
template <typename T> T consumer(BYTE *site) {
  const BYTE prefix[] = {0xff, 0x74, 0x24, 0x04}; // push [esp+4]
  const BYTE suffix[] = {0x83, 0xc4, 0x04, 0xc3}; // add esp,4; ret
  std::memcpy(site - 4, prefix, sizeof(prefix));
  std::memcpy(site + 5, suffix, sizeof(suffix));
  FlushInstructionCache(GetCurrentProcess(), site - 4, 13);
  return reinterpret_cast<T>(site - 4);
}
MEMORY_BASIC_INFORMATION query(void *p) {
  MEMORY_BASIC_INFORMATION info{};
  require(VirtualQuery(p, &info, sizeof(info)) == sizeof(info), "query memory");
  return info;
}

#ifdef GDXG_MOVIE_TEST_DEMAND
uint64_t address_space_used() {
  uint64_t used = 0;
  uintptr_t cursor = 0;
  MEMORY_BASIC_INFORMATION info{};
  while (VirtualQuery(reinterpret_cast<void *>(cursor), &info, sizeof(info)) ==
         sizeof(info)) {
    if (info.State != MEM_FREE)
      used += info.RegionSize;
    const auto next = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
    if (next <= cursor)
      break;
    cursor = next;
  }
  return used;
}
#endif

struct ImageView {
  BYTE *base;
  DWORD size;
};

ImageView map_image(const char *path) {
  std::ifstream input(path, std::ios::binary);
  require(bool(input), "open DLL as data");
  std::vector<BYTE> file((std::istreambuf_iterator<char>(input)), {});
  require(file.size() >= sizeof(IMAGE_DOS_HEADER), "DOS header available");
  auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(file.data());
  require(dos->e_lfanew > 0 &&
              size_t(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS32) < file.size(),
          "PE header available");
  auto *nt =
      reinterpret_cast<const IMAGE_NT_HEADERS32 *>(file.data() + dos->e_lfanew);
  const DWORD image_size = nt->OptionalHeader.SizeOfImage;
  require(image_size >= 0x52000 &&
              nt->OptionalHeader.SizeOfHeaders <= file.size(),
          "supported image bounds");
  auto *base = static_cast<BYTE *>(VirtualAlloc(
      nullptr, image_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  require(base != nullptr, "data image allocation");
  std::memcpy(base, file.data(), nt->OptionalHeader.SizeOfHeaders);
  auto *sections = IMAGE_FIRST_SECTION(nt);
  for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
    const auto &section = sections[i];
    require(uint64_t(section.VirtualAddress) + section.SizeOfRawData <=
                    image_size &&
                uint64_t(section.PointerToRawData) + section.SizeOfRawData <=
                    file.size(),
            "section bounds");
    std::memcpy(base + section.VirtualAddress,
                file.data() + section.PointerToRawData, section.SizeOfRawData);
  }
  DWORD previous;
  require(VirtualProtect(base, image_size, PAGE_EXECUTE_READ, &previous),
          "data image protection");
  return {base, image_size};
}

int inspect_image(const char *movie_path, const char *game_path) {
  // Map both exact DLLs as data only. LoadLibrary, DllMain, game code and the
  // proprietary decoder are never executed by this validation.
  const auto movie = map_image(movie_path);
  const auto game = map_image(game_path);
  require(games::gitadora::legacy::movie::attach(
              reinterpret_cast<HMODULE>(movie.base),
              reinterpret_cast<HMODULE>(game.base)),
          "exact game/libmovie pair accepted by production patcher");
  std::puts("PASS exact game/libmovie images accepted; all six callsites "
            "patched without executing game code");
  VirtualFree(game.base, 0, MEM_RELEASE);
  VirtualFree(movie.base, 0, MEM_RELEASE);
  return 0;
}

BYTE *make_game_fixture(bool jp) {
  const DWORD image_size = jp ? 0x933000 : 0x930000;
  auto *game = static_cast<BYTE *>(VirtualAlloc(
      nullptr, image_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  require(game != nullptr, "game fixture allocation");
  auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(game);
  dos->e_magic = IMAGE_DOS_SIGNATURE;
  dos->e_lfanew = 0x80;
  auto *nt = reinterpret_cast<IMAGE_NT_HEADERS32 *>(game + 0x80);
  nt->Signature = IMAGE_NT_SIGNATURE;
  nt->FileHeader.Machine = IMAGE_FILE_MACHINE_I386;
  nt->FileHeader.TimeDateStamp = jp ? 0x4d476e40 : 0x4ccf6e05;
  nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
  nt->OptionalHeader.SizeOfImage = image_size;

  const DWORD alloc = jp ? 0x913f : 0x90ef;
  const DWORD failure = jp ? 0x91cc : 0x917c;
  const DWORD new_thunk = jp ? 0xce9c3 : 0xcbfd3;
  const DWORD free_thunk = jp ? 0xce9c8 : 0xcbfd8;
  const DWORD new_entry = new_thunk + 0x1597;
  const DWORD free_body = free_thunk - 0x16a;
  game[alloc - 1] = 0x50;
  game[failure - 1] = 0x50;
  branch(game + alloc, 0xe8, game + new_thunk);
  game[alloc + 5] = 0x8b;
  game[alloc + 6] = 0x0d;
  const BYTE after_index[] = {0xc1, 0xe1, 0x04, 0x89, 0x81};
  std::memcpy(game + alloc + 11, after_index, sizeof(after_index));
  branch(game + failure, 0xe8, game + free_thunk);
  game[failure + 5] = 0x8b;
  game[failure + 6] = 0x0d;
  branch(game + new_thunk, 0xe9, game + new_entry);
  const BYTE new_prologue[] = {0x55, 0x8b, 0xec, 0x83, 0xec, 0x0c};
  std::memcpy(game + new_entry, new_prologue, sizeof(new_prologue));
  const BYTE new_body_prefix[] = {0xff, 0x75, 0x08, 0xb8};
  std::memcpy(game + new_entry + 6, new_body_prefix, sizeof(new_body_prefix));
  const auto new_address = reinterpret_cast<uintptr_t>(preload_native_new);
  std::memcpy(game + new_entry + 10, &new_address, sizeof(uint32_t));
  const BYTE new_body_suffix[] = {0xff, 0xd0, 0x83, 0xc4, 0x04,
                                  0x8b, 0xe5, 0x5d, 0xc3};
  std::memcpy(game + new_entry + 14, new_body_suffix, sizeof(new_body_suffix));
  branch(game + free_thunk, 0xe9, game + free_body);
  branch(game + free_body, 0xe9,
         reinterpret_cast<const void *>(preload_native_free));
  DWORD previous;
  require(VirtualProtect(game, image_size, PAGE_EXECUTE_READ, &previous),
          "game fixture RX");
  return game;
}
} // namespace

int main(int argc, char **argv) {
  using namespace games::gitadora::legacy::movie;
  if (argc > 3 && std::strcmp(argv[1], "image") == 0)
    return inspect_image(argv[2], argv[3]);
  const bool jp = argc > 1 && std::strcmp(argv[1], "jp") == 0;
  const bool corrupt = argc > 2;
  auto *base = static_cast<BYTE *>(
      VirtualAlloc(nullptr, 0x52000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  require(base != nullptr, "fixture allocation");
  auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
  dos->e_magic = IMAGE_DOS_SIGNATURE;
  dos->e_lfanew = 0x80;
  auto *nt = reinterpret_cast<IMAGE_NT_HEADERS32 *>(base + 0x80);
  nt->Signature = IMAGE_NT_SIGNATURE;
  nt->FileHeader.Machine = IMAGE_FILE_MACHINE_I386;
  nt->FileHeader.TimeDateStamp = jp ? 0x4d476d42 : 0x4ccf6d0b;
  nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
  nt->OptionalHeader.SizeOfImage = 0x52000;
  const DWORD alloc = jp ? 0x4c87 : 0x4c67;
  const DWORD failure = jp ? 0x4d01 : 0x4ce1;
  constexpr DWORD copy_alloc = 0x2c3b;
  const BYTE after[] = {0x8b, 0xe8, 0x83, 0xc4, 0x04, 0x85, 0xed};
  const BYTE dtor[] = {0x8b, 0x86, 0xec, 0x19, 0,   0,
                       0x85, 0xc0, 0x74, 0x09, 0x50};
  const BYTE prologue[] = {0x55, 0x8b, 0xec, 0x83, 0xec, 0x0c};
  std::memcpy(base + alloc + 5, after, sizeof(after));
  const BYTE after_copy[] = {0x8b, 0xf8, 0x83, 0xc4, 0x04, 0x3b, 0xfb};
  base[copy_alloc - 1] = 0x55;
  std::memcpy(base + copy_alloc + 5, after_copy, sizeof(after_copy));
  std::memcpy(base + 0x2887, dtor, sizeof(dtor));
  std::memcpy(base + 0xa512, prologue, sizeof(prologue));
  const BYTE new_body_prefix[] = {0xff, 0x75, 0x08, 0xb8};
  std::memcpy(base + 0xa518, new_body_prefix, sizeof(new_body_prefix));
  const auto native_new_address = reinterpret_cast<uintptr_t>(native_new);
  std::memcpy(base + 0xa51c, &native_new_address, sizeof(uint32_t));
  const BYTE new_body_suffix[] = {0xff, 0xd0, 0x83, 0xc4, 0x04,
                                  0x8b, 0xe5, 0x5d, 0xc3};
  std::memcpy(base + 0xa520, new_body_suffix, sizeof(new_body_suffix));
  branch(base + 0xa46a, 0xe9, reinterpret_cast<const void *>(native_free));
  branch(base + 0xa57c, 0xe9, base + 0xa512);
  branch(base + 0xa5ab, 0xe9, base + 0xa46a);
  branch(base + alloc, 0xe8, base + 0xa57c);
  branch(base + copy_alloc, 0xe8, base + 0xa57c);
  branch(base + failure, 0xe8, base + 0xa5ab);
  branch(base + 0x2892, 0xe8, base + 0xa5ab);
  auto *game = make_game_fixture(jp);
  const DWORD game_size = jp ? 0x933000 : 0x930000;
  const DWORD preload_site = jp ? 0x913f : 0x90ef;
  const DWORD preload_free = jp ? 0x91cc : 0x917c;
  DWORD game_old;
  require(VirtualProtect(game, game_size, PAGE_EXECUTE_READWRITE, &game_old),
          "game fixture mutation");
  if (corrupt) {
    if (std::strcmp(argv[2], "timestamp") == 0)
      nt->FileHeader.TimeDateStamp = 0;
    else if (std::strcmp(argv[2], "game-timestamp") == 0)
      reinterpret_cast<IMAGE_NT_HEADERS32 *>(game + 0x80)
          ->FileHeader.TimeDateStamp = 0;
    else if (std::strcmp(argv[2], "game-release") == 0)
      game[preload_free] ^= 1;
    else if (std::strcmp(argv[2], "copy-call") == 0)
      base[copy_alloc + 1] ^= 1;
    else if (std::strcmp(argv[2], "copy-context") == 0)
      base[copy_alloc + 5] ^= 1;
    else
      base[failure] ^= 1;
  }
  const std::vector<BYTE> game_before(game, game + game_size);
  require(VirtualProtect(game, game_size, PAGE_EXECUTE_READ, &game_old),
          "game fixture RX restored");
  const std::vector<BYTE> before(base, base + 0x52000);
  DWORD old;
  require(VirtualProtect(base, 0x52000, PAGE_EXECUTE_READ, &old), "fixture RX");
#ifdef GDXG_MOVIE_TEST_DEMAND
  const auto used_before_attach = address_space_used();
#endif
  const bool installed =
      attach(reinterpret_cast<HMODULE>(base), reinterpret_cast<HMODULE>(game));
  if (corrupt) {
    require(!installed, "unknown build or mismatched release rejected");
    require(std::memcmp(before.data(), base, before.size()) == 0,
            "rejection changes no code");
    require(std::memcmp(game_before.data(), game, game_before.size()) == 0,
            "rejection leaves game code untouched");
    std::puts(
        "PASS unsupported build/corrupt free call rejected without mutation");
    return 0;
  }
  require(installed && attach(reinterpret_cast<HMODULE>(base),
                              reinterpret_cast<HMODULE>(game)),
          "attach and repeat attach");
#ifdef GDXG_MOVIE_TEST_DEMAND
  require(address_space_used() < used_before_attach + 16u * 1024u * 1024u,
          "production attach reserves no movie arena (including repeat attach)");
#endif
  require(query(base + alloc).Protect == PAGE_EXECUTE_READ &&
              query(base + 0x2892).Protect == PAGE_EXECUTE_READ &&
              query(game + preload_site).Protect == PAGE_EXECUTE_READ,
          "RX protection restored, including shared callsite page");
  for (size_t i = 0; i < before.size(); ++i) {
    bool displacement = false;
    for (DWORD site : {alloc, failure, DWORD(0x2892), copy_alloc})
      displacement |= i > site && i < site + 5;
    require(displacement || before[i] == base[i],
            "only four movie CALL displacements change");
  }
  for (size_t i = 0; i < game_before.size(); ++i) {
    bool displacement = false;
    for (DWORD site : {preload_site, preload_free})
      displacement |= i > site && i < site + 5;
    require(displacement || game_before[i] == game[i],
            "only two game CALL displacements change");
  }
  require(VirtualProtect(base, 0x52000, PAGE_EXECUTE_READWRITE, &old),
          "enable test consumer");
  require(VirtualProtect(game, game_size, PAGE_EXECUTE_READWRITE, &old),
          "enable preloader consumers");
  using New = void *(__cdecl *)(size_t);
  using Free = void(__cdecl *)(void *);
  auto allocate = consumer<New>(base + alloc);
  auto copy_allocate = consumer<New>(base + copy_alloc);
  auto failed_load = consumer<Free>(base + failure);
  auto destroy = consumer<Free>(base + 0x2892);
#ifdef GDXG_MOVIE_TEST_DEMAND
  auto preload = consumer<New>(game + preload_site);
  auto preload_failure = consumer<Free>(game + preload_free);
  std::array<BYTE *, 3> cache{};
  const std::array<size_t, 3> cache_sizes = {jp ? 119999875u : 119999874u,
                                          44999875u, 17249877u};
  for (size_t i = 0; i < cache.size(); ++i) {
    cache[i] = static_cast<BYTE *>(preload(cache_sizes[i]));
    require(cache[i] && preload_native_allocations == 0 &&
                query(cache[i]).AllocationBase == cache[i],
            "startup request owns on-demand pages without CRT allocation");
    std::memset(cache[i], int(0x31 + i), cache_sizes[i]);
  }
  for (unsigned iteration = 0; iteration < 3; ++iteration) {
    std::array<BYTE *, 3> copies{};
    const auto allocations_before = native_allocations;
    const auto frees_before = native_frees;
    for (size_t i = 0; i < copies.size(); ++i) {
      copies[i] = static_cast<BYTE *>(copy_allocate(cache_sizes[i]));
      require(copies[i] && native_allocations == allocations_before &&
                  query(copies[i]).AllocationBase == copies[i],
              "owned cached copy owns independent pages");
      std::memcpy(copies[i], cache[i], cache_sizes[i]);
      require(std::memcmp(copies[i], cache[i], cache_sizes[i]) == 0,
              "native owned copy matches complete persistent source");
    }
    failed_load(copies[0]);
    destroy(copies[1]);
    destroy(copies[2]);
    for (auto *copy : copies)
      require(query(copy).State == MEM_FREE, "copy destruction returns entire VA");
    require(native_frees == frees_before && preload_native_frees == 0,
            "page failure and destructor never reach either native CRT");
    auto *music = static_cast<BYTE *>(allocate(161999874));
    auto *theme = static_cast<BYTE *>(allocate(89999876));
    require(music && theme && music != theme,
            "historical third-song pair coexists with persistent caches");
    music[161999873] = 0x61;
    theme[89999875] = 0x62;
    destroy(music);
    destroy(theme);
    require(query(music).State == MEM_FREE && query(theme).State == MEM_FREE,
            "third-song destruction returns complete ranges without retained reserve");
    require(native_allocations == native_frees,
            "transient pages never reach either native allocator");
    for (size_t i = 0; i < cache.size(); ++i)
      require(cache[i][0] == BYTE(0x31 + i) &&
                  cache[i][cache_sizes[i] - 1] == BYTE(0x31 + i),
              "persistent startup source survives transient frees");
  }
  for (auto *cached : cache) {
    preload_failure(cached);
    require(query(cached).State == MEM_FREE, "startup failure returns entire VA range");
  }
  require(preload_native_allocations == 0 && preload_native_frees == 0,
          "startup pages never reach game CRT");
  // Small requests and exhausted bookkeeping retain module-specific CRT pairing.
  destroy(allocate(1024));
  preload_failure(preload(1024));
  require(native_allocations == 1 && native_frees == 1 &&
              preload_native_allocations == 1 && preload_native_frees == 1,
          "small native allocations pair with the correct module's CRT");
  std::array<void *, 64> live{};
  for (auto &p : live) p = allocate(1024 * 1024);
  auto *fallback_movie = allocate(1024 * 1024);
  auto *fallback_cache = preload(1024 * 1024);
  require(fallback_movie && fallback_cache && native_allocations == 2 &&
              preload_native_allocations == 2,
          "full ownership table falls back to each original allocator");
  destroy(fallback_movie);
  preload_failure(fallback_cache);
  for (auto p : live) {
    require(p != nullptr, "tracking capacity allocated");
    destroy(p);
    require(query(p).State == MEM_FREE, "tracked fallback test pages fully returned");
  }
  require(native_allocations == native_frees &&
              preload_native_allocations == preload_native_frees,
          "all fallback CRT allocations balanced");
  const auto before_direct = native_allocations;
  auto native = reinterpret_cast<New>(base + 0xa57c);
  auto direct_free = reinterpret_cast<Free>(base + 0xa5ab);
  direct_free(native(1024));
  require(native_allocations == before_direct + 1 && native_allocations == native_frees,
          "unhooked non-video operator new/free remains native");
  std::puts("PASS production on-demand pages, no fixed reservation, exact x86 CALL consumers, full copies, paired CRT fallback and complete VA release");
  return 0;
#else
  auto *a = static_cast<BYTE *>(allocate(119999874));
  auto *b = static_cast<BYTE *>(allocate(114133210));
  require(a && b && a != b && native_allocations == 0,
          "both movie sizes use reserved slots");
  require(query(a).Type == MEM_PRIVATE && query(a).State == MEM_COMMIT,
          "movie buffer is committed virtual memory");
  a[119999873] = 1;
  b[114133209] = 2;
  failed_load(a);
  require(query(a).State == MEM_RESERVE && native_frees == 0,
          "load-failure free decommits without CRT");
  auto *replacement = allocate(119999874);
  require(replacement == a, "next source reuses reservation");
  auto *fallback = allocate(360 * 1024 * 1024);
  require(fallback && native_allocations == 1,
          "full pool falls back to native allocator");
  destroy(fallback);
  require(native_frees == 1, "native fallback pairs with native free");
  destroy(replacement);
  destroy(b);
  require(query(a).State == MEM_RESERVE && query(b).State == MEM_RESERVE &&
              native_frees == 1,
          "both destructor paths retain reservations");
  auto native = reinterpret_cast<New>(base + 0xa57c);
  auto direct_free = reinterpret_cast<Free>(base + 0xa5ab);
  direct_free(native(1024));
  require(native_allocations == 2 && native_frees == 2,
          "non-video operator new/free untouched");
  auto preload = consumer<New>(game + preload_site);
  auto preload_failure = consumer<Free>(game + preload_free);
  std::array<BYTE *, 3> cache{};
  const std::array<size_t, 3> cache_sizes = {jp ? 119999875u : 119999874u,
                                             44999875u, 17249877u};
  for (size_t i = 0; i < cache.size(); ++i) {
    cache[i] = static_cast<BYTE *>(preload(cache_sizes[i]));
    require(cache[i] && query(cache[i]).AllocationBase ==
                            query(cache[0]).AllocationBase,
            "all three startup caches share one reservation");
    cache[i][0] = BYTE(10 + i);
    cache[i][cache_sizes[i] - 1] = BYTE(20 + i);
  }
  require(native_allocations == 2 && preload_native_allocations == 0,
          "startup preload bypasses native CRT");
  // Exercise the distinct optional memory-input copy call.
  // The three persistent caches must survive destruction of each owned copy.
  for (unsigned iteration = 0; iteration < 12; ++iteration) {
    const auto previous_native = native_allocations;
    std::array<BYTE *, 3> copies{};
    for (size_t i = 0; i < copies.size(); ++i) {
      copies[i] = static_cast<BYTE *>(copy_allocate(cache_sizes[i]));
      require(copies[i] && native_allocations == previous_native,
              "cached video copies use reservations");
      std::memcpy(copies[i], cache[i], cache_sizes[i]);
      require(copies[i][0] == BYTE(10 + i) &&
                  copies[i][cache_sizes[i] - 1] == BYTE(20 + i),
              "owned cached copies preserve complete source boundaries");
    }
    for (auto *copy : copies)
      destroy(copy);
    for (auto *copy : copies)
      require(query(copy).State == MEM_RESERVE,
              "cached copies decommit through the movie pool");
    require(native_frees == 2,
            "cached copies never reach the native CRT free");
    for (size_t i = 0; i < cache.size(); ++i)
      require(cache[i][0] == BYTE(10 + i) &&
                  cache[i][cache_sizes[i] - 1] == BYTE(20 + i),
              "destroying copies preserves persistent caches");
  }
  for (unsigned iteration = 0; iteration < 20; ++iteration) {
    auto *video = static_cast<BYTE *>(allocate(114133210));
    require(
        video && query(video).AllocationBase != query(cache[0]).AllocationBase,
        "gameplay video uses second arena alongside persistent startup cache");
    video[114133209] = 0x5a;
    destroy(video);
    for (size_t i = 0; i < cache.size(); ++i)
      require(cache[i][0] == BYTE(10 + i) &&
                  cache[i][cache_sizes[i] - 1] == BYTE(20 + i),
              "transient source destruction preserves cached movie bytes");
  }
  // Exact third-song pair from the 2026-10-24 JP crash. Both sources must
  // coexist with the three persistent system caches without native fallback.
  for (unsigned iteration = 0; iteration < 20; ++iteration) {
    const auto previous_native = native_allocations;
    auto *music = static_cast<BYTE *>(allocate(161999874));
    auto *theme = static_cast<BYTE *>(allocate(89999876));
    auto *overlap = static_cast<BYTE *>(allocate(161999874));
    require(music && theme && overlap && native_allocations == previous_native,
            "162 MB + 90 MB + 162 MB fit pool alongside startup caches");
    require(query(music).AllocationBase == query(theme).AllocationBase,
            "both large sources share expanded playback reservation");
    music[161999873] = 0x61;
    theme[89999875] = 0x62;
    overlap[161999873] = 0x63;
    destroy(music);
    destroy(theme);
    destroy(overlap);
    for (size_t i = 0; i < cache.size(); ++i)
      require(cache[i][0] == BYTE(10 + i) &&
                  cache[i][cache_sizes[i] - 1] == BYTE(20 + i),
              "third-song source destruction preserves startup cache");
  }
  auto *live_video = allocate(114133210);
  require(live_video && query(live_video).AllocationBase !=
                            query(cache[0]).AllocationBase,
          "live gameplay video coexists with all persistent startup caches");
  auto *preload_fallback = preload(360 * 1024 * 1024);
  require(preload_fallback && preload_native_allocations == 1 &&
              native_allocations == 2,
          "preloader uses its own native CRT when both reservations lack a 100 "
          "MiB range");
  preload_failure(preload_fallback);
  require(preload_native_frees == 1 && native_frees == 2,
          "preload fallback paired with its own native free");
  destroy(live_video);
  preload_failure(cache[1]);
  require(query(cache[1]).State == MEM_RESERVE && preload_native_frees == 1 &&
              native_frees == 2,
          "preloader failure never reaches CRT");
  cache[1] = static_cast<BYTE *>(preload(cache_sizes[1]));
  require(cache[1] &&
              query(cache[1]).AllocationBase == query(cache[0]).AllocationBase,
          "preload failure hole is reusable");
  for (auto *cached : cache)
    preload_failure(cached);
  std::puts("PASS x86 JP/CN CALL ABI, native fallback, both frees, source "
            "replacement, RX protection");
#endif
}
