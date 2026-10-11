#include "bootrom.h"
#include "utils/sha256.h"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <system_error>

const char* const kValidatedBootRomSha256 = "9fe4f939d4bf098fbfbc774e67241bd3ddfd6a410005cf40927e99cc710f871d";

bool IsValidatedBootRomDigest(const std::string& sha256) {
    return sha256 == kValidatedBootRomSha256;
}

BootRomImage LoadBootRomImage(const std::string& path) {
    const auto fail = [&path](const std::string& problem) {
        throw std::runtime_error("boot ROM image '" + path + "' " + problem + "; expected a readable " +
                                 std::to_string(kBootRomSize) + "-byte BF52x boot ROM image (see docs/boot-rom.md)");
    };
    if (path.empty()) fail("has an empty path");
    std::error_code error;
    const auto status = std::filesystem::status(path, error);
    if (status.type() == std::filesystem::file_type::not_found) fail("does not exist");
    if (error) fail("cannot be inspected (" + error.message() + ")");
    if (!std::filesystem::is_regular_file(status)) fail("is not a regular file");
    const auto size = std::filesystem::file_size(path, error);
    if (error) fail("has an unknown size (" + error.message() + ")");
    if (size != kBootRomSize) fail("is " + std::to_string(size) + " bytes");

    std::ifstream file(path, std::ios::binary); // read-only: the image is never modified
    if (!file) fail("cannot be opened for reading");
    BootRomImage image;
    image.bytes.resize(kBootRomSize);
    file.read(reinterpret_cast<char*>(image.bytes.data()), kBootRomSize);
    if (file.gcount() != static_cast<std::streamsize>(kBootRomSize)) fail("could not be read completely");
    image.sha256 = Sha256Hex(image.bytes.data(), image.bytes.size());
    image.validatedRevision = IsValidatedBootRomDigest(image.sha256);
    return image;
}

BootROM::BootROM(u32 baseAddr, std::vector<u8> image)
    : MemoryDevice("Boot ROM", baseAddr, kBootRomSize), image_(std::move(image)) {
    if (image_.size() != kBootRomSize) throw std::invalid_argument("boot ROM image must be exactly 32768 bytes");
}

void BootROM::BindFastMem(const std::shared_ptr<FastMem>& mem) {
    MemoryDevice::BindFastMem(mem);
    std::memcpy(memAddress, image_.data(), image_.size());
}

AbsentBootROM::AbsentBootROM(u32 baseAddr, AccessHandler onAccess)
    : Device("Boot ROM (absent)", baseAddr, kBootRomSize), onAccess_(std::move(onAccess)) {}

void AbsentBootROM::Read(u32 offset, void* buffer, u32 length) {
    std::memset(buffer, 0, length);
    if (onAccess_) onAccess_(baseAddress + offset, false);
}

void AbsentBootROM::Write(u32 offset, const void*, u32) {
    if (onAccess_) onAccess_(baseAddress + offset, true);
}

u32 AbsentBootROM::Read32(u32 offset) {
    if (onAccess_) onAccess_(baseAddress + offset, false);
    return 0;
}

void AbsentBootROM::Write32(u32 offset, u32) {
    if (onAccess_) onAccess_(baseAddress + offset, true);
}
