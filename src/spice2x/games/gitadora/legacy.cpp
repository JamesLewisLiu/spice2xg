#include "legacy.h"
#include "legacy_codec.h"
#include "legacy_movie.h"
#include "legacy_texture.h"

#include <atomic>
#include <map>
#include <mutex>
#include <new>
#include <string>

#include <windows.h>
#include <d3d9.h>
#include <imm.h>
#include <mmsystem.h>
#include <objbase.h>

#include "avs/game.h"
#include "gitadora.h"
#include "hooks/graphics/graphics.h"
#include "launcher/launcher.h"
#include "launcher/shutdown.h"
#include "util/libutils.h"
#include "util/logging.h"

namespace games::gitadora::legacy {
    namespace {
        // The two original gdxg.exe hosts share these private libvmrsvr COM
        // interfaces. Their GUIDs and slot signatures were checked against both
        // EXEs and the server; no original code address is called here.
        constexpr GUID IID_WIZARD = {0x6d402155, 0xd941, 0x478a,
                {0xbe, 0xbb, 0xaf, 0x36, 0x1f, 0x63, 0x38, 0x51}};
        constexpr GUID IID_RENDER_ENGINE = {0x6e474394, 0x49fd, 0x416f,
                {0xae, 0xd4, 0x0e, 0xda, 0xd1, 0x87, 0x9d, 0xc7}};
        constexpr GUID IID_MIXER_CONTROL = {0x6ee46528, 0x3c31, 0x43af,
                {0xb8, 0xb6, 0x1c, 0x7a, 0x53, 0xa2, 0xe5, 0xc2}};
        constexpr GUID CLSID_WIZARD = {0x8d372f4d, 0xe0c0, 0x464f,
                {0xbd, 0xa9, 0x96, 0x39, 0x24, 0x89, 0x0a, 0x35}};
        constexpr GUID CLSID_RENDER_ENGINE = {0x9401081e, 0x848a, 0x4da7,
                {0xb6, 0xc1, 0x98, 0x28, 0xbb, 0x49, 0x3e, 0x4f}};
        constexpr HRESULT VMR_NOT_FOUND = static_cast<HRESULT>(0x80040216);
        constexpr HRESULT VMR_WRONG_STATE = static_cast<HRESULT>(0x80040227);
        constexpr char WINDOW_CLASS[] = "gfdm";

        struct NativeExports {
            void (__cdecl *boot_main)(HWND) = nullptr;
            int (__cdecl *boot_step)() = nullptr;
            int (__cdecl *game_initialize)(HWND) = nullptr;
            int (__cdecl *game_mainloop)(HWND) = nullptr;
            int (__cdecl *game_finalize)(HWND) = nullptr;
            void (__cdecl *debug_memory_initialize)() = nullptr;
            void (__cdecl *window_initialize)(HINSTANCE, HWND) = nullptr;
            void (__cdecl *gdbase_cmdline)(char *) = nullptr;
            void (__cdecl *debug_cmdline)(char *) = nullptr;
            void (__cdecl *code_cmdline)(char *) = nullptr;
            void (__cdecl *keys_initialize)() = nullptr;
            void (__cdecl *keys_finalize)() = nullptr;
            void (__cdecl *keys_update)() = nullptr;
            void (__cdecl *key_down)(unsigned int, long) = nullptr;
            void (__cdecl *key_up)(unsigned int, long) = nullptr;
            void (__cdecl *set_device)(IDirect3DDevice9 *) = nullptr;
            void (__cdecl *movie_set_engine)(IUnknown *) = nullptr;
            HRESULT (STDAPICALLTYPE *get_class_object)(REFCLSID, REFIID, void **) = nullptr;
        } native;

        template<size_t Slot, typename... Args>
        HRESULT vmr_call(IUnknown *object, Args... args) {
            auto table = *reinterpret_cast<void ***>(object);
            using Function = HRESULT (STDMETHODCALLTYPE *)(IUnknown *, Args...);
            return reinterpret_cast<Function>(table[Slot])(object, args...);
        }

        template<typename T>
        void release(T *&object) {
            if (object) {
                object->Release();
                object = nullptr;
            }
        }

        // Exactly twelve COM slots, matching the host's IVMRSvrMixerControl.
        // In the original EXE Render updates the game's device pointer and
        // validates state. Scene drawing remains in the original game DLLs.
        struct MixerInterface : IUnknown {
            virtual HRESULT STDMETHODCALLTYPE SetRenderEngine(IUnknown *) = 0;
            virtual HRESULT STDMETHODCALLTYPE GetRenderEngine(IUnknown **) = 0;
            virtual HRESULT STDMETHODCALLTYPE Initialize(IUnknown *) = 0;
            virtual HRESULT STDMETHODCALLTYPE Terminate() = 0;
            virtual HRESULT STDMETHODCALLTYPE InitializeDevice(IUnknown *) = 0;
            virtual HRESULT STDMETHODCALLTYPE Compose(void *) = 0;
            virtual HRESULT STDMETHODCALLTYPE Render(IDirect3DDevice9 *, void *) = 0;
            virtual HRESULT STDMETHODCALLTYPE AddStream(DWORD, DWORD, DWORD, DWORD, DWORD) = 0;
            virtual HRESULT STDMETHODCALLTYPE RemoveStream(DWORD) = 0;
        };

        class MixerControl final : public MixerInterface {
            struct Stream {
                DWORD width, height, texture_width, texture_height;
            };
            std::atomic<ULONG> references{1};
            std::recursive_mutex lock;
            IUnknown *engine = nullptr;
            bool initialized = false;
            std::map<DWORD, Stream> streams;

            ~MixerControl() {
                release(engine);
            }

        public:
            HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **output) override {
                if (!output) {
                    return E_POINTER;
                }
                *output = nullptr;
                if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_MIXER_CONTROL)) {
                    *output = static_cast<MixerInterface *>(this);
                    AddRef();
                    return S_OK;
                }
                return E_NOINTERFACE;
            }

            ULONG STDMETHODCALLTYPE AddRef() override {
                return ++references;
            }

            ULONG STDMETHODCALLTYPE Release() override {
                ULONG remaining = --references;
                if (!remaining) {
                    delete this;
                }
                return remaining;
            }

            HRESULT STDMETHODCALLTYPE SetRenderEngine(IUnknown *value) override {
                std::lock_guard guard(lock);
                if (value) {
                    value->AddRef();
                }
                release(engine);
                engine = value;
                return S_OK;
            }

            HRESULT STDMETHODCALLTYPE GetRenderEngine(IUnknown **output) override {
                if (!output) {
                    return E_POINTER;
                }
                std::lock_guard guard(lock);
                *output = engine;
                if (!engine) {
                    return VMR_NOT_FOUND;
                }
                engine->AddRef();
                return S_OK;
            }

            HRESULT STDMETHODCALLTYPE Initialize(IUnknown *device) override {
                std::lock_guard guard(lock);
                if (initialized) {
                    return VMR_WRONG_STATE;
                }
                if (!device) {
                    return E_POINTER;
                }
                initialized = true;
                return S_OK;
            }

            HRESULT STDMETHODCALLTYPE Terminate() override {
                std::lock_guard guard(lock);
                initialized = false;
                return S_OK;
            }

            HRESULT STDMETHODCALLTYPE InitializeDevice(IUnknown *device) override {
                if (!device) {
                    return E_POINTER;
                }
                std::lock_guard guard(lock);
                initialized = false;
                return Initialize(device);
            }

            HRESULT STDMETHODCALLTYPE Compose(void *) override {
                std::lock_guard guard(lock);
                return initialized ? S_OK : VMR_WRONG_STATE;
            }

            HRESULT STDMETHODCALLTYPE Render(IDirect3DDevice9 *device, void *) override {
                native.set_device(device);
                if (!device) {
                    return E_POINTER;
                }
                std::lock_guard guard(lock);
                if (!initialized) {
                    return VMR_WRONG_STATE;
                }
                return engine ? S_OK : E_UNEXPECTED;
            }

            HRESULT STDMETHODCALLTYPE AddStream(DWORD id, DWORD width, DWORD height,
                    DWORD texture_width, DWORD texture_height) override {
                std::lock_guard guard(lock);
                try {
                    streams.insert_or_assign(id, Stream{width, height, texture_width, texture_height});
                    return S_OK;
                } catch (const std::bad_alloc &) {
                    return E_OUTOFMEMORY;
                }
            }

            HRESULT STDMETHODCALLTYPE RemoveStream(DWORD id) override {
                std::lock_guard guard(lock);
                return streams.erase(id) ? S_OK : VMR_NOT_FOUND;
            }
        };

        struct Host {
            HINSTANCE instance = nullptr;
            HWND window = nullptr;
            IUnknown *wizard = nullptr;
            IUnknown *initial_engine = nullptr;
            IUnknown *engine = nullptr;
            IUnknown *engine_control = nullptr;
            IDirect3DDevice9 *device = nullptr;
            MixerControl *mixer = nullptr;
            std::string command_line;
            bool loaded = false;
            bool initialized = false;
            bool com_initialized = false;
            bool wizard_initialized = false;
            bool engine_initialized = false;
            bool window_registered = false;
            bool keys_initialized = false;
            bool timer_initialized = false;
            std::atomic<bool> process_lifetime_resources{false};
            bool game_started = false;
            bool quit = false;
            bool shutting_down = false;
        } host;
        std::atomic<bool> exit_requests_enabled{false};
        std::atomic<unsigned> exit_request{0}; // 0: none, 1: exit, 2: restart
        std::atomic<HWND> active_window{nullptr};

        bool check(HRESULT result, const char *operation) {
            if (SUCCEEDED(result)) {
                return true;
            }
            log_warning("gdxg-legacy", "{} failed: HRESULT {:08x}", operation,
                    static_cast<unsigned long>(result));
            return false;
        }

        HRESULT create_vmr(REFCLSID clsid, REFIID iid, IUnknown **output) {
            IClassFactory *factory = nullptr;
            HRESULT result = native.get_class_object(clsid, IID_IClassFactory,
                    reinterpret_cast<void **>(&factory));
            if (SUCCEEDED(result)) {
                result = factory->CreateInstance(nullptr, iid, reinterpret_cast<void **>(output));
                factory->Release();
            }
            return result;
        }

        LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
            switch (message) {
                case WM_KEYDOWN:
                    if (host.keys_initialized) {
                        native.key_down(static_cast<unsigned int>(wparam), static_cast<long>(lparam));
                    }
                    if (wparam == VK_ESCAPE) {
                        PostMessage(window, WM_CLOSE, 0, 0);
                    }
                    break;
                case WM_KEYUP:
                    if (host.keys_initialized) {
                        native.key_up(static_cast<unsigned int>(wparam), static_cast<long>(lparam));
                    }
                    break;
                case WM_CLOSE:
                    if (host.process_lifetime_resources.load(std::memory_order_acquire)) {
                        // A native frame can run its own message loop. Original
                        // WM_CLOSE ends the process rather than awaiting WinMain.
                        launcher::shutdown(EXIT_SUCCESS);
                        return 0;
                    }
                    host.quit = true;
                    return 0;
                case WM_DESTROY:
                    host.quit = true;
                    return 0;
            }
            return DefWindowProcA(window, message, wparam, lparam);
        }

        bool create_window() {
            host.instance = GetModuleHandle(nullptr);
            WNDCLASSEXA wc{};
            wc.cbSize = sizeof(wc);
            wc.style = CS_OWNDC | CS_BYTEALIGNCLIENT | CS_BYTEALIGNWINDOW;
            wc.lpfnWndProc = window_proc;
            wc.hInstance = host.instance;
            wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW);
            wc.lpszClassName = WINDOW_CLASS;
            if (!RegisterClassExA(&wc)) {
                log_warning("gdxg-legacy", "RegisterClassEx failed: {}", GetLastError());
                return false;
            }
            host.window_registered = true;
            DWORD style = GRAPHICS_WINDOWED ? WS_OVERLAPPEDWINDOW : WS_POPUP;
            RECT area{0, 0, 1280, 720};
            AdjustWindowRect(&area, style, FALSE);
            host.window = CreateWindowExA(0, WINDOW_CLASS, "GFDM", style,
                    0, 0, area.right - area.left, area.bottom - area.top,
                    nullptr, nullptr, host.instance, nullptr);
            if (!host.window) {
                log_warning("gdxg-legacy", "CreateWindowEx failed: {}", GetLastError());
                return false;
            }
            active_window.store(host.window, std::memory_order_release);
            ImmAssociateContext(host.window, nullptr);
            graphics_capture_initial_window(host.window);
            native.keys_initialize();
            host.keys_initialized = true;
            return true;
        }

        bool pump_messages() {
            if (exit_request.load(std::memory_order_acquire) != 0) {
                host.quit = true;
            }
            MSG message{};
            while (PeekMessageA(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) {
                    host.quit = true;
                    break;
                }
                TranslateMessage(&message);
                DispatchMessageA(&message);
            }
            if (exit_request.load(std::memory_order_acquire) != 0) {
                host.quit = true;
            }
            return !host.quit;
        }
    }

    void load() {
        if (host.loaded) {
            return;
        }
        auto boot = libutils::load_library(MODULE_PATH / "boot.dll");
        auto game = libutils::load_library(MODULE_PATH / "game.dll");
        auto vmr = libutils::load_library(MODULE_PATH / "libvmrsvr.dll");
        auto system = libutils::get_module("libsystem.dll");
        auto base = libutils::get_module("libgdbase.dll");
        auto tools = libutils::get_module("libgftools.dll");
        auto movie = libutils::get_module("libmovie.dll");
        // Load the dependency graph before Spice's per-module device hooks.
        avs::game::DLL_INSTANCE = game;
        native.boot_main = libutils::get_proc<decltype(native.boot_main)>(boot, "?boot_main@@YAXPAUHWND__@@@Z");
        native.boot_step = libutils::get_proc<decltype(native.boot_step)>(boot, "?boot_step@@YAHXZ");
        native.game_initialize = libutils::get_proc<decltype(native.game_initialize)>(game, "?game_initialize@@YAHPAUHWND__@@@Z");
        native.game_mainloop = libutils::get_proc<decltype(native.game_mainloop)>(game, "?game_mainloop@@YAHPAUHWND__@@@Z");
        native.game_finalize = libutils::get_proc<decltype(native.game_finalize)>(game, "?game_finalize@@YAHPAUHWND__@@@Z");
        native.debug_memory_initialize = libutils::get_proc<decltype(native.debug_memory_initialize)>(system, "sys_debug_mem_initialize");
        native.window_initialize = libutils::get_proc<decltype(native.window_initialize)>(system, "sys_window_initialize");
        native.debug_cmdline = libutils::get_proc<decltype(native.debug_cmdline)>(system, "sys_debug_set_cmdline");
        native.code_cmdline = libutils::get_proc<decltype(native.code_cmdline)>(system, "sys_code_set_cmdline");
        native.gdbase_cmdline = libutils::get_proc<decltype(native.gdbase_cmdline)>(base, "?gdbase_set_cmdline@@YAXQAD@Z");
        native.keys_initialize = libutils::get_proc<decltype(native.keys_initialize)>(system, "?sys_input_dbgkey_initialize@@YAXXZ");
        native.keys_finalize = libutils::get_proc<decltype(native.keys_finalize)>(system, "?sys_input_dbgkey_finalize@@YAXXZ");
        native.keys_update = libutils::get_proc<decltype(native.keys_update)>(system, "?sys_input_dbgkey_update@@YAXXZ");
        native.key_down = libutils::get_proc<decltype(native.key_down)>(system, "?sys_input_dbgkey_keydown@@YAXIJ@Z");
        native.key_up = libutils::get_proc<decltype(native.key_up)>(system, "?sys_input_dbgkey_keyup@@YAXIJ@Z");
        native.set_device = libutils::get_proc<decltype(native.set_device)>(tools, "?SetD3DDevice@@YAXPAUIDirect3DDevice9@@@Z");
        native.movie_set_engine = libutils::get_proc<decltype(native.movie_set_engine)>(movie, "?movie_set_render_engine@@YAXPAUIVMRSvrRenderEngine@@@Z");
        native.get_class_object = libutils::get_proc<decltype(native.get_class_object)>(vmr, "DllGetClassObject");
        host.loaded = true;
    }

    bool entry_init(char *, void *) {
        if (host.initialized) {
            return true;
        }
        exit_requests_enabled.store(true, std::memory_order_release);
        load();
        // The launcher's global audio COM hook runs after load(). Install the
        // narrow movie hook here so it chains to audio instead of being replaced
        // by the global IAT sweep before the first native movie request.
        codec::attach(libutils::get_module("libmovie.dll"));
        movie::attach(libutils::get_module("libmovie.dll"), avs::game::DLL_INSTANCE);
        texture::attach(libutils::get_module("libgftools.dll"));
        host.quit = false;
        host.timer_initialized = timeBeginPeriod(1) == TIMERR_NOERROR;
        native.debug_memory_initialize();
        host.command_line = "gdxg.exe -I \"" + identity_path() + "\"";
        host.command_line += is_drum() ? " -d" : " -g";
        native.gdbase_cmdline(host.command_line.data());
        native.debug_cmdline(host.command_line.data());
        native.code_cmdline(host.command_line.data());

        // COINIT_SPEED_OVER_MEMORY with the original multithreaded apartment.
        HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_SPEED_OVER_MEMORY);
        if (!check(result, "CoInitializeEx")) {
            shutdown();
            return false;
        }
        host.com_initialized = true;
        host.mixer = new (std::nothrow) MixerControl();
        bool success = host.mixer && create_window()
                && check(create_vmr(CLSID_WIZARD, IID_WIZARD, &host.wizard), "create VMR wizard")
                && check(create_vmr(CLSID_RENDER_ENGINE, IID_RENDER_ENGINE,
                        &host.initial_engine), "create VMR render engine");
        if (success) {
            success = check(vmr_call<3>(host.initial_engine, host.window, DWORD(0),
                    static_cast<MixerInterface *>(host.mixer)), "initialize VMR render engine");
            host.engine_initialized = success;
        }
        if (success) {
            success = check(vmr_call<3>(host.wizard, DWORD(0), host.window, host.initial_engine),
                    "initialize VMR wizard");
            host.wizard_initialized = success;
        }
        if (success) {
            success = check(vmr_call<13>(host.wizard, &host.engine), "get VMR render engine")
                    && host.engine
                    && check(vmr_call<10>(host.engine, &host.engine_control), "get VMR mixer control")
                    && check(vmr_call<11>(host.engine, &host.device), "get VMR D3D9 device")
                    && host.device;
        }
        if (!success) {
            shutdown();
            return false;
        }
        native.set_device(host.device);
        native.window_initialize(host.instance, host.window);
        ShowWindow(host.window, SW_SHOWNORMAL);
        SetForegroundWindow(host.window);
        host.initialized = true;
        log_info("gdxg-legacy", "Native {} {} host initialized (1280x720)",
                is_cn() ? "XG1CN" : "XG1", is_drum() ? "DM" : "GF");
        return true;
    }

    void entry_main() {
        if (!host.initialized) {
            log_fatal("gdxg-legacy", "Native frontend entered without initialization");
        }
        if (!pump_messages()) {
            shutdown();
            return;
        }
        // Spice owns AVS. In particular, never call boot_avs or
        // boot_avs_terminate: both original versions really start/stop AVS.
        host.process_lifetime_resources.store(true, std::memory_order_release);
        log_info("gdxg-legacy", "calling native boot_main");
        native.boot_main(host.window);
        log_info("gdxg-legacy", "native boot_main returned");
        int boot_result = 1;
        while (pump_messages() && (boot_result = native.boot_step()) != 0) {
        }
        log_info("gdxg-legacy", "native boot loop finished (result={}, quit={})", boot_result, host.quit);
        if (!host.quit) {
            host.game_started = true;
            auto result = native.game_initialize(host.window);
            log_info("gdxg-legacy", "native game_initialize returned {}", result);
            native.movie_set_engine(host.engine);
            while (pump_messages()) {
                native.keys_update();
                native.game_mainloop(host.window);
            }
        }
        shutdown();
    }

    bool request_exit(bool restart) {
        if (!exit_requests_enabled.load(std::memory_order_acquire)) {
            return false;
        }
        bool first_request;
        if (restart) {
            first_request = exit_request.exchange(2, std::memory_order_acq_rel) == 0;
        } else {
            unsigned expected = 0;
            first_request = exit_request.compare_exchange_strong(expected, 1, std::memory_order_acq_rel);
        }
        if (first_request) {
            if (auto window = active_window.load(std::memory_order_acquire)) {
                PostMessageA(window, WM_CLOSE, 0, 0);
            }
        }
        // Keep accepting requests after native cleanup while the launcher releases
        // its own devices and AVS. A repeated hotkey must not force-kill that work.
        return true;
    }

    bool restart_requested() {
        return exit_request.load(std::memory_order_acquire) == 2;
    }

    bool has_process_lifetime_resources() {
        return host.process_lifetime_resources.load(std::memory_order_acquire);
    }

    void shutdown() {
        if (host.shutting_down) {
            return;
        }
        host.shutting_down = true;
        host.quit = true;
        if (host.game_started) {
            host.game_started = false;
            log_info("gdxg-legacy", "calling native game_finalize");
            native.game_finalize(host.window);
        }
        if (host.process_lifetime_resources.load(std::memory_order_acquire)) {
            // Both original WinMain implementations call exit after game_finalize;
            // neither tears down boot/modules/AVS. boot_terminate is unsafe for
            // partially initialized movie playback, and game_finalize leaves AVS
            // allocations live even after a fully initialized game. Keep those
            // modules, heaps and renderer together until the final process exit.
            // The launcher still releases Spice's own subsystems before that.
            log_info("gdxg-legacy", "native modules retained until process exit");
            return;
        }
        if (host.wizard_initialized) {
            host.wizard_initialized = false;
            check(vmr_call<4>(host.wizard), "terminate VMR wizard");
        } else if (host.engine_initialized) {
            check(vmr_call<4>(host.initial_engine), "terminate VMR render engine");
        }
        host.engine_initialized = false;
        if (host.mixer) {
            host.mixer->SetRenderEngine(nullptr);
        }
        release(host.device);
        release(host.engine_control);
        release(host.engine);
        release(host.initial_engine);
        release(host.wizard);
        release(host.mixer);
        if (host.keys_initialized) {
            host.keys_initialized = false;
            native.keys_finalize();
        }
        if (host.window) {
            active_window.store(nullptr, std::memory_order_release);
            DestroyWindow(host.window);
            host.window = nullptr;
        }
        if (host.window_registered) {
            UnregisterClassA(WINDOW_CLASS, host.instance);
            host.window_registered = false;
        }
        if (host.com_initialized) {
            CoUninitialize();
            host.com_initialized = false;
        }
        if (host.timer_initialized) {
            timeEndPeriod(1);
            host.timer_initialized = false;
        }
        host.initialized = false;
        host.shutting_down = false;
    }
}
