#pragma once

#include "io.h"
#include <functional>
#include <string>
#include <vector>

// The BF52x on-chip boot ROM (docs/boot-rom.md). Its contents are supplied by
// the user at run time; no ROM bytes are part of the emulator.
constexpr u32 kBootRomBase = 0xEF000000;
constexpr u32 kBootRomSize = 0x8000;

// SHA-256 of the one ROM revision whose v241 behaviour has been validated.
extern const char* const kValidatedBootRomSha256;

struct BootRomImage {
    std::vector<u8> bytes;   // exactly kBootRomSize bytes
    std::string sha256;      // lowercase hex
    bool validatedRevision = false;
};

// Reads and validates a ROM image file without modifying it. Throws
// std::runtime_error with an actionable message for any unusable input.
BootRomImage LoadBootRomImage(const std::string& path);

// True for the digest of the validated revision.
bool IsValidatedBootRomDigest(const std::string& sha256);

// The ROM region backed by a supplied image.
class BootROM : public MemoryDevice {
public:
    BootROM(u32 baseAddr, std::vector<u8> image);

    void BindFastMem(const std::shared_ptr<FastMem>& mem) override;

private:
    std::vector<u8> image_;
};

// The ROM region when no image was supplied. It is never mapped as memory:
// every access reaches this device, which reports it so the CPU can stop.
// Reads return 0 only so that the reporting access can complete.
class AbsentBootROM : public Device {
public:
    using AccessHandler = std::function<void(u32 address, bool write)>;

    AbsentBootROM(u32 baseAddr, AccessHandler onAccess);

    // No ProcessWithInterrupt work: skipping the service pass changes nothing.
    bool ServiceIsNoOp() override { return true; }

    void Read(u32 offset, void* buffer, u32 length) override;
    void Write(u32 offset, const void* buffer, u32 length) override;
    u32 Read32(u32 offset) override;
    void Write32(u32 offset, u32 value) override;

private:
    AccessHandler onAccess_;
};
