#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <dshow.h>
#include <filesystem>
#include <thread>
#include <vector>

#include "games/gitadora/legacy_codec.h"
#include "util/detour.h"
#include "util/logging.h"

// Only logging/fatal UI are replaced. The codec, PE import lookup/patch,
// VirtualProtect guard and PEB helpers are production sources.
std::string_view log_get_datetime() { return "test"; }
std::string_view log_get_datetime(std::time_t) { return "test"; }
void logger::push(std::string data, logger::Style, bool) {
  std::fputs(data.c_str(), stdout);
}
void show_popup_for_fatal_error(std::string) { std::abort(); }
void launcher::stop_subsystems() { std::abort(); }
void launcher::kill(UINT) { std::abort(); }

// Unused by the scoped IAT path; required by detour/PEB diagnostic helpers.
std::wstring s2ws(const std::string &) { std::abort(); }
std::string ws2s(const std::wstring &) { std::abort(); }

namespace {
constexpr CLSID NEWER = {0x37991d68,
                         0x42a3,
                         0x40e3,
                         {0x8c, 0x05, 0x03, 0x71, 0x70, 0xe1, 0xa4, 0x2a}};
constexpr CLSID OLDER = {0x8acd52ed,
                         0x9c2d,
                         0x4008,
                         {0x91, 0x29, 0xdc, 0xe9, 0x55, 0xd8, 0x60, 0x65}};
std::atomic_uint fallbacks{0}, other_calls{0}, global_com_calls{0};
using Create = HRESULT (*)(REFCLSID, IUnknown *, DWORD, REFIID, void **);
Create create;

void require(bool ok, const char *reason) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", reason);
    std::exit(1);
  }
}

HRESULT WINAPI deny_registered(REFCLSID clsid, IUnknown *outer, DWORD context,
                               REFIID iid, void **object) {
  if (IsEqualGUID(clsid, NEWER) || IsEqualGUID(clsid, OLDER)) {
    ++fallbacks;
    require(outer == nullptr && IsEqualGUID(iid, IID_IBaseFilter),
            "fallback parameters preserved");
    require(context == CLSCTX_INPROC_SERVER || context == CLSCTX_LOCAL_SERVER,
            "fallback context preserved");
    if (object)
      *object = nullptr;
    return REGDB_E_CLASSNOTREG;
  }
  ++other_calls;
  return CoCreateInstance(clsid, outer, context, iid, object);
}

// Like audio::init's module-wide sweep, this has one shared system fallback,
// not a separate saved function for every module it overwrites.
HRESULT WINAPI global_com_hook(REFCLSID clsid, IUnknown *outer, DWORD context,
                               REFIID iid, void **object) {
  ++global_com_calls;
  return deny_registered(clsid, outer, context, iid, object);
}

void repeat(REFCLSID clsid, int count) {
  require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)),
          "worker COM init");
  std::printf("worker %lu COM initialized\n", GetCurrentThreadId());
  for (int i = 0; i < count; ++i) {
    IBaseFilter *filter = nullptr;
    require(create(clsid, nullptr, CLSCTX_INPROC_SERVER, IID_IBaseFilter,
                   reinterpret_cast<void **>(&filter)) == S_OK &&
                filter,
            "repeated local creation");
    if (i == 0 || i == count - 1)
      std::printf("worker %lu filter %d created\n", GetCurrentThreadId(),
                  i + 1);
    filter->Release();
    CoFreeUnusedLibraries();
  }
  CoUninitialize();
}
} // namespace

int main(int argc, char **argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  require(argc == 2, "mode argument: newer / older / fallback");
  require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), "COM init");
  wchar_t exe[32768];
  require(GetModuleFileNameW(nullptr, exe, 32768) != 0, "exe path");
  auto consumer_path =
      std::filesystem::path(exe).parent_path() / L"gdxg_codec_consumer.dll";
  auto consumer = LoadLibraryW(consumer_path.c_str());
  require(consumer != nullptr, "load standalone consumer");
  create = reinterpret_cast<Create>(GetProcAddress(consumer, "create"));
  require(create != nullptr, "consumer export");
  require(detour::iat_try("CoCreateInstance", deny_registered, consumer,
                          "ole32.dll") != nullptr,
          "deny registered decoder in consumer");
  games::gitadora::legacy::codec::attach(nullptr);
  games::gitadora::legacy::codec::attach(consumer);
  auto slot = detour::iat_find("CoCreateInstance", consumer, "ole32.dll");
  require(slot && *slot != reinterpret_cast<void *>(deny_registered),
          "production IAT hook installed");
  auto hook = *slot;
  require(detour::iat_try("CoCreateInstance", global_com_hook, consumer,
                          "ole32.dll") == hook,
          "global COM sweep overwrites the early codec hook");
  IBaseFilter *bypassed = nullptr;
  require(create(NEWER, nullptr, CLSCTX_INPROC_SERVER, IID_IBaseFilter,
                 reinterpret_cast<void **>(&bypassed)) == REGDB_E_CLASSNOTREG &&
              !bypassed && fallbacks == 1 && global_com_calls == 1,
          "early attachment reproduces registered fallback before late attach");
  fallbacks = 0;
  global_com_calls = 0;
  // The native frontend must do this after launcher audio initialization,
  // before boot/video creation. Decoder failures must still pass through audio.
  games::gitadora::legacy::codec::attach(consumer);
  require(*slot == hook,
          "late codec hook restored above global audio COM hook");
  games::gitadora::legacy::codec::attach(consumer);
  require(*slot == hook, "repeated attach preserves hook");
  require(SetCurrentDirectoryW(L"C:\\Windows"), "change CWD");

  // Neither the invalid output pointer nor non-inproc context should load AX.
  require(create(NEWER, nullptr, CLSCTX_INPROC_SERVER, IID_IBaseFilter,
                 nullptr) == E_POINTER,
          "null output rejected");
  require(fallbacks == 0, "null output does not reach original COM");
  require(global_com_calls == 0,
          "null output does not reach global audio hook");
  IBaseFilter *filter = nullptr;
  require(create(NEWER, nullptr, CLSCTX_LOCAL_SERVER, IID_IBaseFilter,
                 reinterpret_cast<void **>(&filter)) == REGDB_E_CLASSNOTREG &&
              !filter && fallbacks == 1 && global_com_calls == 1,
          "non-inproc request forwarded unchanged");
  fallbacks = 0;
  global_com_calls = 0;

  auto result = create(NEWER, nullptr, CLSCTX_INPROC_SERVER, IID_IBaseFilter,
                       reinterpret_cast<void **>(&filter));
  std::printf("newer=%08lx fallbacks=%u\n", static_cast<unsigned long>(result),
              fallbacks.load());
  if (std::string_view(argv[1]) == "fallback") {
    require(result == REGDB_E_CLASSNOTREG && !filter && fallbacks == 1 &&
                global_com_calls == 1,
            "local failure preserves original COM result");
    std::puts("PASS late attachment after global COM sweep and failure "
              "fallback without registry changes");
    CoUninitialize();
    return 0;
  }
  const auto &clsid = std::string_view(argv[1]) == "older" ? OLDER : NEWER;
  if (IsEqualGUID(clsid, OLDER)) {
    require(result == REGDB_E_CLASSNOTREG && !filter && fallbacks == 1,
            "unsupported newer class retains native fallback");
    require(create(OLDER, nullptr, CLSCTX_INPROC_SERVER, IID_IBaseFilter,
                   reinterpret_cast<void **>(&filter)) == S_OK &&
                filter,
            "native older class fallback succeeds locally");
  } else {
    require(result == S_OK && filter && fallbacks == 0,
            "local newer class succeeds without registered decoder");
  }
  auto expected_fallbacks = fallbacks.load();
  HMODULE owner = nullptr;
  require(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCWSTR>(
                                 (*reinterpret_cast<void ***>(filter))[0]),
                             &owner),
          "filter vtable module");
  wchar_t path[32768];
  require(GetModuleFileNameW(owner, path, 32768) != 0, "decoder module path");
  require(std::filesystem::equivalent(
              path, std::filesystem::path(exe).parent_path() / L"clvsd.ax"),
          "decoder object comes from executable directory");
  std::puts("local object module verified");

  IGraphBuilder *graph = nullptr;
  require(create(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
                 IID_IGraphBuilder,
                 reinterpret_cast<void **>(&graph)) == S_OK &&
              graph && other_calls == 1 &&
              global_com_calls == expected_fallbacks + 1,
          "non-decoder COM forwarded through the global audio hook");
  require(graph->AddFilter(filter, L"Local decoder") == S_OK,
          "actual graph AddFilter");
  require(graph->RemoveFilter(filter) == S_OK, "actual graph RemoveFilter");
  std::puts("graph Add/Remove verified");
  graph->Release();
  filter->Release();
  std::puts("starting four COM workers");
  std::vector<std::thread> threads;
  std::atomic_uint finished{0};
  for (int i = 0; i < 4; ++i)
    threads.emplace_back([&] {
      repeat(clsid, 1);
      ++finished;
    });
  // DirectShow filters may own windows on the first calling thread. Keep its
  // queue alive as the real host does, rather than blocking it in join().
  while (finished < 4) {
    MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }
  for (auto &thread : threads)
    thread.join();
  std::puts("concurrent creation finished");
  repeat(clsid, 5);
  require(fallbacks == expected_fallbacks,
          "repeat/rebuild never uses registered supported decoder");
  require(
      global_com_calls == expected_fallbacks + 1,
      "supported decoder bypasses global audio while other COM preserves it");
  std::puts("PASS late attachment after global COM sweep, production IAT, "
            "local module, graph Add/Remove, changed CWD, "
            "five sequential CoFreeUnusedLibraries rebuild cycles, and four "
            "concurrent creations");
  CoUninitialize();
}
