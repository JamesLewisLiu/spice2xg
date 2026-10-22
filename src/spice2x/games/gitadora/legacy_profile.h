#pragma once

#include <cstddef>
#include <cstring>
#include <string>

namespace games::gitadora::legacy {
    struct Identity {
        std::string model, dest, spec, rev, ext;
    };

    // Original XG1, XG1CN and K32 getters copy 0x54 bytes through a hidden
    // __cdecl return pointer. Other generations retain their existing adapter.
    constexpr std::size_t BUTTON_STATE_SIZE = 84;
    inline void clear_button_state(void *buffer, bool legacy_profile) {
        std::memset(buffer, 0, legacy_profile ? BUTTON_STATE_SIZE : 64);
    }

    bool parse_identity(const std::string &xml, Identity &result, std::string &error);
    bool valid_release_code(const std::string &code);
}
