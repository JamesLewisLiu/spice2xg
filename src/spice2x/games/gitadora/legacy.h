#pragma once

#include <string>

namespace games::gitadora::legacy {
    // Selected explicitly before per-game configuration and input discovery.
    bool enabled();
    bool is_cn();
    const std::string &identity_path();
    void configure(const std::string &path, const std::string &bootstrap = "");
    // Called after AVS boot, before Patch Manager and game DLL load.
    void resolve_datecode();
    const std::string &ea3_config_path();

    // The native host replaces gdxg.exe, not any original game DLL.
    void load();
    bool entry_init(char *sid_code, void *app_param);
    void entry_main();
    void shutdown();

    // The original EXEs end the process after game_finalize. Once boot_main
    // runs, native modules and their AVS allocations must outlive host cleanup.
    bool has_process_lifetime_resources();

    // Requests from input/API threads are consumed by the native main loop.
    bool request_exit(bool restart);
    bool restart_requested();
}
