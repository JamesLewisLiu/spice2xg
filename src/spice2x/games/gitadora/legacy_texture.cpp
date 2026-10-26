#include "legacy_texture.h"

#include <climits>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>

#include "util/detour.h"
#include "util/logging.h"

namespace games::gitadora::legacy::texture {
namespace {
#if defined(_M_IX86) || defined(__i386__)
// Official IDA: JP gfCreateTexture is cdecl with six stack arguments and
// a 32-bit return. Its GL_LUMINANCE4_ALPHA4 path swaps the two nibbles and
// uploads A4L4; CN instead expands the nibbles and uploads A8L8.
using CreateTexture = uintptr_t (__cdecl *)(int, int, int, int, int, const void *);
CreateTexture original_create = nullptr;
HMODULE attached = nullptr;
std::mutex attach_mutex;

uintptr_t __cdecl create_texture(int format, int width, int height, int flags,
                                  int size, const void *source) {
    constexpr int LA4 = 0x8043, LA8 = 0x8045;
    if (format != LA4 || !source || width <= 0 || height <= 0 ||
        uint64_t(width) * uint64_t(height) > INT_MAX / 2) {
        return original_create(format, width, height, flags, size, source);
    }
    const auto pixels = size_t(width) * size_t(height);
    std::unique_ptr<uint8_t[]> expanded(new (std::nothrow) uint8_t[pixels * 2]);
    if (!expanded) {
        log_warning("gdxg-texture", "Cannot expand {}x{} packed texture", width, height);
        return original_create(format, width, height, flags, size, source);
    }
    const auto *input = static_cast<const uint8_t *>(source);
    for (size_t i = 0; i < pixels; ++i) {
        // Match CN's zero-preserving expansion, including its rounding. The
        // native LA8 path swaps each input pair before uploading [L8, A8].
        const uint8_t l = input[i] & 0xf0;
        const uint8_t a = uint8_t(input[i] << 4);
        expanded[2 * i] = a ? a | 0x0f : 0;
        expanded[2 * i + 1] = l ? l | 0x0f : 0;
    }
    auto result = original_create(LA8, width, height, flags,
                                  int(pixels * 2), expanded.get());
    log_info("gdxg-texture", "Expanded JP packed texture {}x{} to native A8L8", width, height);
    return result;
}
#endif
}

bool attach(HMODULE tools) {
#if defined(_M_IX86) || defined(__i386__)
    std::lock_guard lock(attach_mutex);
    if (!tools) return false;
    if (attached) return attached == tools;
    auto *base = reinterpret_cast<const uint8_t *>(tools);
    auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS32 *>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
        nt->OptionalHeader.SizeOfImage != 0x36b000) return false;
    if (nt->FileHeader.TimeDateStamp == 0x4ccf6cab) {
        log_info("gdxg-texture", "CN already uses native A8L8; no texture hook needed");
        return true;
    }
    auto entry = GetProcAddress(tools, "gfCreateTexture");
    // The absolute address in the prologue relocates with ASLR: validate the
    // opcode and its resolved current-texture global rather than fixed bytes.
    uintptr_t current = 0;
    if (entry && reinterpret_cast<const uint8_t *>(entry)[0] == 0xa1) {
        uint32_t address;
        std::memcpy(&address, reinterpret_cast<const uint8_t *>(entry) + 1, 4);
        current = address;
    }
    if (nt->FileHeader.TimeDateStamp != 0x4d476ce4 ||
        reinterpret_cast<const uint8_t *>(entry) != base + 0x2bd0 ||
        current != reinterpret_cast<uintptr_t>(base + 0x2ee438) ||
        std::memcmp(base + 0x2800, "\x81\xf9\x58\x80\x00\x00\xb8\x15", 8) != 0 ||
        std::memcmp(base + 0x2840, "\xb8\x34\x00\x00\x00\xc3", 6) != 0) {
        log_warning("gdxg-texture", "Unrecognized libgftools; packed texture hook not installed");
        return false;
    }
    if (!detour::trampoline_try(reinterpret_cast<CreateTexture>(entry), create_texture,
                                &original_create)) {
        log_warning("gdxg-texture", "Cannot hook JP gfCreateTexture");
        return false;
    }
    attached = tools;
    log_info("gdxg-texture", "JP packed texture compatibility installed (A4L4 input to A8L8)");
    return true;
#else
    (void)tools;
    return false;
#endif
}
}
