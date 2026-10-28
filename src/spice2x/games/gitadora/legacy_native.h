#pragma once

#include <windows.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <vector>
#include "legacy_profile.h"

namespace games::gitadora::legacy {
    // Inspect the exported date getter as file data. Never load a game DLL or
    // run DllMain/AVS just to identify a game in the standalone configurator.
    inline bool inspect_native_identity(const std::vector<unsigned char> &file, Identity &identity) {
        const auto read = [&file](size_t offset, auto &value) {
            if (offset > file.size() || sizeof(value) > file.size() - offset) return false;
            std::memcpy(&value, file.data() + offset, sizeof(value));
            return true;
        };
        IMAGE_DOS_HEADER dos {};
        IMAGE_NT_HEADERS32 nt {};
        if (!read(0, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 ||
                !read(size_t(dos.e_lfanew), nt) || nt.Signature != IMAGE_NT_SIGNATURE ||
                nt.FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
                nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
                nt.FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER32) ||
                nt.OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT ||
                nt.FileHeader.NumberOfSections > 96) return false;
        std::vector<IMAGE_SECTION_HEADER> sections(nt.FileHeader.NumberOfSections);
        const size_t section_table = size_t(dos.e_lfanew) + 24 + nt.FileHeader.SizeOfOptionalHeader;
        for (size_t i = 0; i < sections.size(); ++i) {
            if (!read(section_table + i * sizeof(IMAGE_SECTION_HEADER), sections[i])) return false;
        }
        const auto offset = [&](uint32_t rva, size_t length) -> std::optional<size_t> {
            for (const auto &section : sections) {
                if (rva < section.VirtualAddress) continue;
                const size_t delta = rva - section.VirtualAddress;
                if (delta > section.SizeOfRawData || length > section.SizeOfRawData - delta) continue;
                const uint64_t pos = uint64_t(section.PointerToRawData) + delta;
                if (pos <= file.size() && length <= file.size() - size_t(pos)) return size_t(pos);
            }
            return std::nullopt;
        };
        const auto &directory = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        auto exports_offset = offset(directory.VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY));
        IMAGE_EXPORT_DIRECTORY exports {};
        if (!exports_offset || !read(*exports_offset, exports) || exports.NumberOfNames > 65536 ||
                exports.NumberOfFunctions > 65536) return false;
        auto names = offset(exports.AddressOfNames, size_t(exports.NumberOfNames) * 4);
        auto ordinals = offset(exports.AddressOfNameOrdinals, size_t(exports.NumberOfNames) * 2);
        auto functions = offset(exports.AddressOfFunctions, size_t(exports.NumberOfFunctions) * 4);
        if (!names || !ordinals || !functions) return false;
        constexpr char getter[] = "sys_code_get_release_code";
        for (size_t i = 0; i < exports.NumberOfNames; ++i) {
            uint32_t name_rva = 0;
            if (!read(*names + i * 4, name_rva)) return false;
            auto name = offset(name_rva, sizeof(getter));
            if (!name || std::memcmp(file.data() + *name, getter, sizeof(getter))) continue;
            uint16_t ordinal = 0;
            uint32_t function_rva = 0;
            if (!read(*ordinals + i * 2, ordinal) || ordinal >= exports.NumberOfFunctions ||
                    !read(*functions + ordinal * 4, function_rva)) return false;
            auto code = offset(function_rva, 9);
            if (!code) return false;
            const auto *p = file.data() + *code;
            // Official IDA: both versions begin cmp byte ptr [date],0; jnz.
            if (p[0] != 0x80 || p[1] != 0x3d || p[6] != 0 || p[7] != 0x75) return false;
            uint32_t date_va = 0;
            read(*code + 2, date_va);
            if (date_va < nt.OptionalHeader.ImageBase) return false;
            auto data = offset(date_va - nt.OptionalHeader.ImageBase, 11);
            if (!data || file[*data + 10] != 0) return false;
            const std::string date(reinterpret_cast<const char *>(file.data() + *data), 10);
            if (date != "2011020100" && date != "2010110200") return false;
            identity = {"J33", date == "2010110200" ? "C" : "J", "A", "A", date};
            return true;
        }
        return false;
    }

    inline bool inspect_native_identity(const std::filesystem::path &module, Identity &identity) {
        std::ifstream input(module, std::ios::binary | std::ios::ate);
        if (!input) return false;
        const auto length = input.tellg();
        if (length < 0 || length > 16 * 1024 * 1024) return false;
        std::vector<unsigned char> file(static_cast<size_t>(length));
        input.seekg(0);
        if (!input.read(reinterpret_cast<char *>(file.data()), file.size())) return false;
        return inspect_native_identity(file, identity);
    }
}
