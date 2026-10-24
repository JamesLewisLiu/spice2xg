#include "legacy_codec.h"

#include <array>
#include <atomic>
#include <mutex>
#include <objbase.h>
#include <string>

#include "util/detour.h"
#include "util/logging.h"

namespace games::gitadora::legacy::codec {
namespace {
// Both XG1 regions try the newer class, then the older one. Do not
// remap them: different clvsd.ax releases expose different classes.
constexpr std::array<CLSID, 2> DECODERS = {{
    {0x37991d68,
     0x42a3,
     0x40e3,
     {0x8c, 0x05, 0x03, 0x71, 0x70, 0xe1, 0xa4, 0x2a}},
    {0x8acd52ed,
     0x9c2d,
     0x4008,
     {0x91, 0x29, 0xdc, 0xe9, 0x55, 0xd8, 0x60, 0x65}},
}};
constexpr IID FACTORY_IID = {0x00000001, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};

using CreateInstance = HRESULT(WINAPI *)(REFCLSID, IUnknown *, DWORD, REFIID,
                                         void **);
using GetClassObject = HRESULT(WINAPI *)(REFCLSID, REFIID, void **);
CreateInstance original_create = nullptr;
std::mutex hook_mutex;
std::once_flag load_once;
GetClassObject get_class_object = nullptr;
HMODULE codec_module = nullptr;
std::atomic_bool created[DECODERS.size()]{};

void load_local() {
  // Anchor to spice.exe, not CWD: launchers may start elsewhere. Use
  // Unicode and an absolute path so the AX's dependencies stay local.
  std::wstring path(32768, L'\0');
  auto length =
      GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (length == 0 || length >= path.size()) {
    log_warning("gdxg-codec",
                "Cannot resolve executable path; using registered decoder");
    return;
  }
  path.resize(length);
  auto slash = path.find_last_of(L"\\/");
  if (slash == std::wstring::npos) {
    log_warning(
        "gdxg-codec",
        "Cannot resolve executable directory; using registered decoder");
    return;
  }
  path.resize(slash + 1);
  path += L"clvsd.ax";
  auto attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    log_info("gdxg-codec",
             "No accessible clvsd.ax beside spice.exe (error {}); using "
             "registered decoder",
             GetLastError());
    return;
  }
  if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
    log_warning("gdxg-codec",
                "Local clvsd.ax is a directory; using registered decoder");
    return;
  }
  codec_module =
      LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!codec_module) {
    log_warning(
        "gdxg-codec",
        "Cannot load local clvsd.ax (error {}); using registered decoder",
        GetLastError());
    return;
  }
  get_class_object = reinterpret_cast<GetClassObject>(
      GetProcAddress(codec_module, "DllGetClassObject"));
  if (!get_class_object) {
    log_warning(
        "gdxg-codec",
        "Local clvsd.ax has no DllGetClassObject; using registered decoder");
  }
  // Keep a loader reference until process exit: native libmovie calls
  // CoFreeUnusedLibraries between graphs. Factories are per-call and
  // released on the calling COM thread, never shared across apartments.
}

HRESULT WINAPI create_instance(REFCLSID clsid, IUnknown *outer, DWORD context,
                               REFIID iid, void **object) {
  for (size_t i = 0; i < DECODERS.size(); ++i) {
    if (!IsEqualGUID(clsid, DECODERS[i]) || !(context & CLSCTX_INPROC_SERVER)) {
      continue;
    }
    if (!object) {
      return E_POINTER;
    }
    std::call_once(load_once, load_local);
    if (!get_class_object) {
      break;
    }
    *object = nullptr;
    IClassFactory *factory = nullptr;
    auto result = get_class_object(clsid, FACTORY_IID,
                                   reinterpret_cast<void **>(&factory));
    if (SUCCEEDED(result) && !factory) {
      result = E_UNEXPECTED;
    }
    if (SUCCEEDED(result) && factory) {
      result = factory->CreateInstance(outer, iid, object);
      factory->Release();
      factory = nullptr;
      if (SUCCEEDED(result)) {
        if (!created[i].exchange(true)) {
          log_info("gdxg-codec",
                   "Local clvsd.ax {} decoder created without registration",
                   i == 0 ? "newer" : "older");
        }
        return result;
      }
    }
    if (factory) {
      factory->Release();
    }
    if (result != CLASS_E_CLASSNOTAVAILABLE) {
      log_warning("gdxg-codec",
                  "Local clvsd.ax {} class creation failed ({}); trying "
                  "registered decoder",
                  i == 0 ? "newer" : "older", FMT_HRESULT(result));
    } else {
      log_misc(
          "gdxg-codec",
          "Local clvsd.ax does not provide {} class; keeping native fallback",
          i == 0 ? "newer" : "older");
    }
    // Preserve the exact request and the game's newer -> older
    // fallback. Other COM classes always use the original import.
    *object = nullptr;
    break;
  }
  return original_create(clsid, outer, context, iid, object);
}
} // namespace

void attach(HMODULE movie) {
  if (!movie) {
    return;
  }
  std::lock_guard lock(hook_mutex);
  auto slot = detour::iat_find("CoCreateInstance", movie, "ole32.dll");
  if (!slot || *slot == reinterpret_cast<void *>(create_instance)) {
    return;
  }
  // Save before exposing the hook, and never replace the original with
  // our own function if the same native dependency is attached twice.
  original_create = reinterpret_cast<CreateInstance>(*slot);
  if (detour::iat_try("CoCreateInstance", create_instance, movie,
                      "ole32.dll")) {
    log_info("gdxg-codec",
             "Native libmovie decoder hook installed (spice.exe directory)");
  } else {
    log_warning("gdxg-codec", "Cannot install native libmovie decoder hook");
  }
}
} // namespace games::gitadora::legacy::codec
