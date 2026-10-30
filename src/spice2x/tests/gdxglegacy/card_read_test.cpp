#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "acio/icca/icca.h"

static int EXTDEV_CARDUNIT_EJECT[2];
static bool EXTDEV_CARD_IN[2];
static uint8_t EXTDEV_CARD[2][8];

#include "card_read_hooks.inc"

// Indirect calls keep the legacy/null-status cases observable in optimized
// builds, including a negative control built from the original broken hooks.
static int (__cdecl *volatile read_legacy)(int, void *) = cardunit_card_read;
static int (__cdecl *volatile read_with_status)(int, void *, int *) = cardunit_card_read2;

static bool check(bool condition, const char *message) {
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
    return condition;
}

int main(int argc, char **argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (argc != 2) return 2;
    const bool legacy = std::strcmp(argv[1], "legacy") == 0;
    const bool status_requested = std::strcmp(argv[1], "status") == 0;
    if (!legacy && !status_requested && std::strcmp(argv[1], "null-status") != 0) return 2;

    const std::array<uint8_t, 8> cards[] = {
        {0xe0, 0x04, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc},
        {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef},
    };
    // Repeated absent/present/absent reads exercise both branches after state
    // changes; the other reader must retain its own card and eject state.
    for (int unit = 0; unit < 2; ++unit) {
        for (int iteration = 0; iteration < 20; ++iteration) {
            for (int state : {0, 1, 0, 2, 0}) {
                const int other = 1 - unit;
                EXTDEV_CARDUNIT_EJECT[unit] = 1;
                EXTDEV_CARDUNIT_EJECT[other] = 7;
                EXTDEV_CARD_IN[unit] = state != 0;
                EXTDEV_CARD_IN[other] = true;
                const auto &id = cards[state == 2 ? 1 : 0];
                std::memcpy(EXTDEV_CARD[unit], id.data(), 8);
                std::memset(EXTDEV_CARD[other], 0x5a, 8);
                std::array<uint8_t, 24> guarded;
                guarded.fill(0xa5);
                std::array<int, 3> status = {0x12345678, -99, 0x23456789};
                auto output = guarded.data() + 8;
                const int result = legacy ? read_legacy(unit, output)
                    : read_with_status(unit, output, status_requested ? &status[1] : nullptr);
                const std::array<uint8_t, 8> empty{};
                const auto &expected = state == 0 ? empty : id;
                if (!check(result == (state == 0 ? 1 : 0), "return code changed")
                    || !check(std::equal(expected.begin(), expected.end(), output), "wrong card bytes")
                    || !check(std::all_of(guarded.begin(), guarded.begin() + 8,
                        [](auto b) { return b == 0xa5; }), "card prefix overwritten")
                    || !check(std::all_of(guarded.end() - 8, guarded.end(),
                        [](auto b) { return b == 0xa5; }), "card suffix overwritten")
                    || !check(status[0] == 0x12345678 && status[2] == 0x23456789,
                        "status canary overwritten")
                    || !check(status[1] == (status_requested ? state : -99), "wrong card status")
                    || !check(EXTDEV_CARDUNIT_EJECT[unit] == 0, "eject flag not cleared")
                    || !check(EXTDEV_CARDUNIT_EJECT[other] == 7 && EXTDEV_CARD_IN[other],
                        "other reader state changed")
                    || !check(std::all_of(std::begin(EXTDEV_CARD[other]), std::end(EXTDEV_CARD[other]),
                        [](auto b) { return b == 0x5a; }), "other reader card changed")
                    || !check(EXTDEV_CARD_IN[unit] == (state != 0)
                        && std::memcmp(EXTDEV_CARD[unit], id.data(), 8) == 0,
                        "read changed the stored card")) return 1;
            }
        }
    }
    std::printf("PASS: %s, 200 reads across two readers, card/status bounds and eject state\n", argv[1]);
    return 0;
}
