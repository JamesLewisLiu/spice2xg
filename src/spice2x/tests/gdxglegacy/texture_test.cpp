// Optional integration probe: texture_test <absolute libgftools.dll> [hook].
// Uses the real regional texture implementation and an actual D3D9 device.
// No game, AVS boot, resources, configuration or COM registration is needed.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>
#include <d3d9.h>
#include "games/gitadora/legacy_texture.h"
#include "util/detour.h"
#include "util/logging.h"

std::string_view log_get_datetime() { return "test"; }
std::string_view log_get_datetime(std::time_t) { return "test"; }
void logger::push(std::string data, logger::Style, bool) { std::fputs(data.c_str(), stdout); }
void show_popup_for_fatal_error(std::string) { std::abort(); }
void launcher::stop_subsystems() { std::abort(); }
void launcher::kill(UINT) { std::abort(); }
std::wstring s2ws(const std::string &) { std::abort(); }
std::string ws2s(const std::wstring &) { std::abort(); }
void require(bool ok, const char *message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s (%lu)\n", message, GetLastError()); std::exit(1); }
}
template<typename T> T proc(HMODULE module, const char *name) {
    auto p = GetProcAddress(module, name); require(p, name); return reinterpret_cast<T>(p);
}
uint8_t expand(uint8_t nibble) { return nibble ? (nibble << 4) | 15 : 0; }
int main(int argc, char **argv) {
    require(argc >= 2, "supply libgftools.dll path");
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    auto path = std::filesystem::absolute(argv[1]);
    SetDllDirectoryW(path.parent_path().c_str());
    for (auto name : {L"libavs-win32.dll", L"libdevice.dll", L"libpackfs.dll", L"d3dx9_31.dll"}) {
        auto dep = LoadLibraryExW((path.parent_path()/name).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        std::printf("DEPENDENCY %ls=%p error=%lu\n", name, dep, dep ? 0 : GetLastError());
    }
    auto module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    require(module, "LoadLibrary libgftools");
    // Substitute only the unbooted AVS allocator/memory imports for this probe.
    require(detour::iat_try("mem_set", reinterpret_cast<void *>(std::memset), module), "mem_set import");
    require(detour::iat_try("mem_cpy", reinterpret_cast<void *>(std::memcpy), module), "mem_cpy import");
    require(detour::iat_try("std_malloc", reinterpret_cast<void *>(std::malloc), module), "malloc import");
    require(detour::iat_try("std_free", reinterpret_cast<void *>(std::free), module), "free import");
    auto window = CreateWindowW(L"STATIC", L"XG1 texture probe", WS_OVERLAPPEDWINDOW,
                                 0, 0, 1280, 720, nullptr, nullptr, nullptr, nullptr);
    require(window, "hidden window");
    auto d3d = Direct3DCreate9(D3D_SDK_VERSION); require(d3d, "Direct3DCreate9");
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = window;
    pp.BackBufferWidth = 1280; pp.BackBufferHeight = 720; pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.EnableAutoDepthStencil = TRUE; pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    IDirect3DDevice9 *device = nullptr;
    require(SUCCEEDED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device)), "CreateDevice");
    proc<void (__cdecl *)(IDirect3DDevice9 *)>(module, "?SetD3DDevice@@YAXPAUIDirect3DDevice9@@@Z")(device);
    require(proc<int (__cdecl *)()>(module, "gfdraw_initialize")() == 0, "gfdraw_initialize");
    bool hook = argc >= 3;
    if (hook) require(games::gitadora::legacy::texture::attach(module), "production hook install");
    auto gen = proc<void (__cdecl *)(int, unsigned *)>(module, "gfGenTextures");
    auto bind = proc<uintptr_t (__cdecl *)(unsigned)>(module, "gfBindTexture");
    auto create = proc<uintptr_t (__cdecl *)(int,int,int,int,int,const void *)>(module, "gfCreateTexture");
    auto get = proc<IDirect3DTexture9 *(__cdecl *)(unsigned)>(module, "gfGetTexture");
    auto remove = proc<void (__cdecl *)(int,unsigned *)>(module, "gfDeleteTextures");
    unsigned missing = 0, tested = 0;
    for (auto dimensions : {std::pair{512,512}, {276,632}, {512,1024}, {1024,256}, {16,16}}) {
        const auto [width,height] = dimensions;
        std::vector<uint8_t> source(size_t(width)*height);
        for (size_t i=0; i<source.size(); ++i) source[i] = uint8_t(i);
        const auto original = source;
        unsigned id=0; gen(1,&id); bind(id);
        create(0x8043,width,height,0,int(source.size()),source.data());
        auto tex = get(id);
        require(source==original, "input preserved");
        if (!tex) { ++missing; std::printf("MISSING %dx%d\n",width,height); remove(1,&id); continue; }
        D3DSURFACE_DESC desc{}; require(SUCCEEDED(tex->GetLevelDesc(0,&desc)), "texture desc");
        D3DLOCKED_RECT lock{}; require(SUCCEEDED(tex->LockRect(0,&lock,nullptr,D3DLOCK_READONLY)), "read pixels");
        std::printf("TEXTURE %dx%d format=%u pitch=%d\n",width,height,unsigned(desc.Format),lock.Pitch);
        require(desc.Width==unsigned(width) && desc.Height==unsigned(height), "dimensions unchanged");
        require(desc.Format==D3DFMT_A8L8, "expanded format A8L8");
        size_t mismatch=0;
        for (int y=0;y<height;++y) for(int x=0;x<width;++x) {
            auto *p=static_cast<uint8_t *>(lock.pBits)+y*lock.Pitch+2*x;
            auto s=source[size_t(y)*width+x];
            mismatch += p[0]!=expand(s>>4) || p[1]!=expand(s&15);
        }
        tex->UnlockRect(0);
        std::printf("MISMATCH=%zu\n",mismatch);
        require(mismatch==0,"all luminance/alpha pixels match CN conversion");
        ++tested; remove(1,&id);
    }
    require(!hook || missing==0, "all converted textures created");
    std::printf("PASS tested=%u missing=%u hook=%u\n", tested,missing,hook);
    device->SetTexture(0,nullptr); device->Release(); d3d->Release(); DestroyWindow(window);
    return 0;
}
