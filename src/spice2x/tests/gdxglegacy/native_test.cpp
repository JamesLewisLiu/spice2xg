#include <cassert>
#include <iostream>
#include "games/gitadora/legacy_native.h"

using namespace games::gitadora::legacy;

static std::vector<unsigned char> fixture(const char *date) {
    std::vector<unsigned char> bytes(0x800);
    const auto put = [&](size_t pos, const auto &value) { std::memcpy(bytes.data() + pos, &value, sizeof(value)); };
    IMAGE_DOS_HEADER dos {};
    dos.e_magic = IMAGE_DOS_SIGNATURE;
    dos.e_lfanew = 0x80;
    put(0, dos);
    IMAGE_NT_HEADERS32 nt {};
    nt.Signature = IMAGE_NT_SIGNATURE;
    nt.FileHeader.Machine = IMAGE_FILE_MACHINE_I386;
    nt.FileHeader.SizeOfOptionalHeader = sizeof(nt.OptionalHeader);
    nt.FileHeader.NumberOfSections = 1;
    nt.OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
    nt.OptionalHeader.ImageBase = 0x10000000;
    nt.OptionalHeader.NumberOfRvaAndSizes = 16;
    nt.OptionalHeader.DataDirectory[0] = {0x1000, sizeof(IMAGE_EXPORT_DIRECTORY)};
    put(0x80, nt);
    IMAGE_SECTION_HEADER section {};
    section.VirtualAddress = 0x1000;
    section.PointerToRawData = 0x200;
    section.SizeOfRawData = 0x600;
    put(0x80 + sizeof(nt), section);
    IMAGE_EXPORT_DIRECTORY exports {};
    exports.NumberOfFunctions = exports.NumberOfNames = 1;
    exports.AddressOfFunctions = 0x1050;
    exports.AddressOfNames = 0x1054;
    exports.AddressOfNameOrdinals = 0x1058;
    put(0x200, exports);
    put(0x250, uint32_t(0x1200));
    put(0x254, uint32_t(0x1080));
    std::memcpy(bytes.data() + 0x280, "sys_code_get_release_code", 26);
    bytes[0x400] = 0x80; bytes[0x401] = 0x3d;
    put(0x402, uint32_t(0x10001400));
    bytes[0x407] = 0x75;
    std::memcpy(bytes.data() + 0x600, date, 11);
    return bytes;
}

int main(int argc, char **argv) {
    Identity identity;
    for (auto date : {"2011020100", "2010110200"}) {
        auto bytes = fixture(date);
        assert(inspect_native_identity(bytes, identity));
        assert(identity.ext == date && identity.dest == (date[3] == '0' ? "C" : "J"));
        for (size_t length : {size_t(0), size_t(64), size_t(0x200), size_t(0x600), size_t(0x60a)}) {
            auto truncated = bytes;
            truncated.resize(length);
            assert(!inspect_native_identity(truncated, identity));
        }
        auto corrupt = bytes;
        corrupt[0x258] = 0xff;
        assert(!inspect_native_identity(corrupt, identity));
        corrupt = bytes; corrupt[0x400] = 0xc3;
        assert(!inspect_native_identity(corrupt, identity));
        corrupt = bytes; corrupt[0x605] = '9';
        assert(!inspect_native_identity(corrupt, identity));
        corrupt = bytes; corrupt[0x84] = 0x64;
        assert(!inspect_native_identity(corrupt, identity));
        corrupt = bytes;
        uint32_t invalid = 0xfffffff0;
        std::memcpy(corrupt.data() + 0x402, &invalid, 4);
        assert(!inspect_native_identity(corrupt, identity));
    }
    if (argc == 3) {
        assert(inspect_native_identity(std::filesystem::u8path(argv[1]), identity));
        assert(identity.dest == argv[2]);
        std::cout << identity.dest << ':' << identity.ext << '\n';
    }
    std::cout << "Native identity inspection passed\n";
}
