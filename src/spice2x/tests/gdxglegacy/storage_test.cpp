#include <cassert>
#include <iostream>
#include <map>
#include "games/gitadora/legacy_storage.h"

using namespace games::gitadora::legacy;

int main() {
    for (const auto &mode : {"GF", "DM"}) {
        StorageLayout layout;
        const auto root = std::filesystem::path("E:/HDD/XG1/binspice/DEV") / mode;
        layout.configure(root, "E:/HDD/XG1", [](const char *) { return std::string(); });
        assert(layout.paths[1] == (root / "CONF/NVRAM").generic_string());
        assert(layout.paths[2] == (root / "CONF/RAW").generic_string());
        assert(layout.paths[3] == (root / "TMP").generic_string());
        assert(layout.paths[5] == (root / "CACHE/PROXY").generic_string());
        assert(std::string(layout.redirect("E:\\CONF\\NVRAM")) == layout.paths[1]);
        assert(std::string(layout.redirect("F:/CACHE/PROXY/")) == layout.paths[5]);
        assert(std::string(layout.redirect("./TMP")) == layout.paths[3]);
        assert(std::string(layout.redirect("CONF/RAW")) == layout.paths[2]);
        assert(layout.contains_device(layout.paths[5].c_str()));
        assert(layout.contains_device((layout.paths[5] + "/").c_str()));
        const auto mixed = "e:\\hdd\\xg1\\binspice\\dev\\" + std::string(mode) + "\\tmp";
        assert(layout.contains_device(mixed.c_str()));
        assert(!layout.contains_device("E:/CONF"));
        assert(!layout.contains_device("E:/HDD/XG1/binspice/CACHE/PROXY"));
        assert(!layout.contains_device(nullptr));
        assert(!layout.contains_device(""));
        assert(layout.redirect(nullptr) == nullptr);
        const char *other = "E:/OTHER/CONF";
        assert(layout.redirect(other) == other);
        other = "F:/CONF";
        assert(layout.redirect(other) == other);
        other = "E:/TMP";
        assert(layout.redirect(other) == other);
        std::map<std::string, std::string> settings {
            {"/config/fs/nvram/device", "E:/Custom/NVRAM"},
            {"/config/fs/raw/device", "F:/Custom/RAW"},
            {"/config/fs/tmp/device", "E:/Custom/Temp"},
            {"/config/fs/cache/device", "E:/Custom/Cache"},
            {"/config/fs/root/device", "E:/Assets"},
        };
        layout.configure(root, "E:/HDD/XG1", [&](const char *key) { return settings[key]; });
        assert(layout.paths[1] == "E:/Custom/NVRAM");
        assert(layout.paths[2] == "F:/Custom/RAW");
        assert(layout.paths[3] == "E:/Custom/Temp");
        assert(layout.paths[5] == "E:/Custom/Cache/PROXY");
        assert(layout.asset_root == "E:/Assets");
        assert(layout.contains_device("e:\\custom\\temp"));
        assert(layout.contains_device("F:/Custom/RAW"));
        assert(!layout.contains_device("E:/Custom/CacheOther"));
        assert(std::string(layout.redirect("E:/CONF/NVRAM")) == "E:/Custom/NVRAM");
    }
    std::cout << "GF/DM defaults, custom AVS devices and drive-remap exclusions passed\n";
}
