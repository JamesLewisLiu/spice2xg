#pragma once

#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <cstring>

namespace games::gitadora::legacy {
    struct StorageLayout {
        std::array<std::string, 6> paths;
        std::string asset_root;

        static std::string host_path(const char *value) {
            auto path = std::filesystem::absolute(std::filesystem::u8path(value)).lexically_normal().generic_string();
            while (path.size() > 3 && path.back() == '/') path.pop_back();
            return path;
        }

        template<class Read>
        void configure(const std::filesystem::path &base, const std::filesystem::path &assets, Read read) {
            const auto device = [&](const char *key, const std::filesystem::path &fallback) {
                auto value = read(key);
                return host_path(value.empty() ? fallback.generic_string().c_str() : value.c_str());
            };
            asset_root = device("/config/fs/root/device", assets);
            paths[1] = device("/config/fs/nvram/device", base / "CONF/NVRAM");
            paths[2] = device("/config/fs/raw/device", base / "CONF/RAW");
            paths[0] = std::filesystem::u8path(paths[1]).parent_path().generic_string();
            paths[3] = device("/config/fs/tmp/device", base / "TMP");
            paths[4] = device("/config/fs/cache/device", base / "CACHE");
            paths[5] = device("/config/fs/proxy/device", std::filesystem::u8path(paths[4]) / "PROXY");
        }

        bool contains_device(const char *value) const {
            if (!value || !*value) return false;
            auto path = host_path(value);
            if (_stricmp(path.c_str(), asset_root.c_str()) == 0) return true;
            for (const auto &device : paths) {
                if (!device.empty() && _stricmp(path.c_str(), device.c_str()) == 0) return true;
            }
            return false;
        }

        const char *redirect(const char *path) const {
            if (!path) return nullptr;
            std::string normalized(path);
            std::replace(normalized.begin(), normalized.end(), '\\', '/');
            while (!normalized.empty() && normalized.back() == '/') normalized.pop_back();
            const char *relative = normalized.c_str();
            bool conf = true, cache = true;
            if (_strnicmp(relative, "E:/", 3) == 0) { relative += 3; cache = false; }
            else if (_strnicmp(relative, "F:/", 3) == 0) { relative += 3; conf = false; }
            else if (normalized.rfind("./", 0) == 0) relative += 2;
            else if (normalized.find(':') != std::string::npos || normalized.rfind('/', 0) == 0) return path;
            static constexpr const char *folders[] = {"CONF", "CONF/NVRAM", "CONF/RAW", "TMP", "CACHE", "CACHE/PROXY"};
            for (size_t i = 0; i < paths.size(); ++i) {
                if ((i < 3 ? conf : cache) && _stricmp(relative, folders[i]) == 0) return paths[i].c_str();
            }
            return path;
        }
    };
}
