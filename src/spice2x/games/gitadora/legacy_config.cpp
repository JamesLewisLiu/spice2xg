#include "legacy.h"
#include "legacy_movie.h"
#include "legacy_profile.h"
#include "legacy_native.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <algorithm>
#include <cctype>
#include "avs/game.h"
#include "launcher/launcher.h"
#include "launcher/options.h"
#include "external/tinyxml2/tinyxml2.h"
#include "util/libutils.h"
#include "util/logging.h"
#include "util/sysutils.h"

namespace games::gitadora::legacy {
    static std::string selected_identity;
    static std::string selected_ea3_config;
    static bool chinese = false;
    static std::filesystem::path selected_directory;

    bool enabled() { return !selected_identity.empty(); }
    bool is_cn() { return enabled() && chinese; }
    const std::string &identity_path() { return selected_identity; }
    const std::string &ea3_config_path() { return selected_ea3_config; }
    const std::filesystem::path &mode_directory() { return selected_directory; }

    bool detect_native() {
        Identity identity;
        return std::filesystem::exists(MODULE_PATH / "boot.dll") &&
                std::filesystem::exists(MODULE_PATH / "libgftools.dll") &&
                inspect_native_identity(MODULE_PATH / "libsystem.dll", identity);
    }

    void configure(const std::string &path, const std::string &bootstrap) {
#if SPICE64
        log_fatal("gdxg-legacy", "XG1 requires the 32-bit spice.exe");
#else
        // No fixed movie reservation: preserve address space for native heaps
        // and D3D resources. A 32-bit host still benefits from LAA on 64-bit OS.
        movie::prepare();
        if (!sysutils::is_large_address_aware()) {
            log_warning("gdxg-legacy", "XG1 is using a 2 GiB address space; use spice_laa.exe on 64-bit Windows for more virtual address space");
        }
        Identity identity;
        std::string mode = path;
        std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char ch) { return std::toupper(ch); });
        if (mode == "GF" || mode == "DM") {
            if (!inspect_native_identity(MODULE_PATH / "libsystem.dll", identity)) {
                log_fatal("gdxg-legacy", "cannot identify XG1 region/date from libsystem.dll");
            }
            identity.model = mode == "DM" ? "J32" : "J33";
            selected_identity = mode;
        } else {
            auto absolute_path = std::filesystem::absolute(std::filesystem::u8path(path));
            std::ifstream input(absolute_path, std::ios::binary);
            if (!input) {
                log_fatal("gdxg-legacy", "cannot read identity: {}", path);
            }
            const std::string xml((std::istreambuf_iterator<char>(input)), {});
            std::string error;
            if (!parse_identity(xml, identity, error)) {
                log_fatal("gdxg-legacy", "{}: {}", path, error);
            }
            tinyxml2::XMLDocument document;
            document.Parse(xml.c_str(), xml.size());
            if (document.FirstChildElement("ea3")) {
                selected_ea3_config = absolute_path.string();
            }
            selected_identity = absolute_path.string();
        }
        if (!bootstrap.empty() || std::filesystem::exists("prop/bootstrap.xml")) {
            auto release = launcher::detect_bootstrap_release_code(bootstrap);
            if (valid_release_code(release) &&
                    (!valid_release_code(identity.ext) || release > identity.ext)) {
                identity.ext = release;
            }
        }
        std::strcpy(avs::game::MODEL, identity.model.c_str());
        std::strcpy(avs::game::DEST, identity.dest.c_str());
        std::strcpy(avs::game::SPEC, identity.spec.c_str());
        std::strcpy(avs::game::REV, identity.rev.c_str());
        std::strcpy(avs::game::EXT, identity.ext.c_str());
        chinese = identity.dest == "C";
        selected_directory = MODULE_PATH / "DEV" / (identity.model == "J32" ? "DM" : "GF");
        if (selected_ea3_config.empty() && std::filesystem::exists(selected_directory / "ea3-config.xml")) {
            selected_ea3_config = (selected_directory / "ea3-config.xml").string();
        }
        log_info("gdxg-legacy", "selected {} from {}", avs::game::get_identifier(), selected_identity);
#endif
    }

    void resolve_datecode() {
#if !SPICE64
        if (!enabled()) {
            return;
        }
        // The exported getter returns the module's published release code,
        // not the PE build timestamp. Both original versions start with this
        // field populated; AVS is live if a future module needs its env fallback.
        auto module = libutils::load_library(MODULE_PATH / "libsystem.dll");
        auto getter = libutils::get_proc<const char *(__cdecl *)()>(module, "sys_code_get_release_code");
        const char *release = getter();
        std::string native_release;
        if (release) {
            native_release.assign(release, strnlen(release, 11));
        }
        if (!valid_release_code(avs::game::EXT)) {
            if (!valid_release_code(native_release)) {
                log_fatal("gdxg-legacy", "no valid release code in identity/bootstrap or libsystem.dll");
            }
            std::strcpy(avs::game::EXT, native_release.c_str());
            log_info("gdxg-legacy", "detected release code {} from libsystem.dll", avs::game::EXT);
        } else if (valid_release_code(native_release) && native_release != avs::game::EXT) {
            log_warning("gdxg-legacy", "configured release {} differs from native {}; retaining configured identity",
                    avs::game::EXT, native_release);
        }
        log_info("gdxg-legacy", "resolved game identity {}", avs::game::get_identifier());
#endif
    }
}
