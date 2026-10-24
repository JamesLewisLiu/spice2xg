#include <objbase.h>
#include <windows.h>

// A standalone COM consumer; never loads native game DLLs.
extern "C" __declspec(dllexport) HRESULT create(REFCLSID clsid, IUnknown *outer,
                                                DWORD context, REFIID iid,
                                                void **object) {
  return CoCreateInstance(clsid, outer, context, iid, object);
}
