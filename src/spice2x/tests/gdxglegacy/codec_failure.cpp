#include <objbase.h>
#include <windows.h>

namespace {
class Factory final : public IClassFactory {
public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **object) override {
    if (object)
      *object = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
  ULONG STDMETHODCALLTYPE Release() override { return 1; }
  HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown *, REFIID,
                                           void **object) override {
    if (object)
      *object = nullptr;
    return E_OUTOFMEMORY;
  }
  HRESULT STDMETHODCALLTYPE LockServer(BOOL) override { return S_OK; }
} factory;
} // namespace

extern "C" HRESULT WINAPI DllGetClassObject(REFCLSID, REFIID iid,
                                            void **object) {
  if (!object)
    return E_POINTER;
  *object = nullptr;
#ifdef FAIL_FACTORY
  return E_FAIL;
#else
  if (!IsEqualGUID(iid, IID_IClassFactory))
    return E_NOINTERFACE;
  *object = &factory;
  factory.AddRef();
  return S_OK;
#endif
}
