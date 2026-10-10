#pragma once

#include <cstdint>

// One guest access to a memory-mapped register (0xFFC00000 and up), as seen by
// EmulatorMemory. Used for exact execution-state comparison
// (docs/multiblock-execution.md).
struct MMIOAccess {
    uint32_t pc, address, value;
    unsigned width;
    bool write;
    bool operator==(const MMIOAccess& o) const {
        return pc == o.pc && address == o.address && value == o.value && width == o.width && write == o.write;
    }
};
