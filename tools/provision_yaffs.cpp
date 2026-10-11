// Offline tool: place a raw mkyaffs2image image into an erased region of an
// emulator MT29F4G08 NAND image (docs/yaffs-provisioning.md). Never modifies
// its inputs and never overwrites an existing output.
#include "cpu/nand_ecc.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>

namespace {
namespace fs = std::filesystem;

// Emulator image layout (src/peripheral/MT29F4G08.cpp): all page data, then all OOB.
constexpr std::uint64_t kPageSize = 2048, kOobSize = 64, kPagesPerBlock = 64, kBlocks = 4096;
constexpr std::uint64_t kPages = kBlocks * kPagesPerBlock;
constexpr std::uint64_t kDataSize = kPages * kPageSize;            // 536870912
constexpr std::uint64_t kImageSize = kDataSize + kPages * kOobSize; // 553648128
// mkyaffs2image record: one chunk plus its spare, whose first 28 bytes are the
// packed tags2 (tags and tag ECC) and the rest 0xFF.
constexpr std::uint64_t kRecordSize = kPageSize + kOobSize;
constexpr std::uint64_t kTagsSize = 28, kEccSize = 32;

std::uint64_t ParseDecimal(const char* text, const char* name) {
    const std::string value(text);
    if (value.empty() || value.size() > 9 || value.find_first_not_of("0123456789") != std::string::npos)
        throw std::runtime_error(std::string(name) + " must be a decimal integer, got '" + value + "'");
    return std::stoull(value);
}

void RequireRegularFile(const fs::path& path, const char* role) {
    std::error_code error;
    const auto status = fs::status(path, error);
    if (status.type() == fs::file_type::not_found) throw std::runtime_error(std::string(role) + " " + path.string() + " does not exist");
    if (error || !fs::is_regular_file(status)) throw std::runtime_error(std::string(role) + " " + path.string() + " is not a regular file");
}

bool Erased(const std::uint8_t* bytes, std::size_t length) {
    return std::all_of(bytes, bytes + length, [](std::uint8_t b) { return b == 0xFF; });
}

int Run(int argc, char** argv, std::optional<fs::path>& createdOutput) {
    if (argc != 7)
        throw std::runtime_error("usage: provision_yaffs BASE.img RAW.yaffs2 NEW.img START-BLOCK TAGS-OFFSET ECC-OFFSET");
    const fs::path basePath(argv[1]), rawPath(argv[2]), outputPath(argv[3]);
    const auto block = ParseDecimal(argv[4], "START-BLOCK");
    const auto tagsOffset = ParseDecimal(argv[5], "TAGS-OFFSET");
    const auto eccOffset = ParseDecimal(argv[6], "ECC-OFFSET");
    if (block >= kBlocks) throw std::runtime_error("START-BLOCK must be below " + std::to_string(kBlocks));
    if (tagsOffset + kTagsSize > kOobSize || eccOffset + kEccSize > kOobSize)
        throw std::runtime_error("TAGS-OFFSET + 28 and ECC-OFFSET + 32 must fit in the 64-byte OOB");
    if (!(tagsOffset + kTagsSize <= eccOffset || eccOffset + kEccSize <= tagsOffset))
        throw std::runtime_error("the tags and ECC ranges overlap in the OOB");

    RequireRegularFile(basePath, "base image");
    RequireRegularFile(rawPath, "YAFFS image");
    std::error_code error;
    if (fs::symlink_status(outputPath, error).type() != fs::file_type::not_found)
        throw std::runtime_error("output " + outputPath.string() + " already exists; choose a new path (existing images are never overwritten)");
    const auto baseSize = fs::file_size(basePath);
    if (baseSize != kImageSize)
        throw std::runtime_error("base image is " + std::to_string(baseSize) + " bytes; expected " + std::to_string(kImageSize) +
                                 " (MT29F4G08 data then OOB layout)");
    const auto rawSize = fs::file_size(rawPath);
    if (rawSize == 0 || rawSize % kRecordSize != 0)
        throw std::runtime_error("YAFFS image is " + std::to_string(rawSize) + " bytes; expected a nonzero multiple of " +
                                 std::to_string(kRecordSize) + " (2048-byte chunk + 64-byte spare)");
    const auto records = rawSize / kRecordSize;
    if (records > kPages - block * kPagesPerBlock)
        throw std::runtime_error("YAFFS image has " + std::to_string(records) + " records, more than the pages from block " +
                                 std::to_string(block) + " to the end of the device");

    // Validate everything before creating the output.
    std::ifstream base(basePath, std::ios::binary);
    std::ifstream source(rawPath, std::ios::binary);
    if (!base || !source) throw std::runtime_error("cannot open the inputs for reading");
    std::array<std::uint8_t, kRecordSize> record{};
    std::array<std::uint8_t, kPageSize> oldData{};
    std::array<std::uint8_t, kOobSize> oldOob{};
    for (std::uint64_t i = 0; i < records; ++i) {
        if (!source.read(reinterpret_cast<char*>(record.data()), static_cast<std::streamsize>(record.size())))
            throw std::runtime_error("YAFFS read failed at record " + std::to_string(i));
        if (!Erased(record.data(), record.size()) && !Erased(record.data() + kPageSize + kTagsSize, kOobSize - kTagsSize))
            throw std::runtime_error("YAFFS record " + std::to_string(i) +
                                     " has spare data after the 28-byte packed tags; only the mkyaffs2image layout is supported");
        const auto page = block * kPagesPerBlock + i;
        base.seekg(static_cast<std::streamoff>(page * kPageSize));
        if (!base.read(reinterpret_cast<char*>(oldData.data()), kPageSize)) throw std::runtime_error("base read failed");
        base.seekg(static_cast<std::streamoff>(kDataSize + page * kOobSize));
        if (!base.read(reinterpret_cast<char*>(oldOob.data()), kOobSize)) throw std::runtime_error("base OOB read failed");
        if (!Erased(oldData.data(), oldData.size()) || !Erased(oldOob.data(), oldOob.size()))
            throw std::runtime_error("target region is not erased at page " + std::to_string(page));
    }

    if (!fs::copy_file(basePath, outputPath, fs::copy_options::none)) throw std::runtime_error("output copy failed");
    createdOutput = outputPath;
    source.clear();
    source.seekg(0);
    std::fstream output(outputPath, std::ios::binary | std::ios::in | std::ios::out);
    if (!output) throw std::runtime_error("cannot open the output for writing");
    std::array<std::uint8_t, kOobSize> oob{};
    std::uint64_t written = 0;
    for (std::uint64_t i = 0; i < records; ++i) {
        if (!source.read(reinterpret_cast<char*>(record.data()), static_cast<std::streamsize>(record.size())))
            throw std::runtime_error("YAFFS read failed at record " + std::to_string(i));
        if (Erased(record.data(), record.size())) continue; // block padding stays erased
        oob.fill(0xFF);
        std::copy_n(record.begin() + kPageSize, kTagsSize, oob.begin() + static_cast<std::ptrdiff_t>(tagsOffset));
        for (unsigned sector = 0; sector < 8; ++sector) {
            auto [odd, even] = ComputeEccPair(record.data() + sector * 256, 256, 0);
            // Confirmed against all eight sectors of a guest-written
            // checkpoint page: two LE 16-bit NFC parity registers/sector.
            auto packed = std::uint32_t{odd} | (std::uint32_t{even} << 16);
            for (unsigned b = 0; b < 4; ++b) oob[eccOffset + sector * 4 + b] = static_cast<std::uint8_t>(packed >> (b * 8));
        }
        const auto page = block * kPagesPerBlock + i;
        output.seekp(static_cast<std::streamoff>(page * kPageSize));
        output.write(reinterpret_cast<const char*>(record.data()), kPageSize);
        output.seekp(static_cast<std::streamoff>(kDataSize + page * kOobSize));
        output.write(reinterpret_cast<const char*>(oob.data()), kOobSize);
        if (!output) throw std::runtime_error("output write failed");
        ++written;
    }
    if (source.peek() != std::char_traits<char>::eof()) throw std::runtime_error("YAFFS input grew during conversion");
    output.flush();
    if (!output) throw std::runtime_error("output flush failed");
    createdOutput.reset();
    std::cout << "Provisioned " << records << " records (" << written << " written, " << records - written
              << " erased) at block " << block << "; tags=" << tagsOffset << " ECC=" << eccOffset << '\n';
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    std::optional<fs::path> createdOutput;
    try {
        return Run(argc, argv, createdOutput);
    } catch (const std::exception& e) {
        std::cerr << "provision_yaffs: " << e.what() << '\n';
        if (createdOutput) {
            std::error_code ignored;
            fs::remove(*createdOutput, ignored); // never leave a partial image behind
        }
        return 1;
    }
}
