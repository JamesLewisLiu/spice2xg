#include "ea3.h"

#include <optional>

#include "build/defs.h"
#include "easrv/easrv.h"
#include "easrv/smartea.h"
#include "games/gitadora/gitadora.h"
#include "games/gitadora/legacy.h"
#include "hooks/avshook.h"
#include "hooks/graphics/graphics.h"
#include "util/detour.h"
#include "util/deferlog.h"
#include "util/fileutils.h"
#include "util/libutils.h"
#include "util/logging.h"
#include "util/utils.h"
#include "launcher/logger.h"

#include "core.h"
#include "game.h"

namespace avs {

    typedef void (*ssl_protocol_init_t)();
    typedef void (*ssl_protocol_fini_t)();
    static ssl_protocol_init_t ssl_protocol_init = nullptr;
    static ssl_protocol_fini_t ssl_protocol_fini = nullptr;

    // functions
    AVS_EA3_BOOT_STARTUP_T avs_ea3_boot_startup;
    AVS_EA3_SHUTDOWN_T avs_ea3_shutdown;

    namespace ea3 {

        // settings
        std::string CFG_PATH;
        std::string APP_PATH;
        std::string BOOTSTRAP_PATH;
        std::string PCBID_CUSTOM = "";
        std::string SOFTID_CUSTOM = "";
        std::string ACCOUNTID_CUSTOM = "";
        std::string URL_CUSTOM = "";
        int HTTP11 = -1;
        int URL_SLASH = -1;
        int PCB_TYPE = -1;
        bool EA3_DEBUG_VERBOSE = false;

        // handle
        std::string VERSION_STR = "unknown";
        HINSTANCE DLL_INSTANCE = nullptr;
        std::string DLL_NAME = "";
        std::string EA3_BOOT_URL = "";
        std::string EA3_BOOT_PCBID = "";
        std::string EA3_BOOT_ACCOUNTID = "";

        // static fields
        static constexpr struct avs_ea3_import IMPORT_LEGACY {
            .version  = "legacy",
            .boot     = "ea3_boot",
            .shutdown = "ea3_shutdown",
        };
        static constexpr struct avs_ea3_import IMPORT_AVS21360 {
            .version  = "2.13.6.0",
            .boot     = "XEb552d500005d",
            .shutdown = "XEb552d5000060",
        };
        static constexpr struct avs_ea3_import IMPORT_AVS21430 {
            .version  = "2.14.3.0",
            .boot     = "XE7aee11000070",
            .shutdown = "XE7aee11000074",
        };
        static constexpr struct avs_ea3_import IMPORT_AVS21580 {
            .version  = "2.15.8.0",
            .boot     = "XE592acd00008c",
            .shutdown = "XE592acd00005a",
        };
        static constexpr struct avs_ea3_import IMPORT_AVS21610 {
            .version  = "2.16.1.0",
            .boot     = "XEyy2igh000006",
            .shutdown = "XEyy2igh000007",
        };
        static constexpr struct avs_ea3_import IMPORT_AVS21630 {
            .version  = "2.16.3.0",
            .boot     = "XEyy2igh000007",
            .shutdown = "XEyy2igh000008",
        };
        static constexpr struct avs_ea3_import IMPORT_AVS21651 {
            .version  = "2.16.5.1",
            .boot     = "XEyy2igh000007",
            .shutdown = "XEyy2igh000008",
        };
        static constexpr struct avs_ea3_import IMPORT_AVS21671 {
            .version  = "2.16.7.1",
            .boot     = "XEyy2igh000007",
            .shutdown = "XEyy2igh000008",
        };
        static constexpr struct avs_ea3_import IMPORT_AVS21681 {
            .version  = "2.16.8.1",
            .boot     = "XEyy2igh000007",
            .shutdown = "XEyy2igh000008",
        };
        static constexpr struct avs_ea3_import IMPORT_AVS21700 {
            .version  = "2.17.0.0",
            .boot     = "XEmdwapa000024",
            .shutdown = "XEmdwapa000025",
        };
        static constexpr struct avs_ea3_import IMPORT_AVS21730 {
            .version  = "2.17.3.0",
            .boot     = "XEmdwapa000024",
            .shutdown = "XEmdwapa000025",
        };
        static const struct avs_ea3_import IMPORTS[core::AVS_VERSION_COUNT] = {
            IMPORT_LEGACY,
            IMPORT_AVS21360,
            IMPORT_AVS21430,
            IMPORT_AVS21580,
            IMPORT_AVS21610,
            IMPORT_AVS21630,
            IMPORT_AVS21651,
            IMPORT_AVS21671,
            IMPORT_AVS21681,
            IMPORT_AVS21700,
            IMPORT_AVS21730,
        };


        static void set_string(core::property_ptr owner, const char *path, const char *value) {
            core::property_search_remove_safe(owner, nullptr, path);
            core::property_node_create(owner, nullptr, core::NODE_TYPE_str, path, value);
        }

        // Shared by bootstrap-owned games and XG1's game-owned EA3 config.
        static void apply_identity_overrides(core::property_ptr owner, const char *model) {
            if (!PCBID_CUSTOM.empty()) {
                set_string(owner, "/ea3/id/pcbid", PCBID_CUSTOM.substr(0, 20).c_str());
                if (ACCOUNTID_CUSTOM.empty() && strcmp(model, "M32") == 0) {
                    ACCOUNTID_CUSTOM = PCBID_CUSTOM;
                }
            }
            if (!SOFTID_CUSTOM.empty()) {
                set_string(owner, "/ea3/id/softid", SOFTID_CUSTOM.substr(0, 20).c_str());
            }
            if (!ACCOUNTID_CUSTOM.empty()) {
                set_string(owner, "/ea3/id/accountid", ACCOUNTID_CUSTOM.substr(0, 20).c_str());
            }
            if (!EA3_DEBUG_VERBOSE) {
                core::property_search_remove_safe(owner, nullptr, "/ea3/debug/verbose");
            }
        }

        static void apply_network_settings(core::property_ptr ea3_config,
                unsigned short easrv_port, bool easrv_maint, bool easrv_smart) {
            auto ea3_network = core::property_search_safe(ea3_config, nullptr, "/ea3/network");
            // http11
            if (HTTP11 >= 0) {
                avs::core::property_search_remove_safe(ea3_config, ea3_network, "http11");
                avs::core::property_node_create(ea3_config, ea3_network,
                        avs::core::NODE_TYPE_bool, "http11", HTTP11);
            }

            // url slash
            if (URL_SLASH >= 0) {
                avs::core::property_search_remove_safe(ea3_config, ea3_network, "url_slash");
                avs::core::property_node_create(ea3_config, ea3_network,
                        avs::core::NODE_TYPE_bool, "url_slash", URL_SLASH);
            }

            // custom service url
            if (!URL_CUSTOM.empty()) {
                avs::core::property_search_remove_safe(ea3_config, ea3_network, "services");
                avs::core::property_node_create(ea3_config, ea3_network,
                        avs::core::NODE_TYPE_str, "services", URL_CUSTOM.c_str());
            }

            // server - replace URL on the fly
            if (easrv_port != 0u) {
                std::ostringstream url;
                url << "http://localhost:" << easrv_port << "/";
                std::string url_str = url.str();

                avs::core::property_search_remove_safe(ea3_config, ea3_network, "services");
                avs::core::property_node_create(ea3_config, ea3_network,
                        avs::core::NODE_TYPE_str, "services", url_str.c_str());
            }

            // remember URL
            char url_buffer[512] {};
            avs::core::property_node_refer(ea3_config, nullptr, "/ea3/network/services",
                    avs::core::NODE_TYPE_str, url_buffer, sizeof(url_buffer));
            EA3_BOOT_URL = std::string(url_buffer);

            // ssl initialization
            if (string_begins_with(url_buffer, "https")) {

                // load ssl module
                HMODULE kws = libutils::try_library("kws.dll");
                if (kws != nullptr) {

                    // get functions
                    ssl_protocol_init = libutils::try_proc<ssl_protocol_init_t>(kws, "ssl_protocol_init");
                    ssl_protocol_fini = libutils::try_proc<ssl_protocol_fini_t>(kws, "ssl_protocol_fini");

                    // initialize
                    if (ssl_protocol_init != nullptr) {
                        log_info("avs-ea3", "initializing SSL protocol handler");
                        ssl_protocol_init();
                    }
                }
            }

            // smartea logic: check if services are dead
            if (easrv_smart && !smartea::check_url(EA3_BOOT_URL)) {
                log_info("avs-ea3", "starting smartea local server on port 8080");

                // start server
                easrv_start(8080, easrv_maint, 4, 8);

                // fix URL
                EA3_BOOT_URL = "http://localhost:8080";
                avs::core::property_search_remove_safe(ea3_config, ea3_network, "services");
                avs::core::property_node_create(ea3_config, ea3_network,
                        avs::core::NODE_TYPE_str, "services", EA3_BOOT_URL.c_str());

                // fix URL slash
                URL_SLASH = 1;
                avs::core::property_search_remove_safe(ea3_config, ea3_network, "url_slash");
                avs::core::property_node_create(ea3_config, ea3_network,
                        avs::core::NODE_TYPE_bool, "url_slash", &URL_SLASH);
            }

        }


        static thread_local struct {
            core::property_ptr owner = nullptr;
            core::node_ptr node = nullptr;
        } legacy_ea3_config;
        static core::PROPERTY_SEARCH_T legacy_property_search;
        static core::PROPERTY_DESTROY_T legacy_property_destroy;
        static AVS_EA3_BOOT_STARTUP_T legacy_ea3_boot;
        static AVS_EA3_SHUTDOWN_T legacy_ea3_shutdown;
        static unsigned short legacy_easrv_port;
        static bool legacy_easrv_maint;
        static bool legacy_easrv_smart;
        static bool legacy_ea3_live;

        static core::node_ptr legacy_search(core::property_ptr owner, core::node_ptr parent, const char *path) {
            auto node = legacy_property_search(owner, parent, path);
            // Only eam_boot's lookup establishes ownership. An AVS node is not a property handle.
            if (parent == nullptr && path != nullptr && strcmp(path, "/ea3") == 0) {
                legacy_ea3_config = {owner, node};
            }
            return node;
        }

        static core::avs_error_t legacy_destroy(core::property_ptr owner) {
            if (owner == legacy_ea3_config.owner) {
                legacy_ea3_config = {};
            }
            return legacy_property_destroy(owner);
        }

        static int legacy_boot(void *node) {
            auto owner = legacy_ea3_config.owner;
            if (!owner || node != legacy_ea3_config.node) {
                log_fatal("avs-ea3", "legacy ea3_boot called without its captured property owner");
            }
            if (legacy_ea3_live) {
                log_fatal("avs-ea3", "legacy EA3 boot called twice without shutdown");
            }
            // Keep the game-owned EA3 tree consistent with Spice's selected
            // standard configuration and its resolved datecode.
            set_string(owner, "/ea3/soft/model", game::MODEL);
            set_string(owner, "/ea3/soft/dest", game::DEST);
            set_string(owner, "/ea3/soft/spec", game::SPEC);
            set_string(owner, "/ea3/soft/rev", game::REV);
            set_string(owner, "/ea3/soft/ext", game::EXT);
            apply_identity_overrides(owner, game::MODEL);
            apply_network_settings(owner, legacy_easrv_port, legacy_easrv_maint, legacy_easrv_smart);
            char pcbid[21] {};
            char accountid[21] {};
            core::property_node_refer(owner, nullptr, "/ea3/id/pcbid", core::NODE_TYPE_str,
                    pcbid, sizeof(pcbid));
            core::property_node_refer(owner, nullptr, "/ea3/id/accountid", core::NODE_TYPE_str,
                    accountid, sizeof(accountid));
            EA3_BOOT_PCBID = pcbid;
            EA3_BOOT_ACCOUNTID = accountid;
            logger::PCBIDFilter filter;
            log_info("avs-ea3", "calling game-owned legacy EA3 boot");
            int result = legacy_ea3_boot(node);
            legacy_ea3_live = result >= 0;
            return result;
        }

        static void legacy_shutdown() {
            if (!legacy_ea3_live) {
                log_warning("avs-ea3", "ignoring legacy EA3 shutdown without a successful boot");
                return;
            }
            if (ssl_protocol_fini) {
                ssl_protocol_fini();
                ssl_protocol_fini = nullptr;
                ssl_protocol_init = nullptr;
            }
            log_info("avs-ea3", "calling game-owned legacy EA3 shutdown");
            legacy_ea3_shutdown();
            legacy_ea3_live = false;
        }

        void attach_legacy_hooks() {
            if (!games::gitadora::legacy::enabled() || games::gitadora::legacy::is_cn()) {
                return;
            }
            static bool attached = false;
            if (attached) {
                return;
            }
            auto eam = libutils::try_module("libeamusement.dll");
            if (!eam) {
                log_fatal("avs-ea3", "legacy Japanese game requires libeamusement.dll before EA3 hooks");
            }
            legacy_property_search = detour::iat_try("property_search", legacy_search, eam, core::DLL_NAME.c_str());
            legacy_property_destroy = detour::iat_try("property_destroy", legacy_destroy, eam, core::DLL_NAME.c_str());
            legacy_ea3_boot = detour::iat_try("ea3_boot", legacy_boot, eam, DLL_NAME.c_str());
            legacy_ea3_shutdown = detour::iat_try("ea3_shutdown", legacy_shutdown, eam, DLL_NAME.c_str());
            if (!legacy_property_search || !legacy_property_destroy || !legacy_ea3_boot || !legacy_ea3_shutdown) {
                log_fatal("avs-ea3", "unsupported legacy libeamusement imports (EA3 hooks incomplete)");
            }
            attached = true;
        }

        void prepare_legacy(unsigned short easrv_port, bool easrv_maint, bool easrv_smart) {
            legacy_easrv_port = easrv_port;
            legacy_easrv_maint = easrv_maint;
            legacy_easrv_smart = easrv_smart;

            // The device hook replaces the security plug producer, so publish the old libsystem
            // aliases before native boot. Read the existing configuration without changing it.
            char pcbid[21] {};
            char softid[21] {"012199999999"};
            char hardid[21] {"0100DEADBEEF"};
            char accountid[21] {"012018008135"};
            if (CFG_PATH.empty()) {
                if (!games::gitadora::legacy::ea3_config_path().empty()) {
                    CFG_PATH = games::gitadora::legacy::ea3_config_path();
                } else if (games::gitadora::legacy::identity_path() != "GF" &&
                        games::gitadora::legacy::identity_path() != "DM") {
                    for (auto candidate : {"prop/ea3-config.xml", "prop/ea3-cfg.xml", "prop/eamuse-config.xml",
                            "prop/config.xml"}) {
                        if (fileutils::file_exists(candidate)) {
                            CFG_PATH = candidate;
                            break;
                        }
                    }
                }
            }
            if (!CFG_PATH.empty()) {
                log_info("avs-ea3", "reading XG1 settings from {}", CFG_PATH);
                auto config = core::config_read(CFG_PATH.c_str());
                if (config) {
                    auto read_id = [&](const char *key, char *value, size_t size) {
                        if (core::property_search(config, nullptr, key)) {
                            core::property_node_refer(config, nullptr, key, core::NODE_TYPE_str, value, size);
                        }
                    };
                    read_id("/config/ea3_conf/identity/pcbid", pcbid, sizeof(pcbid));
                    read_id("/ea3/id/pcbid", pcbid, sizeof(pcbid));
                    read_id("/ea3/id/softid", softid, sizeof(softid));
                    read_id("/ea3/id/hardid", hardid, sizeof(hardid));
                    read_id("/ea3/id/accountid", accountid, sizeof(accountid));
                    // Keep the old launcher's service settings unless a Spice option overrides them.
                    if (URL_CUSTOM.empty()) {
                        char services[512] {};
                        read_id("/config/ea3_conf/network/services", services, sizeof(services));
                        read_id("/ea3/network/services", services, sizeof(services));
                        URL_CUSTOM = services;
                    }
                    const char *network = core::property_search(config, nullptr, "/ea3/network")
                            ? "/ea3/network/" : "/config/ea3_conf/network/";
                    for (auto item : {std::pair{"http11", &HTTP11}, std::pair{"url_slash", &URL_SLASH}}) {
                        const auto key = std::string(network) + item.first;
                        if (*item.second < 0 && core::property_search(config, nullptr, key.c_str())) {
                            uint8_t value = 0;
                            if (core::property_node_refer(config, nullptr, key.c_str(),
                                    core::NODE_TYPE_bool, &value, sizeof(value)) > 0) {
                                *item.second = value != 0;
                            }
                        }
                    }
                    core::config_destroy(config);
                }
            }
            if (PCBID_CUSTOM.empty()) {
                PCBID_CUSTOM = pcbid[0] ? pcbid : "01201000000000010101";
            }
            if (!SOFTID_CUSTOM.empty()) {
                strncpy(softid, SOFTID_CUSTOM.c_str(), 20);
            }
            if (!ACCOUNTID_CUSTOM.empty()) {
                strncpy(accountid, ACCOUNTID_CUSTOM.c_str(), 20);
            }
            EA3_BOOT_PCBID = PCBID_CUSTOM.substr(0, 20);
            EA3_BOOT_ACCOUNTID = accountid;
            std::string code = std::string(game::MODEL) + game::DEST + game::SPEC + game::REV;
            const auto mcode = "GC" + code;
            const auto soft_code = fmt::format("{}:{}:{}:{}:{}", game::MODEL, game::DEST,
                    game::SPEC, game::REV, game::EXT);
            core::avs_std_setenv("/env/boot/build", VERSION_STRING);
            core::avs_std_setenv("/env/boot/version", "SPICETOOLS");
            core::avs_std_setenv("/env/boot/tag", "SPICETOOLS");
            core::avs_std_setenv("/env/app/tag", game::EXT);
            core::avs_std_setenv("/env/profile/mcode", mcode.c_str());
            core::avs_std_setenv("/env/profile/security_code", mcode.c_str());
            core::avs_std_setenv("/env/profile/secplug_b_security_code", mcode.c_str());
            core::avs_std_setenv("/env/profile/sysid", EA3_BOOT_PCBID.c_str());
            core::avs_std_setenv("/env/profile/system_id", EA3_BOOT_PCBID.c_str());
            core::avs_std_setenv("/env/profile/softid", softid);
            core::avs_std_setenv("/env/profile/software_id", softid);
            core::avs_std_setenv("/env/profile/license_id", softid);
            core::avs_std_setenv("/env/profile/hwid", hardid);
            core::avs_std_setenv("/env/profile/hardware_id", hardid);
            core::avs_std_setenv("/env/profile/account_id", accountid);
            core::avs_std_setenv("/env/profile/soft_id_code", soft_code.c_str());
            deferredlogs::set_softid(soft_code);
            hooks::avs::init();
            code += game::EXT;
            if (!game::entry_init(code.data(), nullptr)) {
                log_fatal("avs-ea3", "legacy entry initialization failed");
            }
        }

        void load_dll() {
            log_info("avs-ea3", "loading DLL");

            // detect DLL name
            if (fileutils::file_exists(MODULE_PATH / "avs2-ea3.dll")) {
                DLL_NAME = "avs2-ea3.dll";
            } else {
#ifdef SPICE64
                DLL_NAME = "libavs-win64-ea3.dll";
#else
                DLL_NAME = "libavs-win32-ea3.dll";
#endif

                if (!fileutils::file_exists(MODULE_PATH / DLL_NAME)) {
                    std::string info_str { fmt::format(
                        "\n\n"
                        "Failed to find critical ea3 DLL on disk (avs2-ea3.dll OR {})\n"
                        "Looked in the following directory: {}\n"
                        "\n"
                        "One of these is required to boot the game. Spice found neither of them. You do not need both, just one, next to your game DLL.\n"
                        "\n"
                        "HOW TO FIX:\n"
                        "    * Avoid manually specifying DLL path (-exec) and module directory (-modules); let spice2x auto-detect unless you have a good reason not to\n"
                        "    * Ensure you do NOT have multiple copies of the game DLLs (e.g., in contents and in contents\\modules)\n"
                        "    * It's also possible that you have incomplete game data\n"
                        "    * Do NOT copy over random DLLs from another game installation; DLL must match game version\n"
                        "\n"
                    , DLL_NAME, MODULE_PATH) };

                    log_warning("avs-ea3", "{}", info_str);
                    log_fatal("avs-ea3", "Failed to find critical ea3 DLL on disk (avs2-ea3.dll OR {})", DLL_NAME);
                }
            }

            // load library
            DLL_INSTANCE = libutils::load_library(MODULE_PATH / DLL_NAME);

            // check by version string
            std::optional<size_t> ver;
            char version[32] {};
            if (fileutils::version_pe(MODULE_PATH / DLL_NAME, version)) {
                for (size_t i = 0; i < core::AVS_VERSION_COUNT; i++) {
                    if (strcmp(IMPORTS[i].version, version) == 0) {
                        ver = i;
                        break;
                    }
                }
            }

            // check version by brute force
            if (!ver.has_value()) {
                for (size_t i = 0; i < core::AVS_VERSION_COUNT; i++) {
                    if (GetProcAddress(DLL_INSTANCE, IMPORTS[i].boot) != nullptr) {
                        ver = i;
                        break;
                    }
                }
            }

            // check if version was found
            if (!ver.has_value()) {
                log_fatal("avs-ea3", "Unknown {}", DLL_NAME);
            }
            size_t i = ver.value();

            // print version
            VERSION_STR = IMPORTS[i].version;
            log_info("avs-ea3", "Found AVS2 EA3 {}", IMPORTS[i].version);

            // load functions
            avs_ea3_boot_startup = libutils::get_proc<AVS_EA3_BOOT_STARTUP_T>(
                    DLL_INSTANCE, IMPORTS[i].boot);
            avs_ea3_shutdown = libutils::get_proc<AVS_EA3_SHUTDOWN_T>(
                    DLL_INSTANCE, IMPORTS[i].shutdown);
        }

        void boot(unsigned short easrv_port, bool easrv_maint, bool easrv_smart) {
            // detect ea3-config file name
            const char *ea3_config_name;
            if (CFG_PATH.size()) {
                ea3_config_name = CFG_PATH.c_str();
            } else if (fileutils::file_exists("prop/ea3-config.xml")) {
                ea3_config_name = "prop/ea3-config.xml";
            } else if (fileutils::file_exists("prop/ea3-cfg.xml")) {
                ea3_config_name = "prop/ea3-cfg.xml";
            } else if (avs::game::DLL_NAME == "beatstream1.dll" && fileutils::file_exists("prop/ea3-config-1.xml")) {
                ea3_config_name = "prop/ea3-config-1.xml";
            } else if (avs::game::DLL_NAME == "beatstream2.dll" && fileutils::file_exists("prop/ea3-config-2.xml")) {
                ea3_config_name = "prop/ea3-config-2.xml";
            } else {
                ea3_config_name = "prop/eamuse-config.xml";
            }

            if (fileutils::file_exists(ea3_config_name)) {
                log_info("avs-ea3", "found {} on disk", ea3_config_name);
            } else if (CFG_PATH.size()) {
                log_fatal("avs-ea3", "user-specified ea3 config file is missing: {}", ea3_config_name);
            } else {
                log_warning("avs-ea3", "looked for the following files in order:");
                log_warning("avs-ea3", "  * prop/ea3-config.xml");
                log_warning("avs-ea3", "  * prop/ea3-cfg.xml");
                if (avs::game::DLL_NAME == "beatstream1.dll") {
                    log_warning("avs-ea3", "  * prop/ea3-config-1.xml");
                }
                if (avs::game::DLL_NAME == "beatstream2.dll") {
                    log_warning("avs-ea3", "  * prop/ea3-config-2.xml");
                }
                log_warning("avs-ea3", "  * prop/eamuse-config.xml");
                log_warning("avs-ea3", "none of the files above were found, game will fail to boot");
                log_warning("avs-ea3", "usually, this means your game data is incomplete");
                log_fatal("avs-ea3", "no ea3 config file found in prop directory; check log messages");
            }

            // remember new config path
            CFG_PATH = ea3_config_name;

            // detect app-config file name
            APP_PATH = APP_PATH.size() ? APP_PATH : "prop/app-config.xml";

            // detect bootstrap file name
            BOOTSTRAP_PATH = BOOTSTRAP_PATH.size() ? BOOTSTRAP_PATH : "prop/bootstrap.xml";

            // read configuration
            auto ea3_config = avs::core::config_read(ea3_config_name, 0x40000);
            auto app_config = fileutils::file_exists(APP_PATH.c_str())
                    ? avs::core::config_read(APP_PATH.c_str())
                    : avs::core::config_read_string("<param/>");

            // get nodes
            auto ea3 = avs::core::property_search_safe(ea3_config, nullptr, "/ea3");
            auto ea3_id = avs::core::property_search(ea3_config, nullptr, "/ea3/id");
            auto ea3_id_hard = avs::core::property_search(ea3_config, nullptr, "/ea3/id/hardid");
            auto ea3_id_soft = avs::core::property_search(ea3_config, nullptr, "/ea3/id/softid");
            auto ea3_id_account = avs::core::property_search(ea3_config, nullptr, "/ea3/id/accountid");
            auto ea3_soft = avs::core::property_search(ea3_config, nullptr, "/ea3/soft");

            // node values
            char EA3_PCBID[21] { 0 };
            char EA3_HARDID[21] { 0 };
            char EA3_SOFTID[21] { 0 };
            char EA3_ACCOUNTID[21] { 0 };
            char EA3_MODEL[4] { 0 };
            char EA3_DEST[2] { 0 };
            char EA3_SPEC[2] { 0 };
            char EA3_REV[2] { 0 };
            char EA3_EXT[11] { 0 };

            // read id nodes
            if (ea3_id != nullptr) {
                avs::core::property_node_refer(ea3_config, ea3_id, "pcbid",
                        avs::core::NODE_TYPE_str, EA3_PCBID, 21);
                avs::core::property_node_refer(ea3_config, ea3_id, "hardid",
                        avs::core::NODE_TYPE_str, EA3_HARDID, 21);
                avs::core::property_node_refer(ea3_config, ea3_id, "softid",
                        avs::core::NODE_TYPE_str, EA3_SOFTID, 21);
                avs::core::property_node_refer(ea3_config, ea3_id, "accountid",
                        avs::core::NODE_TYPE_str, EA3_ACCOUNTID, 21);
            }

            // set hard ID
            if (ea3_id_hard == nullptr) {
                strncpy(EA3_HARDID, "0100DEADBEEF", sizeof(EA3_HARDID));
                EA3_HARDID[20] = '\0';
                avs::core::property_node_create(ea3_config, nullptr,
                        avs::core::NODE_TYPE_str, "/ea3/id/hardid", EA3_HARDID);
            }

            // set soft ID
            if (ea3_id_soft == nullptr) {
                strncpy(EA3_SOFTID, "012199999999", sizeof(EA3_SOFTID));
                EA3_SOFTID[20] = '\0';
                avs::core::property_node_create(ea3_config, nullptr,
                        avs::core::NODE_TYPE_str, "/ea3/id/softid", EA3_SOFTID);
            }

            // read software nodes
            if (ea3_soft != nullptr) {
                log_misc("avs-ea3", "reading soft id from {}...", ea3_config_name);
                avs::core::property_node_refer(ea3_config, ea3_soft, "model",
                        avs::core::NODE_TYPE_str, EA3_MODEL, 4);
                avs::core::property_node_refer(ea3_config, ea3_soft, "dest",
                        avs::core::NODE_TYPE_str, EA3_DEST, 2);
                avs::core::property_node_refer(ea3_config, ea3_soft, "spec",
                        avs::core::NODE_TYPE_str, EA3_SPEC, 2);
                avs::core::property_node_refer(ea3_config, ea3_soft, "rev",
                        avs::core::NODE_TYPE_str, EA3_REV, 2);
                avs::core::property_node_refer(ea3_config, ea3_soft, "ext",
                        avs::core::NODE_TYPE_str, EA3_EXT, 11);
            } else if (fileutils::file_exists("prop/ea3-ident.xml")) {
                log_misc("avs-ea3", "reading soft id from prop/ea3-ident.xml...");

                // read ident config
                auto ea3_ident = avs::core::config_read("prop/ea3-ident.xml");
                if (ea3_ident == nullptr) {
                    log_fatal("avs-ea3", "'prop/ea3-ident.xml' could not be read as property list");
                }

                ea3_soft = avs::core::property_search_safe(ea3_ident, nullptr, "/ea3_conf/soft");

                avs::core::property_node_refer(ea3_ident, ea3_soft, "model",
                        avs::core::NODE_TYPE_str, EA3_MODEL, 4);
                avs::core::property_node_refer(ea3_ident, ea3_soft, "dest",
                        avs::core::NODE_TYPE_str, EA3_DEST, 2);
                avs::core::property_node_refer(ea3_ident, ea3_soft, "spec",
                        avs::core::NODE_TYPE_str, EA3_SPEC, 2);
                avs::core::property_node_refer(ea3_ident, ea3_soft, "rev",
                        avs::core::NODE_TYPE_str, EA3_REV, 2);
                avs::core::property_node_refer(ea3_ident, ea3_soft, "ext",
                        avs::core::NODE_TYPE_str, EA3_EXT, 11);

                // clean up
                avs::core::config_destroy(ea3_ident);
            } else {
                // <ea3><soft> node missing error
                log_warning("avs-ea3", "soft id (datecode) not found in prop XML files");
                log_warning("avs-ea3", "get fixed prop files and try again - you have incomplete data");
                log_warning("avs-ea3", "for experts only:");
                log_warning("avs-ea3", "  * add the missing <soft>...</soft> node to {}", ea3_config_name);
                log_warning("avs-ea3", "  * alternatively, provide ea3-ident.xml with <ea3_conf><soft>...");

                log_fatal("avs-ea3", "datecode missing in '{}'; 'prop/ea3-ident.xml' also missing", ea3_config_name);
            }

            // set account id (`EA3_PCBID` is valid if and only if `/ea3/id` is present)
            if (ea3_id != nullptr && ea3_id_account == nullptr) {
                const char *id = strcmp(EA3_MODEL, "M32") == 0 ? EA3_PCBID : "012018008135";

                strncpy(EA3_ACCOUNTID, id, sizeof(EA3_ACCOUNTID));
                EA3_ACCOUNTID[20] = '\0';

                avs::core::property_node_create(ea3_config, nullptr,
                        avs::core::NODE_TYPE_str, "/ea3/id/accountid", EA3_ACCOUNTID);
            }

            // replace ext code with release_code from bootstrap.xml if it is a newer date
            if (fileutils::file_exists(BOOTSTRAP_PATH.c_str())) {

                // read config
                auto bootstrap = avs::core::config_read(BOOTSTRAP_PATH.c_str(), 0, true);
                if (bootstrap == nullptr) {

                    // bootstrap.xml may be encrypted
                    log_warning("avs-ea3", "'{}' could not be read as property list", BOOTSTRAP_PATH);

                } else {

                    // get release code
                    char release_code[11] { 0 };
                    avs::core::property_node_refer(bootstrap, nullptr, "/config/release_code",
                            avs::core::NODE_TYPE_str, release_code, 11);

                    // compare dates
                    if (atoi(release_code) > atoi(EA3_EXT)) {
                        log_info("avs-ea3", "overwriting ext {} with {} from {}", EA3_EXT,
                                 release_code, BOOTSTRAP_PATH);
                        strncpy(EA3_EXT, release_code, 11);
                    }

                    // clean up
                    avs::core::config_destroy(bootstrap);
                }
            }

            // fall back to default PCBID if node is not found
            if (!EA3_PCBID[0] && PCBID_CUSTOM.empty()) {
                log_warning("avs-ea3", "no PCBID set, falling back to default PCBID value (01201000000000010101)");
                PCBID_CUSTOM = "01201000000000010101";
            }

            apply_identity_overrides(ea3_config, EA3_MODEL);
            avs::core::property_node_refer(ea3_config, nullptr, "/ea3/id/pcbid",
                    avs::core::NODE_TYPE_str, EA3_PCBID, sizeof(EA3_PCBID));
            avs::core::property_node_refer(ea3_config, nullptr, "/ea3/id/softid",
                    avs::core::NODE_TYPE_str, EA3_SOFTID, sizeof(EA3_SOFTID));
            avs::core::property_node_refer(ea3_config, nullptr, "/ea3/id/accountid",
                    avs::core::NODE_TYPE_str, EA3_ACCOUNTID, sizeof(EA3_ACCOUNTID));

            // check if PCBID is defined - should never hit, left for sanity check only
            if (avs::core::property_search(ea3_config, nullptr, "/ea3/id/pcbid") == nullptr) {
                log_fatal("avs-ea3", "node not found: /ea3/id/pcbid (try using -p to specify PCBID)");
            } else if (strlen(EA3_PCBID) == 0) {
                log_fatal("avs-ea3", "no PCBID set (try using -p to specify PCBID)");
            }

            // remove <debug> since it can expose pcbid in logged requests
            if (!EA3_DEBUG_VERBOSE) {
                avs::core::property_search_remove_safe(ea3_config, nullptr, "/ea3/debug/verbose");
            }

            // remember IDs
            char pcbid_buffer[256] { 0 };
            char accountid_buffer[256] { 0 };
            avs::core::property_node_refer(ea3_config, nullptr, "/ea3/id/pcbid",
                    avs::core::NODE_TYPE_str, pcbid_buffer, sizeof(pcbid_buffer));
            avs::core::property_node_refer(ea3_config, nullptr, "/ea3/id/accountid",
                    avs::core::NODE_TYPE_str, accountid_buffer, sizeof(accountid_buffer));
            EA3_BOOT_PCBID = std::string(pcbid_buffer);
            EA3_BOOT_ACCOUNTID = std::string(accountid_buffer);

            // build security code
            std::ostringstream security_code;
            security_code << "G";
            if (strcmp(EA3_MODEL, "KK9") == 0) {
                // Q (Spec F) is FullHD cabinet, E is OLD cabinet.
                security_code << (strcmp(EA3_SPEC, "F") == 0 ? "Q" : "E");
            } else if (strcmp(EA3_MODEL, "NCG") == 0) {
                // we have Q or E as choice again, see prop/code-config.xml
                security_code << "Q";
            } else if (strcmp(EA3_MODEL, "KBI") == 0) {
                // seems to be required to be set to E
                security_code << "E";
            } else if (strcmp(EA3_MODEL, "KCK") == 0 || strcmp(EA3_MODEL, "NCK") == 0) {
                // unsure if it really makes a difference
                security_code << "E";
            } else if (strcmp(EA3_MODEL, "LA9") == 0) {
                // GQ---J(spec)- in bootstrap.xml
                security_code << "Q";
            } else {
                security_code << "*";
            }
            security_code << EA3_MODEL;
            security_code << EA3_DEST;
            security_code << EA3_SPEC;
            security_code << EA3_REV;
            std::string security_code_str = security_code.str();
            log_info("avs-ea3", "security code: {}", security_code_str);

            // pre game init soft ID code
            std::ostringstream soft_id_code_pre_init;
            soft_id_code_pre_init << EA3_MODEL << ":";
            soft_id_code_pre_init << EA3_DEST << ":";
            soft_id_code_pre_init << EA3_SPEC << ":";
            soft_id_code_pre_init << EA3_REV << ":";
            soft_id_code_pre_init << EA3_EXT;
            std::string soft_id_code_pre_init_str = soft_id_code_pre_init.str();

            // set env variables
            avs::core::avs_std_setenv("/env/boot/build", VERSION_STRING);
            avs::core::avs_std_setenv("/env/boot/version", "SPICETOOLS");
            avs::core::avs_std_setenv("/env/boot/tag", "SPICETOOLS");
            avs::core::avs_std_setenv("/env/profile/security_code", security_code_str.c_str());
            avs::core::avs_std_setenv("/env/profile/secplug_b_security_code", security_code_str.c_str());
            avs::core::avs_std_setenv("/env/profile/system_id", EA3_PCBID);
            avs::core::avs_std_setenv("/env/profile/hardware_id", EA3_HARDID);
            avs::core::avs_std_setenv("/env/profile/license_id", EA3_SOFTID);
            avs::core::avs_std_setenv("/env/profile/software_id", EA3_SOFTID);
            avs::core::avs_std_setenv("/env/profile/account_id", EA3_ACCOUNTID);
            avs::core::avs_std_setenv("/env/profile/soft_id_code", soft_id_code_pre_init_str.c_str());

            // build game init code
            std::ostringstream init_code;
            init_code << EA3_MODEL;
            init_code << EA3_DEST;
            if (avs::game::DLL_NAME == "arkmdxbio2.dll" &&
                strcmp(EA3_MODEL, "MDX") == 0 && strcmp(EA3_SPEC, "I") == 0) {
                // ddr gold cab
                init_code << "G";
                init_code << "B";
            } else {
                init_code << EA3_SPEC;
                init_code << EA3_REV;
            }
            init_code << EA3_EXT;
            std::string init_code_str = init_code.str();

            // save game info
            memcpy(avs::game::MODEL, EA3_MODEL, 4);
            memcpy(avs::game::DEST, EA3_DEST, 2);
            memcpy(avs::game::SPEC, EA3_SPEC, 2);
            memcpy(avs::game::EXT, EA3_EXT, 11);

            // hook AVS functions
            hooks::avs::init();

            // update pcb_type in app-config
            if (PCB_TYPE >= 0) {
                if (strcmp(EA3_MODEL, "K39") == 0 || strcmp(EA3_MODEL, "L39") == 0) {
                    avs::core::property_search_remove_safe(app_config, nullptr, "/param/pcb_type_e");
                    avs::core::property_node_create(app_config, nullptr,
                        avs::core::NODE_TYPE_u8, "/param/pcb_type_e", PCB_TYPE);
                } else {
                    avs::core::property_search_remove_safe(app_config, nullptr, "/param/pcb_type");
                    avs::core::property_node_create(app_config, nullptr,
                        avs::core::NODE_TYPE_u8, "/param/pcb_type", PCB_TYPE);
                }
            }

            // update -WINDOW in app-config
            if (games::gitadora::is_arena_model()) {
                avs::core::property_search_remove_safe(app_config, nullptr, "/param/cmdline");
                std::string cmdline{};
                if (games::gitadora::is_drum()) {
                    if (GRAPHICS_WINDOWED) {
                        cmdline = "-d -DM -WINDOW";
                    } else {
                        cmdline = "-d -DM";
                    }
                } else {
                    if (GRAPHICS_WINDOWED) {
                        cmdline = "-g -GF -WINDOW";
                    } else {
                        cmdline = "-g -GF";
                    }
                }
                log_info("avs-ea3", "using cmdline: `{}`", cmdline);
                avs::core::property_node_create(app_config, nullptr,
                        avs::core::NODE_TYPE_str, "/param/cmdline", cmdline.c_str());
            }

            auto app_param = avs::core::property_search_safe(app_config, nullptr, "/param");

            // call the game init
            log_info("avs-ea3", "calling dll_entry_init (init code = {})", init_code_str);
            if (!avs::game::entry_init(init_code_str.data(), app_param)) {
                for (size_t i = 0; i < 255; i++) {
                    log_warning("avs-ea3", "dll_entry_init failed :(");
                }
                deferredlogs::defer_error_messages({
                    "dll_entry_init failed",
                    "    init code: " + init_code_str,
                    "    this is a fatal error that causes boot failure",
                    "    most likely, your prop files have errors"
                });
            }

            // accommodate changes to soft id code
            //
            // TODO(felix): test this with other games, feature gating at the moment
            // for proper reporting of Omnimix and other song packs
            if (_stricmp(EA3_MODEL, "LDJ") == 0 ||
                _stricmp(EA3_MODEL, "L44") == 0 ||
                _stricmp(EA3_MODEL, "T44") == 0 ||
                _stricmp(EA3_MODEL, "M39") == 0 ||
                _stricmp(EA3_MODEL, "KFC") == 0)
            {
                //memcpy(EA3_MODEL, init_code_str.c_str(), 3);
                //EA3_DEST[0] = init_code_str[3];
                //EA3_SPEC[0] = init_code_str[4];
                EA3_REV[0] = init_code_str[5];
                //memcpy(EA3_EXT, init_code_str.c_str() + 6, 10);
            }

            // remove nodes
            avs::core::property_search_remove_safe(ea3_config, nullptr, "/ea3/soft/model");
            avs::core::property_search_remove_safe(ea3_config, nullptr, "/ea3/soft/dest");
            avs::core::property_search_remove_safe(ea3_config, nullptr, "/ea3/soft/spec");
            avs::core::property_search_remove_safe(ea3_config, nullptr, "/ea3/soft/rev");
            avs::core::property_search_remove_safe(ea3_config, nullptr, "/ea3/soft/ext");

            // create nodes
            avs::core::property_node_create(ea3_config, nullptr,
                    avs::core::NODE_TYPE_str, "/ea3/soft/model", EA3_MODEL);
            avs::core::property_node_create(ea3_config, nullptr,
                    avs::core::NODE_TYPE_str, "/ea3/soft/dest", EA3_DEST);
            avs::core::property_node_create(ea3_config, nullptr,
                    avs::core::NODE_TYPE_str, "/ea3/soft/spec", EA3_SPEC);
            avs::core::property_node_create(ea3_config, nullptr,
                    avs::core::NODE_TYPE_str, "/ea3/soft/rev", EA3_REV);
            avs::core::property_node_create(ea3_config, nullptr,
                    avs::core::NODE_TYPE_str, "/ea3/soft/ext", EA3_EXT);

            // create soft ID code
            std::ostringstream soft_id_code;
            soft_id_code << EA3_MODEL << ":";
            soft_id_code << EA3_DEST << ":";
            soft_id_code << EA3_SPEC << ":";
            soft_id_code << EA3_REV << ":";
            soft_id_code << EA3_EXT;
            std::string soft_id_code_str = soft_id_code.str();
            log_info("avs-ea3", "soft id code: {}", soft_id_code_str);
            deferredlogs::set_softid(soft_id_code_str);

            // set soft ID code
            avs::core::avs_std_setenv("/env/profile/soft_id_code", soft_id_code_str.c_str());

            // save new rev
            memcpy(avs::game::REV, EA3_REV, 2);

            apply_network_settings(ea3_config, easrv_port, easrv_maint, easrv_smart);

            // boot EA3
            logger::PCBIDFilter filter;
            log_info("avs-ea3", "calling ea3 boot");
            avs_ea3_boot_startup(ea3);

            // clean up
            avs::core::config_destroy(app_config);
            avs::core::config_destroy(ea3_config);

            // print avs mountpoints
            // it does not exist in VERY old legacy AVS versions (like 2.10.2)
            if (avs::core::avs_fs_dump_mountpoint) {
                avs::core::avs_fs_dump_mountpoint();
            }

            // success
            log_info("avs-ea3", "boot done");
        }

        void shutdown() {

            // SSL shutdown
            if (ssl_protocol_fini != nullptr) {
                log_info("avs-ea3", "unregistering SSL protocol handler");
                ssl_protocol_fini();
            }

            // EA3 shutdown
            log_info("avs-ea3", "shutdown");
            avs_ea3_shutdown();
        }
    }
}
