// OTP storage selection (docs/otp-image.md): an explicitly selected image
// (OP1EMU_OTP_FILE) must be an existing 8192-byte regular file, is never
// created or resized, and keeps guest writes in memory when read-only. The
// historical default still creates an erased otp.bin. All images here are
// synthetic; no real OTP contents are used.
#include "cpu/cpu.h"
#include "cpu/otp.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {
constexpr std::size_t kImageSize = 8192;
constexpr u32 kControl = 0x00, kData0 = 0x80;
constexpr u32 kDoRead = 1u << 14, kDoWrite = 1u << 15;

void Check(bool condition, const std::string& what) {
    if (!condition) {
        std::fprintf(stderr, "otp storage: %s\n", what.c_str());
        std::exit(1);
    }
}

std::vector<u8> Pattern() {
    std::vector<u8> bytes(kImageSize);
    for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<u8>(i * 7 + 3);
    return bytes;
}

void WriteFile(const fs::path& path, const std::vector<u8>& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    Check(static_cast<bool>(file), "cannot write fixture " + path.string());
}

std::vector<u8> ReadFile(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

u32 WordAt(const std::vector<u8>& bytes, std::size_t offset) {
    return static_cast<u32>(bytes[offset]) | static_cast<u32>(bytes[offset + 1]) << 8 |
           static_cast<u32>(bytes[offset + 2]) << 16 | static_cast<u32>(bytes[offset + 3]) << 24;
}

// Page reads and writes go through the guest-visible OTP_CONTROL/OTP_DATA registers.
void ReadPage(OTP& otp, u32 page, u32 (&words)[4]) {
    otp.Write32(kControl, page | kDoRead);
    for (u32 i = 0; i < 4; ++i) words[i] = otp.Read32(kData0 + i * 4);
}

void WritePage(OTP& otp, u32 page, const u32 (&words)[4]) {
    for (u32 i = 0; i < 4; ++i) otp.Write32(kData0 + i * 4, words[i]);
    otp.Write32(kControl, page | kDoWrite);
}

void CheckPage(OTP& otp, const std::vector<u8>& image, u32 page) {
    u32 words[4];
    ReadPage(otp, page, words);
    for (u32 i = 0; i < 4; ++i)
        Check(words[i] == WordAt(image, page * 16 + i * 4), "page " + std::to_string(page) + " word " + std::to_string(i));
}

bool RejectsWith(const fs::path& path, const std::string& expected) {
    try {
        OTP otp(0xFFC03600, path.string(), OtpStorage::ExistingImage);
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        Check(message.find(expected) != std::string::npos, "message lacks '" + expected + "': " + message);
        Check(message.find("8192") != std::string::npos, "message lacks the required size: " + message);
        return true;
    }
    return false;
}

void ValidImageLoadsUnchanged(const fs::path& dir) {
    const auto path = dir / "valid.otp";
    const auto image = Pattern();
    WriteFile(path, image);
    {
        OTP otp(0xFFC03600, path.string(), OtpStorage::ExistingImage);
        Check(!otp.StorageReadOnly(), "a writable image is opened writable");
        for (u32 page : {0x000u, 0x004u, 0x0DFu, 0x100u, 0x1FFu}) CheckPage(otp, image, page);
    }
    Check(ReadFile(path) == image, "loading an image does not modify it");
}

void WritableImageKeepsGuestWrites(const fs::path& dir) {
    const auto path = dir / "writable.otp";
    auto image = Pattern();
    WriteFile(path, image);
    const u32 words[4] = {0x11223344, 0x55667788, 0x99AABBCC, 0xDDEEFF00};
    {
        OTP otp(0xFFC03600, path.string(), OtpStorage::ExistingImage);
        WritePage(otp, 0x10, words);
    }
    for (std::size_t i = 0; i < 4; ++i)
        for (std::size_t b = 0; b < 4; ++b) image[0x100 + i * 4 + b] = static_cast<u8>(words[i] >> (8 * b));
    Check(ReadFile(path) == image, "a guest write is saved to its page and nowhere else");
}

void ReadOnlyImageKeepsWritesInMemory(const fs::path& dir) {
    if (geteuid() == 0) {
        std::puts("otp storage: skipping read-only check as root");
        return;
    }
    const auto path = dir / "read-only.otp";
    const auto image = Pattern();
    WriteFile(path, image);
    fs::permissions(path, fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read);
    const u32 words[4] = {1, 2, 3, 4};
    {
        OTP otp(0xFFC03600, path.string(), OtpStorage::ExistingImage);
        Check(otp.StorageReadOnly(), "a read-only image is reported read-only");
        CheckPage(otp, image, 0x004);
        WritePage(otp, 0x10, words);
        u32 readBack[4];
        ReadPage(otp, 0x10, readBack);
        for (u32 i = 0; i < 4; ++i) Check(readBack[i] == words[i], "a read-only image keeps guest writes in memory");
    }
    Check(ReadFile(path) == image, "a read-only image is never modified");
    fs::permissions(path, fs::perms::owner_write, fs::perm_options::add);
}

void InvalidImagesAreRejected(const fs::path& dir) {
    const auto missing = dir / "missing.otp";
    Check(RejectsWith(missing, "does not exist"), "a missing image is rejected");
    Check(!fs::exists(missing), "a missing image is not created");
    Check(RejectsWith("", "empty path"), "an empty path is rejected");
    Check(RejectsWith(dir, "not a regular file"), "a directory is rejected");

    const auto image = Pattern();
    for (std::size_t size : {std::size_t{0}, std::size_t{16}, kImageSize - 1, kImageSize + 1, kImageSize * 2}) {
        const auto path = dir / ("size-" + std::to_string(size) + ".otp");
        std::vector<u8> bytes(size);
        for (std::size_t i = 0; i < size; ++i) bytes[i] = image[i % kImageSize];
        WriteFile(path, bytes);
        Check(RejectsWith(path, "is " + std::to_string(size) + " bytes"), "a " + std::to_string(size) + "-byte image is rejected");
        Check(ReadFile(path) == bytes, "a rejected image is not modified");
    }
}

void DefaultStorageStillCreatesErasedImage(const fs::path& dir) {
    const auto path = dir / "default-created.otp";
    {
        OTP otp(0xFFC03600, path.string());
        Check(!otp.StorageReadOnly(), "the default storage is writable");
        u32 words[4];
        ReadPage(otp, 0x007, words); // FPS03: synthesized part string
        Check(words[0] == 0x50534441 && words[1] == 0x3546422D, "the default image gets the ADSP-BF524 part string");
    }
    Check(fs::file_size(path) == kImageSize, "the default storage creates an 8192-byte image");
}

// tools/import_otp_dump.py mirrors CalculateEcc; otp_import_tests checks the same vectors.
struct EccProbe : OTP {
    using OTP::OTP;
    u8 Ecc(u64 value) { return CalculateEcc(value); }
};

void EccMatchesImporterVectors(const fs::path& dir) {
    EccProbe otp(0xFFC03600, (dir / "ecc.otp").string());
    const struct { u64 value; u8 ecc; } vectors[] = {
        {0, 0x00}, {1, 0x83}, {u64{1} << 63, 0xC7}, {~u64{0}, 0xFF}, {0x0123456789ABCDEFull, 0x9C},
        {0x3546422D50534441ull, 0xDF}, // "ADSP-BF5", little-endian
    };
    for (const auto& vector : vectors)
        Check(otp.Ecc(vector.value) == vector.ecc, "ECC of " + std::to_string(vector.value) + " matches the importer");
}

// The CPU selects the image from OP1EMU_OTP_FILE and otherwise keeps ./otp.bin.
void CpuSelectsImageFromEnvironment(const fs::path& dir) {
    const auto workdir = dir / "cpu";
    fs::create_directory(workdir);
    const auto previous = fs::current_path();
    fs::current_path(workdir);

    const auto image = dir / "cpu-selected.otp";
    WriteFile(image, Pattern());
    setenv("OP1EMU_OTP_FILE", image.c_str(), 1);
    { BlackFinCpu cpu; }
    Check(!fs::exists(workdir / "otp.bin"), "a selected image replaces the default otp.bin");
    Check(ReadFile(image) == Pattern(), "the selected image is unchanged by CPU construction");

    const auto missing = dir / "cpu-missing.otp";
    setenv("OP1EMU_OTP_FILE", missing.c_str(), 1);
    bool rejected = false;
    try {
        BlackFinCpu cpu;
    } catch (const std::runtime_error& error) {
        rejected = std::string(error.what()).rfind("OP1EMU_OTP_FILE: ", 0) == 0;
    }
    Check(rejected, "an invalid OP1EMU_OTP_FILE stops CPU construction with a named error");
    Check(!fs::exists(missing) && !fs::exists(workdir / "otp.bin"), "a rejected selection creates no file");

    unsetenv("OP1EMU_OTP_FILE");
    { BlackFinCpu cpu; }
    Check(fs::exists(workdir / "otp.bin") && fs::file_size(workdir / "otp.bin") == kImageSize,
          "without OP1EMU_OTP_FILE the CPU still creates ./otp.bin");
    fs::current_path(previous);
}
} // namespace

int main() {
    const auto dir = fs::temp_directory_path() / ("op1emu-otp-storage-" + std::to_string(getpid()));
    fs::remove_all(dir);
    fs::create_directory(dir);
    ValidImageLoadsUnchanged(dir);
    WritableImageKeepsGuestWrites(dir);
    ReadOnlyImageKeepsWritesInMemory(dir);
    InvalidImagesAreRejected(dir);
    DefaultStorageStillCreatesErasedImage(dir);
    CpuSelectsImageFromEnvironment(dir);
    EccMatchesImporterVectors(dir);
    fs::remove_all(dir);
    std::puts("otp storage tests passed");
    return 0;
}
