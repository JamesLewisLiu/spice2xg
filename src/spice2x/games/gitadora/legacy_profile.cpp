#include "legacy_profile.h"

#include <algorithm>
#include "external/tinyxml2/tinyxml2.h"

namespace games::gitadora::legacy {
    bool valid_release_code(const std::string &code) {
        return code.size() == 10 && code != "0000000000" &&
                std::all_of(code.begin(), code.end(), [](unsigned char c) {
                    return c >= '0' && c <= '9';
                });
    }

    bool parse_identity(const std::string &xml, Identity &result, std::string &error) {
        tinyxml2::XMLDocument document;
        if (document.Parse(xml.c_str(), xml.size()) != tinyxml2::XML_SUCCESS) {
            error = "malformed identity XML";
            return false;
        }
        auto root = document.FirstChildElement("ea3_conf");
        if (!root) {
            root = document.FirstChildElement("ea3");
        }
        auto soft = root ? root->FirstChildElement("soft") : nullptr;
        if (!soft) {
            error = "identity requires ea3/soft or ea3_conf/soft";
            return false;
        }
        Identity parsed;
        const auto read = [soft](const char *name) -> std::string {
            auto element = soft->FirstChildElement(name);
            return element && element->GetText() ? element->GetText() : "";
        };
        parsed.model = read("model");
        parsed.dest = read("dest");
        parsed.spec = read("spec");
        parsed.rev = read("rev");
        parsed.ext = read("ext");
        if ((parsed.model != "J32" && parsed.model != "J33") ||
            (parsed.dest != "J" && parsed.dest != "C") ||
            parsed.spec != "A" || parsed.rev.size() != 1 ||
            !(parsed.ext.empty() || parsed.ext == "0000000000" || valid_release_code(parsed.ext))) {
            error = "expected XG1 identity J32/J33, destination J/C, spec A, one-character rev and optional ten-digit ext";
            return false;
        }
        result = std::move(parsed);
        error.clear();
        return true;
    }
}
