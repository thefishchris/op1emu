#include "otp.h"
#include "utils/log.h"
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <system_error>

static constexpr u32 OTP_FPS00 = 0x004;
static constexpr u32 OTP_FPS03 = 0x007;

OTP::OTP(u32 baseAddr, const std::string& storagePath, OtpStorage storage)
    : RegisterDevice("OTP", baseAddr, 0xA0),
      ben(0xFFFF), timing(0x00001485) {

    bool fileCreating = false;
    if (storage == OtpStorage::ExistingImage) {
        OpenExistingImage(storagePath);
    } else {
        // Try to open existing file, or create new one
        storageFile.open(storagePath, std::ios::in | std::ios::out | std::ios::binary);
        if (!storageFile.is_open()) {
            fileCreating = true;
            // Create new file initialized with erased state (0x00)
            storageFile.open(storagePath, std::ios::out | std::ios::binary);
            if (storageFile.is_open()) {
                std::vector<u8> eraseBuffer(TOTAL_SIZE_BYTES, 0);
                storageFile.write(reinterpret_cast<char*>(eraseBuffer.data()), TOTAL_SIZE_BYTES);
                storageFile.close();
                storageFile.open(storagePath, std::ios::in | std::ios::out | std::ios::binary);
            }
        }

        if (storageFile.is_open()) {
            LoadFromFile();
        } else {
            LogError("OTP: Failed to open storage file: %s\n", storagePath.c_str());
        }
    }

    REG32(OTP_CONTROL, 0x00);
    FIELD(OTP_CONTROL, PAGE, 0, 9, R(page), W(page));
    FIELD(OTP_CONTROL, DO_WRITE, 15, 1, R(doWrite ? 1 : 0), W(doWrite));
    FIELD(OTP_CONTROL, DO_READ, 14, 1, R(doRead ? 1 : 0), W(doRead));
    OTP_CONTROL.writeCallback = [this](u32 value) {
        if (doWrite) {
            WritePage(page);
        }
        if (doRead) {
            ReadPage(page);
        }
        statusDone = true;
    };

    REG32(OTP_BEN, 0x04);
    FIELD(OTP_BEN, BEN, 0, 16, R(ben), W(ben));

    REG32(OTP_STATUS, 0x08);
    FIELD(OTP_STATUS, DONE, 0, 1, R(statusDone ? 1 : 0), W1C(statusDone));
    FIELD(OTP_STATUS, RESERVED, 1, 15, R(0), N());

    REG32(OTP_TIMING, 0x0C);
    FIELD(OTP_TIMING, TIMING, 0, 32, R(timing), W(timing));

    // OTP_DATA0-3 registers (32-bit each)
    for (u32 i = 0; i < 4; i++) {
        REG32(OTP_DATA, 0x80 + i * 4);
        auto r = [this, i]() -> u32 {
            return data[i];
        };
        auto w = [this, i](u32 v) {
            data[i] = v;
        };
        FIELD(OTP_DATA, DATA, 0, 32, r, w);
    }

    if (fileCreating) {
        // Initialize OTP memory with chip-specific data
        // Semi-random value for unique chip id (using address as seed)
        uint64_t chip_id_lo = (u64)(uintptr_t)this;
        // BOOTROM requires bit 62 to be set for valid chip ID? See 0xEF0009A4
        chip_id_lo |= 0x4000000000000000ULL;
        WritePageValue(OTP_FPS00, chip_id_lo, ~(u64)(uintptr_t)this);
        // Part string and FPS03 value
        char partStr[16] = "ADSP-BF524";
        u16 fps03 = 0x420C; // BF524
        partStr[14] = (fps03 >> 0) & 0xFF;
        partStr[15] = (fps03 >> 8) & 0xFF;

        u64 lo = *(u64*)&partStr[0];
        u64 hi = *(u64*)&partStr[8];
        WritePageValue(OTP_FPS03, lo, hi);
    }
}

OTP::~OTP() {
    if (storageFile.is_open()) {
        storageFile.close();
    }
}

void OTP::TransferWithMask(u32* dst, const u32* src)
{
    // Transfer bytes based on ben (byte enable) mask
    u8* dstBytes = reinterpret_cast<u8*>(dst);
    const u8* srcBytes = reinterpret_cast<const u8*>(src);

    for (int i = 0; i < 16; ++i) {
        if (ben & (1 << i)) {
            dstBytes[i] = srcBytes[i];
        }
    }
}

void OTP::ReadPage(u16 page)
{
    if (page >= NUM_PAGES) return;

    TransferWithMask(data, &mem[page * PAGE_SIZE_WORDS]);
}

void OTP::WritePage(u16 page)
{
    if (page >= NUM_PAGES) return;

    TransferWithMask(&mem[page * PAGE_SIZE_WORDS], data);
    SavePageToFile(page);
}

void OTP::WritePageValue(u16 page, u64 lo, u64 hi)
{
    if (page >= NUM_PAGES) return;

    u32 src[4] = {
        static_cast<u32>(lo),
        static_cast<u32>(lo >> 32),
        static_cast<u32>(hi),
        static_cast<u32>(hi >> 32)
    };
    TransferWithMask(&mem[page * PAGE_SIZE_WORDS], src);
    SavePageToFile(page);

    u64* ptr = (u64*)&mem[page * PAGE_SIZE_WORDS];
    lo = ptr[0];
    hi = ptr[1];
    u8 loEcc = CalculateEcc(lo);
    u8 hiEcc = CalculateEcc(hi);
    WriteEcc(page, loEcc, hiEcc);
}

u8 OTP::CalculateEcc(u64 data)
{
    u8 p1 = 0, p2 = 0, p4 = 0, p8 = 0, p16 = 0, p32 = 0, p64 = 0;
    u8 overall = 0;
    int dataIdx = 0;
    for (int pos = 1; pos <= 72 && dataIdx < 64; pos++) {
        // Skip parity positions (powers of 2)
        if ((pos & (pos - 1)) == 0)  // pos is a power of 2
            continue;
        // Get the data bit at this logical position
        int bit = (data >> dataIdx) & 1;
        dataIdx++;
        // XOR into appropriate parity accumulators
        if (pos & 1)   p1 ^= bit;
        if (pos & 2)   p2 ^= bit;
        if (pos & 4)   p4 ^= bit;
        if (pos & 8)   p8 ^= bit;
        if (pos & 16)  p16 ^= bit;
        if (pos & 32)  p32 ^= bit;
        if (pos & 64)  p64 ^= bit;
        // Overall parity includes all data bits
        overall ^= bit;
    }
    // Overall parity also includes all parity bits
    overall ^= p1 ^ p2 ^ p4 ^ p8 ^ p16 ^ p32 ^ p64;
    // Pack ECC: [P0, P64, P32, P16, P8, P4, P2, P1]
    u8 ecc = (overall << 7) | (p64 << 6) | (p32 << 5) | (p16 << 4) |
             (p8 << 3) | (p4 << 2) | (p2 << 1) | p1;
    return ecc;
}

void OTP::WriteEcc(u16 page, u8 lo, u8 hi)
{
    if (page >= NUM_PAGES) return;

    u16 eccPage = 0;
    if (page < NUM_PAGES / 2) {
        eccPage = 0xE0 + page / 8;
    } else {
        eccPage = 0x1E0 + (page - NUM_PAGES / 2) / 8;
    }

    int offset = page % 8;
    u16* ptr = reinterpret_cast<u16*>(&mem[eccPage * PAGE_SIZE_WORDS]);
    ptr[offset] = lo | (hi << 8);
    SavePageToFile(eccPage);
}

void OTP::OpenExistingImage(const std::string& storagePath)
{
    const auto fail = [&storagePath](const std::string& problem) {
        throw std::runtime_error("OTP image '" + storagePath + "' " + problem + "; expected an existing raw OTP image of exactly " +
                                 std::to_string(TOTAL_SIZE_BYTES) + " bytes (" + std::to_string(NUM_PAGES) + " pages x " +
                                 std::to_string(PAGE_SIZE_BYTES) + " bytes, see docs/otp-image.md)");
    };
    if (storagePath.empty()) fail("has an empty path");

    std::error_code error;
    const auto status = std::filesystem::status(storagePath, error);
    if (status.type() == std::filesystem::file_type::not_found) fail("does not exist");
    if (error) fail("cannot be inspected (" + error.message() + ")");
    if (!std::filesystem::is_regular_file(status)) fail("is not a regular file");
    const auto size = std::filesystem::file_size(storagePath, error);
    if (error) fail("has an unknown size (" + error.message() + ")");
    if (size != TOTAL_SIZE_BYTES) fail("is " + std::to_string(size) + " bytes, so it is truncated or not an OTP image");

    // Never created or truncated: in|out opens an existing file only.
    storageFile.open(storagePath, std::ios::in | std::ios::out | std::ios::binary);
    if (!storageFile.is_open()) {
        storageFile.open(storagePath, std::ios::in | std::ios::binary);
        if (!storageFile.is_open()) fail("cannot be opened for reading");
        storageReadOnly = true;
    }
    storageFile.read(reinterpret_cast<char*>(mem.data()), TOTAL_SIZE_BYTES);
    if (storageFile.gcount() != static_cast<std::streamsize>(TOTAL_SIZE_BYTES)) fail("could not be read completely");
    LogInfo("OTP: loaded %s image %s", storageReadOnly ? "read-only" : "writable", storagePath.c_str());
    if (storageReadOnly) LogWarn("OTP: image is read-only; guest OTP writes are kept in memory and discarded on exit");
}

void OTP::LoadFromFile()
{
    storageFile.seekg(0, std::ios::beg);
    storageFile.read(reinterpret_cast<char*>(mem.data()), TOTAL_SIZE_BYTES);
}

void OTP::SavePageToFile(u16 page)
{
    if (page >= NUM_PAGES) return;

    if (!storageFile.is_open()) {
        return; // Only in-memory storage
    }
    if (storageReadOnly) {
        if (!readOnlyWriteLogged) LogWarn("OTP: guest wrote page 0x%03X; not saved to the read-only image", page);
        readOnlyWriteLogged = true;
        return;
    }

    u64 pageOffset = static_cast<u64>(page) * PAGE_SIZE_BYTES;
    storageFile.seekp(pageOffset, std::ios::beg);
    storageFile.write(reinterpret_cast<const char*>(&mem[page * PAGE_SIZE_WORDS]), PAGE_SIZE_BYTES);
    storageFile.flush();

    if (!storageFile) {
        storageFile.clear();
    }
}