#pragma once

// bcore headers (from ext/bcore/include/)
#include "mem.h"

// op1emu headers
#include "emu.h"
struct CpuState;

class EmulatorMemory : public Memory {
public:
    explicit EmulatorMemory(Emulator& emulator, const CpuState* cpu = nullptr);

    uint32_t base() const override;
    uint32_t size() const override;
    uintptr_t fast_base() const override;

    uint8_t  read8(uint32_t addr) const override;
    uint16_t read16(uint32_t addr) const override;
    uint32_t read32(uint32_t addr) const override;

    void write8(uint32_t addr, uint8_t val) override;
    void write16(uint32_t addr, uint16_t val) override;
    void write32(uint32_t addr, uint32_t val) override;

    const uint8_t* raw() const override;

    uint32_t rawmem_limit() const override { return 0; }
    void DumpRecentMMIO() const;

private:
    Emulator& emulator_;
    void ObserveClock(uint32_t address, uint32_t value, unsigned width) const;
    const CpuState* cpu_;
    bool traceClock_ = false;
    mutable unsigned clockEvents_ = 0;
    struct MMIOEntry { uint32_t pc, address, value; unsigned width; bool write; };
    mutable std::array<MMIOEntry, 32> recentMMIO_{};
    mutable unsigned recentCount_ = 0;
    mutable unsigned recentIndex_ = 0;
    void RecordMMIO(uint32_t address, uint32_t value, unsigned width, bool write) const;
};
