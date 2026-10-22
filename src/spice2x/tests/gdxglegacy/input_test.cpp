#include "games/gitadora/legacy_input.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <utility>

using namespace games::gitadora::legacy;

static void check(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

int main() {
    try {
        // Byte offsets below are the game.dll consumer's layout, independently
        // of ButtonState members, and include the final two pedal bytes.
        constexpr std::pair<std::uint32_t, std::size_t> guitar[] = {
            {0x200, 29}, {0x400, 30}, {0x800, 31}, {0x1000, 32}, {0x4000, 33},
            {0x80, 39}, {0x100, 40}, {0x8000, 41}, {0x10000, 42}, {0x20000, 43}};
        constexpr std::pair<std::uint32_t, std::size_t> drum[] = {
            {0x80, 48}, {0x100, 52}, {0x40, 56}, {0x10000, 60},
            {0x4000, 64}, {0x200, 68}, {0x20, 72}, {0x800, 80}, {0x8000, 81}};
        for (bool is_drum : {false, true}) {
            auto verify = [&](std::uint32_t mask, std::size_t offset) {
                std::array<std::uint8_t, 16 + BUTTON_STATE_SIZE + 16> memory;
                memory.fill(0xa5);
                auto buffer = memory.data() + 16;
                clear_button_state(buffer, true);
                const std::int32_t xyz[] = {-4080, 2040, 4080};
                std::memcpy(buffer + 16, xyz, sizeof(xyz));
                populate_button_inputs(buffer, mask, is_drum);
                std::array<std::uint8_t, BUTTON_STATE_SIZE> expected{};
                std::memcpy(expected.data() + 16, xyz, sizeof(xyz));
                expected[offset] = is_drum && offset < 80 ? 100 : 1;
                check(std::equal(expected.begin(), expected.end(), buffer),
                    "raw input bit mapped to wrong byte, adjacent pad, or sensor");
                ButtonState state;
                std::memcpy(&state, buffer, sizeof(state));
                if (is_drum) {
                    auto button = offset < 80 ? static_cast<int>((offset - 48) / 4)
                            : static_cast<int>(offset - 80 + 8);
                    check(drum_button_value(state, button) == (offset < 80 ? 100 : 1),
                        "native DM getter lost pad strength or pedal state");
                } else {
                    check(guitar_button_value(state, static_cast<int>(offset - 29)) == 1,
                        "native GF getter lost fret, pick or effect state");
                }
                check(std::all_of(memory.begin(), memory.begin() + 16,
                    [](auto value) { return value == 0xa5; }), "raw sret prefix overwritten");
                check(std::all_of(memory.end() - 16, memory.end(),
                    [](auto value) { return value == 0xa5; }), "raw sret suffix overwritten");
                populate_button_inputs(buffer, 0, is_drum);
                check(std::memcmp(buffer + 16, xyz, sizeof(xyz)) == 0,
                    "neutral input changed sensor coordinates");
                check(buffer[offset] == 0, "released raw input remained pressed");
            };
            if (is_drum) {
                for (auto [mask, offset] : drum) verify(mask, offset);
            } else {
                for (auto [mask, offset] : guitar) verify(mask, offset);
            }
        }
        ButtonState neutral{};
        check(drum_button_value(neutral, 8) == 0 && drum_button_value(neutral, 9) == 0,
            "unpressed DM pedals reported pressed");
        check(drum_button_value(neutral, -1) == 0 && drum_button_value(neutral, 10) == 0,
            "invalid DM button accessed outside state");
        check(guitar_button_value(neutral, -1) == 0 && guitar_button_value(neutral, 16) == 0,
            "invalid GF button accessed outside state");
        check(!attack_border_capacity_valid(-1) && !attack_border_capacity_valid(6)
            && attack_border_capacity_valid(7) && attack_border_capacity_valid(8),
            "native seven-element attack-border capacity contract inverted");
        std::cout << "PASS: GF frets/picks/effects, DM pads/pedals, XYZ preservation and 84-byte canaries\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
