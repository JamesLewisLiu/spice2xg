// included early to avoid warning
#include <winsock2.h>
#include <atomic>

#include "shutdown.h"

#include "api/controller.h"
#include "easrv/easrv.h"
#include "games/gitadora/legacy.h"
#include "rawinput/rawinput.h"
#include "hooks/audio/audio.h"
#include "hooks/graphics/graphics.h"
#include "hooks/graphics/backends/d3d11/d3d11_backend.h"
#include "util/deferlog.h"
#include "util/logging.h"

#include "launcher.h"
#include "logger.h"
#include "nvapi/nvapi.h"
#include "sdk/sdk.h"

namespace launcher {

    static void restart_spawn();
    static std::atomic_flag legacy_process_stopping = ATOMIC_FLAG_INIT;
    static std::atomic<bool> legacy_process_restart{false};

    static bool stop_legacy_process(bool restart) {
        if (!games::gitadora::legacy::has_process_lifetime_resources()) {
            return false;
        }
        if (restart) {
            legacy_process_restart.store(true, std::memory_order_release);
        }
        if (legacy_process_stopping.test_and_set(std::memory_order_acq_rel)) {
            return true;
        }
        // The native loop may be waiting inside a game/renderer call. As in
        // the original WM_CLOSE -> exit path, do not require another frame to
        // return before exiting. Retain native objects while Spice performs
        // the same subsystem cleanup used by its normal Exit Game path.
        log_info("gdxg-legacy", "native process exit: retaining AVS until process exit");
        stop_subsystems();
        if (legacy_process_restart.load(std::memory_order_acquire)) {
            restart_spawn();
        }
        // Match Spice's normal process exit after subsystem cleanup. Native
        // DLL destructors cannot safely require a complete game/AVS teardown.
        kill(EXIT_SUCCESS);
        return true;
    }

    void stop_subsystems() {
        // note that it is possible for stop_subsystems to be called multiple times
        // (e.g., crashing, and then closing the window)
        // therefore, subsystems need to be guarded against multiple unload attempts
        log_info("launcher", "stopping subsystems");

        sdk::fini_sdk_modules();

        // stop dx11 background workers (poll thread, LDR notification)
        // before anything else, so they can't race against the teardown.
        graphics_d3d11_shutdown();

        // reset monitor settings
        reset_monitor_on_exit();

        // before shutting down logger, dump any deferred log messages
        deferredlogs::dump_to_logger();

        // flush/stop logger
        logger::stop();

        // stop ea server
        easrv_shutdown();

        // free api sockets
        if (API_CONTROLLER) {
            API_CONTROLLER->free_socket();
        }

        // notify audio hook
        hooks::audio::stop();

        // stop raw input
        if (RI_MGR) {
            RI_MGR->stop();
        }

        // unload nvapi and free library (if loaded)
        nvapi::unload();
    }

    void kill(UINT exit_code) {

        // terminate
        TerminateProcess(GetCurrentProcess(), exit_code);
    }

    void shutdown(UINT exit_code) {
        if (exit_code == EXIT_SUCCESS && stop_legacy_process(false)) {
            return;
        }
        if (exit_code == EXIT_SUCCESS && games::gitadora::legacy::request_exit(false)) {
            return;
        }

        // force exit after 1s
        std::thread force_thread([exit_code] {
            Sleep(1000);
            log_info("launcher", "force shutdown");
            kill(exit_code);
            return nullptr;
        });
        force_thread.detach();

        // stop all subsystems
        stop_subsystems();

        // terminate
        kill(exit_code);
    }

    static void restart_spawn() {

        // never do this twice
        static bool already_done = false;
        if (already_done) {
            return;
        } else {
            already_done = true;
        }

        if (games::gitadora::legacy::enabled()) {
            // The CRT system("START ...") path can wait for the replacement
            // while retaining the native process. Spawn directly, preserving
            // Windows quoting, Unicode arguments, environment and working dir.
            std::wstring application(32768, L'\0');
            auto length = GetModuleFileNameW(nullptr, application.data(), application.size());
            if (length == 0 || length >= application.size()) {
                log_warning("launcher", "unable to resolve native restart executable: {}", GetLastError());
                return;
            }
            application.resize(length);
            std::wstring command_line = GetCommandLineW();
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            PROCESS_INFORMATION process{};
            if (CreateProcessW(application.c_str(), command_line.data(), nullptr, nullptr,
                    FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
                CloseHandle(process.hThread);
                CloseHandle(process.hProcess);
            } else {
                log_warning("launcher", "unable to restart native process: {}", GetLastError());
            }
            return;
        }

        // start the process using the same args
        if (LAUNCHER_ARGC > 0) {

            // build cmd line
            std::string cmd_line = "START \"\" ";
            for (int i = 0; i < LAUNCHER_ARGC; i++)
                cmd_line += " \"" + std::string(LAUNCHER_ARGV[i]) + "\"";

            // run command
            system(cmd_line.c_str());
        }
    }

    void finish_legacy_shutdown() {
        if (games::gitadora::legacy::restart_requested()) {
            restart_spawn();
        }
        if (games::gitadora::legacy::has_process_lifetime_resources()) {
            // Spice has already detached its devices, joined its workers and
            // flushed/stopped logging. Retained native resources are reclaimed
            // together by the OS, as on Spice's normal Exit Game path.
            kill(EXIT_SUCCESS);
        }
    }

    void restart() {
        if (stop_legacy_process(true)) {
            return;
        }
        if (games::gitadora::legacy::request_exit(true)) {
            return;
        }

        // force restart after 1s
        std::thread force_thread([] {
            Sleep(1000);
            log_info("launcher", "force restart");
            restart_spawn();
            launcher::kill(0);
            return nullptr;
        });
        force_thread.detach();

        // clean up before restart so resources can be reclaimed
        stop_subsystems();

        // spawn new and terminate this process
        restart_spawn();
        launcher::kill(0);
    }
}
