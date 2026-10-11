// Runtime boot ROM (docs/boot-rom.md): image validation, the CPU's
// OP1EMU_BOOTROM_FILE selection, and the absent-ROM stub. All images here are
// synthetic; no real ROM content is used.
#include "cpu/bootrom.h"
#include "cpu/cpu.h"
#include "cpu/nand.h"
#include "utils/sha256.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {
void Check(bool condition, const std::string& what) {
    if (!condition) {
        std::fprintf(stderr, "boot rom: %s\n", what.c_str());
        std::exit(1);
    }
}

class ScopedEnv {
public:
    ScopedEnv(const char* name, const char* value) : name_(name) {
        if (const char* old = std::getenv(name)) old_ = old;
        if (value) setenv(name, value, 1); else unsetenv(name);
    }
    ~ScopedEnv() { if (old_) setenv(name_, old_->c_str(), 1); else unsetenv(name_); }

private:
    const char* name_;
    std::optional<std::string> old_;
};

class IdleNand final : public NandFlash {
public:
    void SendCommand(u8) override { Check(false, "unexpected NAND command"); }
    void SendAddress(u8) override { Check(false, "unexpected NAND address"); }
    u8 ReadData() override { Check(false, "unexpected NAND read"); return 0; }
    void WriteData(u8) override { Check(false, "unexpected NAND write"); }
    bool IsDataReady() const override { return false; }
    bool IsBusy() const override { return false; }
    void StartPageRead() override { Check(false, "unexpected page read"); }
    void StartPageWrite() override { Check(false, "unexpected page write"); }
    u32 PageRead(u8*, u32) override { Check(false, "unexpected DMA read"); return 0; }
    u32 PageWrite(const u8*, u32) override { Check(false, "unexpected DMA write"); return 0; }
    void SetReadCallback(ReadCallback) override {}
};

std::vector<u8> SyntheticRom() {
    std::vector<u8> bytes(kBootRomSize);
    for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<u8>(i * 13 + 7);
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

bool RejectsWith(const std::string& path, const std::string& expected) {
    try {
        LoadBootRomImage(path);
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        Check(message.find(expected) != std::string::npos, "message lacks '" + expected + "': " + message);
        Check(message.find("32768") != std::string::npos, "message lacks the required size: " + message);
        return true;
    }
    return false;
}

void Sha256MatchesKnownVectors() {
    Check(Sha256Hex(nullptr, 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "SHA-256 of empty input");
    const std::string abc = "abc";
    Check(Sha256Hex(reinterpret_cast<const u8*>(abc.data()), abc.size()) ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 of 'abc'");
    const std::string two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    Check(Sha256Hex(reinterpret_cast<const u8*>(two.data()), two.size()) ==
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", "SHA-256 of the two-block vector");
}

void ValidImageLoadsUnchanged(const fs::path& dir) {
    const auto path = dir / "synthetic.rom";
    const auto bytes = SyntheticRom();
    WriteFile(path, bytes);
    const auto image = LoadBootRomImage(path.string());
    Check(image.bytes == bytes, "a valid image loads byte for byte");
    Check(image.sha256 == Sha256Hex(bytes.data(), bytes.size()), "the image digest is its SHA-256");
    Check(!image.validatedRevision, "a synthetic image is not the validated revision");
    Check(ReadFile(path) == bytes, "loading never modifies the image");
}

void OnlyTheValidatedDigestIsRecognised() {
    Check(IsValidatedBootRomDigest(kValidatedBootRomSha256), "the validated digest is recognised");
    Check(std::string(kValidatedBootRomSha256).size() == 64, "the validated digest is a SHA-256");
    std::string other = kValidatedBootRomSha256;
    other[0] = other[0] == '0' ? '1' : '0';
    Check(!IsValidatedBootRomDigest(other), "any other digest is an unvalidated revision");
}

void ReadOnlyImageLoads(const fs::path& dir) {
    const auto path = dir / "read-only.rom";
    const auto bytes = SyntheticRom();
    WriteFile(path, bytes);
    fs::permissions(path, fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read);
    Check(LoadBootRomImage(path.string()).bytes == bytes, "a read-only image loads");
    Check(ReadFile(path) == bytes, "a read-only image is unchanged");
    fs::permissions(path, fs::perms::owner_write, fs::perm_options::add);
}

void InvalidImagesAreRejected(const fs::path& dir) {
    const auto missing = dir / "missing.rom";
    Check(RejectsWith(missing.string(), "does not exist"), "a missing image is rejected");
    Check(!fs::exists(missing), "a missing image is not created");
    Check(RejectsWith("", "empty path"), "an empty path is rejected");
    Check(RejectsWith(dir.string(), "not a regular file"), "a directory is rejected");
    for (std::size_t size : {std::size_t{0}, std::size_t{kBootRomSize - 1}, std::size_t{kBootRomSize + 1}}) {
        const auto path = dir / ("size-" + std::to_string(size) + ".rom");
        std::vector<u8> bytes(size, 0x5A);
        WriteFile(path, bytes);
        Check(RejectsWith(path.string(), "is " + std::to_string(size) + " bytes"), "a " + std::to_string(size) + "-byte image is rejected");
        Check(ReadFile(path) == bytes, "a rejected image is unchanged");
    }
    if (geteuid() == 0) {
        std::puts("boot rom: skipping unreadable-file check as root");
        return;
    }
    const auto unreadable = dir / "unreadable.rom";
    WriteFile(unreadable, SyntheticRom());
    fs::permissions(unreadable, fs::perms::none);
    Check(RejectsWith(unreadable.string(), "cannot be opened for reading"), "an unreadable image is rejected");
    fs::permissions(unreadable, fs::perms::owner_read | fs::perms::owner_write);
}

void CpuLoadsSelectedImage(const fs::path& dir) {
    const auto path = dir / "cpu.rom";
    const auto bytes = SyntheticRom();
    WriteFile(path, bytes);
    ScopedEnv rom("OP1EMU_BOOTROM_FILE", path.c_str());
    BlackFinCpu cpu;
    Check(cpu.BootRomLoaded() && !cpu.BootRomValidated(), "the CPU loads an unvalidated synthetic image");
    const u32 word = static_cast<u32>(bytes[4]) | static_cast<u32>(bytes[5]) << 8 |
                     static_cast<u32>(bytes[6]) << 16 | static_cast<u32>(bytes[7]) << 24;
    Check(cpu.GetEmulator().MemoryRead32(kBootRomBase + 4) == word, "the image is visible at 0xEF000000");
    Check(!cpu.Failed(), "reading a loaded ROM is not a failure");
}

void CpuRejectsInvalidSelection(const fs::path& dir) {
    const auto path = dir / "cpu-missing.rom";
    ScopedEnv rom("OP1EMU_BOOTROM_FILE", path.c_str());
    bool rejected = false;
    try {
        BlackFinCpu cpu;
    } catch (const std::runtime_error& error) {
        rejected = std::string(error.what()).rfind("OP1EMU_BOOTROM_FILE: ", 0) == 0;
    }
    Check(rejected, "an invalid OP1EMU_BOOTROM_FILE stops CPU construction with a named error");
}

void AbsentRomStopsResetVector() {
    ScopedEnv rom("OP1EMU_BOOTROM_FILE", nullptr);
    BlackFinCpu cpu;
    cpu.AttachNandFlash(std::make_shared<IdleNand>());
    Check(!cpu.BootRomLoaded(), "no image means no ROM");
    cpu.SetPC(kBootRomBase);
    cpu.Run();
    Check(cpu.Failed(), "execution at the reset vector stops without a ROM");
    Check(cpu.PacketEntryCount() == 0, "no instruction of absent ROM memory executes");
}

// A program at 0x100 returns into the ROM: NOP; RTS with RETS = 0xEF000018.
void ReturnIntoAbsentRom(bool multiBlock) {
    ScopedEnv rom("OP1EMU_BOOTROM_FILE", nullptr);
    ScopedEnv profile("OP1EMU_PROFILE", multiBlock ? "functional" : nullptr);
    BlackFinCpu cpu;
    cpu.AttachNandFlash(std::make_shared<IdleNand>());
    if (multiBlock) cpu.SetMultiBlock(true, false);
    cpu.GetEmulator().MemoryWrite16(0x100, 0x0000); // NOP
    cpu.GetEmulator().MemoryWrite16(0x102, 0x0010); // RTS
    cpu.SetRegister(RegIndex::RETS, kBootRomBase + 0x18);
    cpu.SetPC(0x100);
    const std::string mode = multiBlock ? "multi-block" : "single-block";
    uint64_t before = 0;
    for (int i = 0; i < 8 && !cpu.Failed(); ++i) {
        before = cpu.PacketEntryCount();
        cpu.Run();
    }
    Check(cpu.Failed(), mode + ": returning into an absent ROM stops");
    Check(cpu.PC() == kBootRomBase + 0x18, mode + ": the stop is at the ROM entry");
    Check(cpu.PacketEntryCount() == before, mode + ": nothing executes once the ROM is entered");
    Check(cpu.PacketEntryCount() >= 2, mode + ": the program before the ROM entry ran");
}

// R0 = [P1] with P1 in the ROM: a data load from absent ROM stops the CPU.
void LoadFromAbsentRom() {
    ScopedEnv rom("OP1EMU_BOOTROM_FILE", nullptr);
    BlackFinCpu cpu;
    cpu.AttachNandFlash(std::make_shared<IdleNand>());
    cpu.GetEmulator().MemoryWrite16(0x100, 0x9108); // R0 = [P1]
    cpu.GetEmulator().MemoryWrite16(0x102, 0x2000); // JUMP.S 0
    cpu.SetRegister(RegIndex::P1, kBootRomBase + 0x40);
    cpu.SetPC(0x100);
    cpu.Run();
    Check(cpu.Failed(), "a load from absent ROM stops the CPU");
}

void BusAccessToAbsentRom() {
    ScopedEnv rom("OP1EMU_BOOTROM_FILE", nullptr);
    BlackFinCpu cpu;
    cpu.GetEmulator().MemoryRead32(kBootRomBase + 0x100);
    Check(cpu.Failed(), "a bus read of absent ROM stops the CPU");
}

// LDR-style execution that never enters the ROM runs without an image.
void ProgramOutsideRomRuns() {
    ScopedEnv rom("OP1EMU_BOOTROM_FILE", nullptr);
    BlackFinCpu cpu;
    cpu.AttachNandFlash(std::make_shared<IdleNand>());
    cpu.GetEmulator().MemoryWrite16(0x100, 0x0000); // NOP
    cpu.GetEmulator().MemoryWrite16(0x102, 0x2000); // JUMP.S 0 (to itself)
    cpu.SetPC(0x100);
    for (int i = 0; i < 16; ++i) cpu.Run();
    Check(!cpu.Failed(), "code outside the ROM runs without an image");
    Check(cpu.PacketEntryCount() > 2, "the program kept running");
}
} // namespace

int main() {
    const auto dir = fs::temp_directory_path() / ("op1emu-bootrom-" + std::to_string(getpid()));
    fs::remove_all(dir);
    fs::create_directory(dir);
    Sha256MatchesKnownVectors();
    ValidImageLoadsUnchanged(dir);
    OnlyTheValidatedDigestIsRecognised();
    ReadOnlyImageLoads(dir);
    InvalidImagesAreRejected(dir);
    CpuLoadsSelectedImage(dir);
    CpuRejectsInvalidSelection(dir);
    AbsentRomStopsResetVector();
    ReturnIntoAbsentRom(false);
    ReturnIntoAbsentRom(true);
    LoadFromAbsentRom();
    BusAccessToAbsentRom();
    ProgramOutsideRomRuns();
    fs::remove_all(dir);
    std::puts("boot rom tests passed");
    return 0;
}
