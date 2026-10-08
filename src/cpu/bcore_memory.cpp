#include "bcore_memory.h"
#include "cpu_state.h"
#include "utils/log.h"
#include <cstdlib>
#include <cstring>

EmulatorMemory::EmulatorMemory(Emulator& emulator, const CpuState* cpu)
    : emulator_(emulator), cpu_(cpu) {
    const char* value = std::getenv("OP1EMU_TRACE_CLOCK");
    traceClock_ = value && std::strcmp(value, "1") == 0;
}

void EmulatorMemory::ObserveClock(uint32_t address, uint32_t value, unsigned width) const {
    if (!traceClock_ || clockEvents_ >= 128 || address < 0xFFC00000 || address > 0xFFC00010 || (address & 3u)) return;
    ++clockEvents_;
    LogInfo("clock-observe op=write address=%08X width=%u value=%08X pc=%08X packets=%llu mapped=%u",
            address, width, value, cpu_ ? cpu_->pc : 0,
            static_cast<unsigned long long>(cpu_ ? cpu_->packet_entries : 0), emulator_.IsMemoryValid(address) ? 1u : 0u);
}

uint32_t EmulatorMemory::base() const {
    return 0;
}

uint32_t EmulatorMemory::size() const {
    return 0xFFFFFFFF;
}

uintptr_t EmulatorMemory::fast_base() const {
    return 0;
}

uint8_t EmulatorMemory::read8(uint32_t addr) const {
    const auto value = emulator_.MemoryRead8(addr);
    RecordMMIO(addr, value, 8, false);
    return value;
}

uint16_t EmulatorMemory::read16(uint32_t addr) const {
    const auto value = emulator_.MemoryRead16(addr);
    RecordMMIO(addr, value, 16, false);
    return value;
}

uint32_t EmulatorMemory::read32(uint32_t addr) const {
    const auto value = emulator_.MemoryRead32(addr);
    RecordMMIO(addr, value, 32, false);
    return value;
}

void EmulatorMemory::write8(uint32_t addr, uint8_t val) {
    RecordMMIO(addr, val, 8, true);
    if (traceClock_) ObserveClock(addr, val, 8);
    emulator_.MemoryWrite8(addr, val);
}

void EmulatorMemory::write16(uint32_t addr, uint16_t val) {
    RecordMMIO(addr, val, 16, true);
    if (traceClock_) ObserveClock(addr, val, 16);
    emulator_.MemoryWrite16(addr, val);
}

void EmulatorMemory::write32(uint32_t addr, uint32_t val) {
    RecordMMIO(addr, val, 32, true);
    if (traceClock_) ObserveClock(addr, val, 32);
    emulator_.MemoryWrite32(addr, val);
}

const uint8_t* EmulatorMemory::raw() const {
    return nullptr;
}

void EmulatorMemory::RecordMMIO(uint32_t address, uint32_t value, unsigned width, bool write) const {
    if (address < 0xFFC00000 || address >= 0xFFE00000) return;
    recentMMIO_[recentIndex_] = {cpu_ ? cpu_->pc : 0, address, value, width, write};
    recentIndex_ = (recentIndex_ + 1) % 32;
    recentCount_ = std::min(recentCount_ + 1, 32u);
}

void EmulatorMemory::DumpRecentMMIO() const {
    for (unsigned i = 0; i < recentCount_; ++i) {
        const auto& e = recentMMIO_[(recentIndex_ + 32 - recentCount_ + i) % 32];
        LogWarn("idle-recent-mmio pc=%08X op=%s address=%08X width=%u value=%08X",
                e.pc, e.write ? "write" : "read", e.address, e.width, e.value);
    }
}
