#pragma once

#include "io.h"
#include <array>
#include <string>
#include <fstream>

// How OTP construction treats its storage file (docs/otp-image.md).
enum class OtpStorage {
    // The historical default: open the file, or create an erased one with a
    // synthesized chip ID and part string when it cannot be opened.
    CreateIfMissing,
    // An explicitly selected image: it must already exist as a regular file of
    // exactly TOTAL_SIZE_BYTES. Construction throws std::runtime_error otherwise.
    // A read-only image is loaded and guest OTP writes stay in memory.
    ExistingImage,
};

class OTP : public RegisterDevice {
public:
    // ProcessWithInterrupt() is the inherited no-op, so skipping it changes nothing.
    bool ServiceIsNoOp() override { return true; }
    OTP(u32 baseAddr, const std::string& storagePath, OtpStorage storage = OtpStorage::CreateIfMissing);
    ~OTP();
    bool StorageReadOnly() const { return storageReadOnly; }

protected:
    constexpr static size_t NUM_PAGES = 0x200;
    constexpr static size_t PAGE_SIZE_WORDS = 4; // 128 bits = 4 x 32-bit words
    constexpr static size_t PAGE_SIZE_BYTES = PAGE_SIZE_WORDS * sizeof(u32); // 16 bytes
    constexpr static size_t TOTAL_SIZE_BYTES = NUM_PAGES * PAGE_SIZE_BYTES; // 8KB

    void ReadPage(u16 page);
    void WritePage(u16 page);
    void TransferWithMask(u32* dst, const u32* src);
    void WritePageValue(u16 page, u64 lo, u64 hi);
    u8 CalculateEcc(u64 data);
    void WriteEcc(u16 page, u8 lo, u8 hi);
    void LoadFromFile();
    void OpenExistingImage(const std::string& storagePath);
    void SavePageToFile(u16 page);

    // OTP memory storage: 0x200 pages, each 128 bits (4 x 32-bit words)
    std::array<u32, NUM_PAGES * PAGE_SIZE_WORDS> mem{};

    // Storage file
    std::fstream storageFile;
    bool storageReadOnly = false;
    bool readOnlyWriteLogged = false;

    // Register values
    u16 page = 0;
    bool doRead = false;
    bool doWrite = false;
    bool statusDone = false;
    u16 ben{0xFFFF};
    u32 timing{0x00001485};
    u32 data[4]{0};
};