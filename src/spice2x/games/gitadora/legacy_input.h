#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "legacy_profile.h"

namespace games::gitadora::legacy {
    // Both original game.dll input-test callers copy 21 DWORDs. libextio's
    // get_input builds its bit mask from these same bytes and pad values.
    struct ButtonState {
        std::uint32_t reserved_header[4];
        std::int32_t sensor_xyz[3];
        std::uint8_t sensor_error;
        std::uint8_t guitar_buttons[16];
        std::uint8_t reserved_guitar[2];
        std::uint8_t guitar_motor;
        std::int32_t drum_pads[8];
        std::uint8_t drum_pedals[2];
        std::uint8_t reserved_tail[2];
    };
    static_assert(sizeof(ButtonState) == BUTTON_STATE_SIZE);
    static_assert(offsetof(ButtonState, sensor_xyz) == 16);
    static_assert(offsetof(ButtonState, guitar_buttons) == 29);
    static_assert(offsetof(ButtonState, guitar_motor) == 47);
    static_assert(offsetof(ButtonState, drum_pads) == 48);
    static_assert(offsetof(ButtonState, drum_pedals) == 80);

    inline void populate_button_inputs(void *buffer, std::uint32_t input, bool drum) {
        ButtonState state;
        std::memcpy(&state, buffer, sizeof(state));
        if (drum) {
            // Original order: HT, LT, SN, FT, LC, RC, HH, unused. A digital
            // button has no physical velocity; 100 gives the raw TEST meter a
            // visible strike while gameplay continues to use the input mask.
            constexpr std::uint32_t masks[] = {0x80, 0x100, 0x40, 0x10000,
                    0x4000, 0x200, 0x20, 0};
            for (std::size_t i = 0; i < 8; ++i) {
                state.drum_pads[i] = (input & masks[i]) ? 100 : 0;
            }
            state.drum_pedals[0] = (input & 0x800) != 0;
            state.drum_pedals[1] = (input & 0x8000) != 0;
        } else {
            // R/G/B/Y/P, five unused buttons, pick up/down, knob bits1/2/3,
            // unused fourth effect switch. A wail uses sensor_xyz separately.
            constexpr std::uint32_t masks[] = {0x200, 0x400, 0x800, 0x1000,
                    0x4000, 0, 0, 0, 0, 0, 0x80, 0x100, 0x8000, 0x10000, 0x20000, 0};
            for (std::size_t i = 0; i < 16; ++i) {
                state.guitar_buttons[i] = (input & masks[i]) != 0;
            }
        }
        std::memcpy(buffer, &state, sizeof(state));
    }

    inline int drum_button_value(const ButtonState &state, int button) {
        if (button >= 0 && button < 8) return state.drum_pads[button];
        if (button == 8 || button == 9) return state.drum_pedals[button - 8];
        return 0;
    }

    inline int guitar_button_value(const ButtonState &state, int button) {
        return button >= 0 && button < 16 ? state.guitar_buttons[button] != 0 : 0;
    }

    inline bool attack_border_capacity_valid(int capacity) {
        return capacity >= 7;
    }
}
