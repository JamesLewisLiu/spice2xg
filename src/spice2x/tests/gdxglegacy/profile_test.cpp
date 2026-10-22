#include "games/gitadora/legacy_profile.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>

using namespace games::gitadora::legacy;

static void check(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

static std::string xml(const std::string &model, const std::string &dest,
        const std::string &ext = "", const std::string &spec = "A") {
    return "<?xml version=\"1.0\" encoding=\"shift-jis\"?>\n"
        "<!-- real files may contain comments -->\n<ea3_conf><soft>"
        "<model __type=\"str\">" + model + "</model><dest>" + dest +
        "</dest><spec>" + spec + "</spec><rev>A</rev><ext>" + ext +
        "</ext></soft></ea3_conf>";
}

int main() {
    try {
        for (auto model : {"J32", "J33"}) {
            for (auto dest : {"J", "C"}) {
                Identity identity;
                std::string error;
                check(parse_identity(xml(model, dest), identity, error), "valid identity rejected");
                check(identity.model == model && identity.dest == dest && identity.ext.empty(),
                    "identity fields not preserved");
                check(parse_identity(xml(model, dest, "2011020100"), identity, error),
                    "ten-digit ext rejected");
                auto standard = xml(model, dest, "2011020100");
                standard.replace(standard.find("ea3_conf"), 8, "ea3");
                standard.replace(standard.rfind("ea3_conf"), 8, "ea3");
                check(parse_identity(standard, identity, error), "standard EA3 identity rejected");
                check(identity.model == model && identity.dest == dest && identity.ext == "2011020100",
                    "standard identity fields not preserved");
            }
        }
        check(valid_release_code("2010110200") && valid_release_code("2011020100"),
            "native release code rejected");
        check(!valid_release_code("") && !valid_release_code("0000000000") &&
            !valid_release_code("201102010x") && !valid_release_code("20110201000"),
            "invalid native release code accepted");
        for (const auto &invalid : {"", "<ea3_conf>", "<ea3_conf/>", "<ea3_conf><soft/></ea3_conf>",
                "<wrong><soft><model>J32</model></soft></wrong>"}) {
            Identity identity;
            std::string error;
            check(!parse_identity(invalid, identity, error) && !error.empty(), "malformed identity accepted");
        }
        for (const auto &invalid : {xml("K32", "J"), xml("J32", "K"), xml("J33", "C", "123"),
                xml("J32", "J", "abcdefghij"), xml("J32", "J", "", "B")}) {
            Identity identity;
            std::string error;
            check(!parse_identity(invalid, identity, error), "unsupported version accepted");
        }

        // Prefix and suffix stand in for the caller's adjacent stack slots.
        alignas(16) std::array<uint8_t, 16 + BUTTON_STATE_SIZE + 16> memory;
        for (bool legacy : {false, true}) {
            memory.fill(0xa5);
            clear_button_state(memory.data() + 16, legacy);
            const auto written = legacy ? BUTTON_STATE_SIZE : 64;
            check(std::all_of(memory.begin(), memory.begin() + 16,
                [](auto byte) { return byte == 0xa5; }), "sret prefix overwritten");
            check(std::all_of(memory.begin() + 16, memory.begin() + 16 + written,
                [](auto byte) { return byte == 0; }), "sret return bytes uninitialized");
            check(std::all_of(memory.begin() + 16 + written, memory.end(),
                [](auto byte) { return byte == 0xa5; }), "sret suffix or other profile overwritten");
        }
        std::cout << "PASS: four identities, invalid input, 84-byte sret and unchanged 64-byte adapter\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
