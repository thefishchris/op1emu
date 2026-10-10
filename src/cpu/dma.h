#pragma once

#include "io.h"
#include "emu.h"
#include <vector>
#include <map>
#include <memory>
#include <optional>

class Bus;

class DMABus {
public:
    virtual ~DMABus() {}

    virtual u32 DMARead(int x, int y, void* dest, u32 length) = 0;
    virtual u32 DMAWrite(int x, int y, const void* source, u32 length) = 0;
    // Discard any buffered data. MDMA's internal FIFO is flushed when a new
    // transfer starts; peripheral buses ignore this.
    virtual void DMAFlush() {}
    // False only when a transfer in this direction (toMemory: peripheral to
    // memory) is certain to move nothing now and the bus call would have no
    // side effect, so the channel may skip its staging copy and the call.
    virtual bool TransferPossible(bool toMemory) const { return true; }
};

// MDMA source/destination channels are just two ordinary DMA channels
// connected through an internal FIFO: the source channel does memory->FIFO
// (WNR=0), the destination channel does FIFO->memory (WNR=1). Both sides run
// through the normal DMAChannel::ProcessTransfer(), so this bus only needs to
// hold bytes in flight; it decouples the two channels' independent 1D/2D
// stride advancement and provides backpressure via short reads/writes.
class MemorySrcDMABus : public DMABus {
public:
    MemorySrcDMABus() {}
    u32 DMARead(int x, int y, void* dest, u32 length) override;
    u32 DMAWrite(int x, int y, const void* source, u32 length) override;
    void DMAFlush() override { head = tail = 0; }

protected:
    static constexpr u32 CAPACITY = 4096; // matches ProcessTransfer's staging buffer
    u8 buffer[CAPACITY];
    u32 head = 0; // dequeue offset
    u32 tail = 0; // enqueue offset
};

class MemoryDestDMABus : public DMABus {
public:
    MemoryDestDMABus(MemorySrcDMABus& source) : source(source) {}
    u32 DMARead(int x, int y, void* dest, u32 length) override;
    u32 DMAWrite(int x, int y, const void* source, u32 length) override;
    void DMAFlush() override { source.DMAFlush(); }

protected:
    MemorySrcDMABus& source;
};

enum DMAPeripheralType {
    DMAPeripheralPPI = 0x0,
    DMAPeripheralHOSTDP = 0x1,
    DMAPeripheralNFC = 0x2,
    DMAPeripheralSPORT0Rx = 0x3,
    DMAPeripheralSPORT0Tx = 0x4,
    DMAPeripheralSPORT1Rx = 0x5,
    DMAPeripheralSPORT1Tx = 0x6,
    DMAPeripheralSPI = 0x7,
    DMAPeripheralUART0Rx = 0x8,
    DMAPeripheralUART0Tx = 0x9,
    DMAPeripheralUART1Rx = 0xA,
    DMAPeripheralUART1Tx = 0xB,
    DMAPeripheralMDMADest0 = 0xC,
    DMAPeripheralMDMASrc0 = 0xD,
    DMAPeripheralMDMADest1 = 0xE,
    DMAPeripheralMDMASrc1 = 0xF,
};

class DMAChannel;
class DMA : public RegisterDevice {
public:
    DMA(u32 baseAddr, Emulator& emu);

    void AttachDMABus(DMAPeripheralType type, const std::shared_ptr<DMABus>& bus) {
        dmaBuses[type] = bus;
    }

    void BindInterrupt(int channel, int q, InterruptHandler callback);
    bool ServiceIdleNFCReadCompletion();
    bool ServiceIdleMDMACompletion();
    // Running peripheral channels mapped to `type`: the fewest bytes left in
    // a current row (the next point where completion, an interrupt or a
    // descriptor fetch can occur); nullopt if no such channel runs.
    std::optional<u32> PeripheralRowBytesRemaining(DMAPeripheralType type) const;
    // Run every channel mapped to `type` until none moves data; returns the
    // bytes moved. Rate-limited peripherals only (each call moves only what
    // the peripheral has due).
    uint64_t ServicePeripheral(DMAPeripheralType type);

    std::shared_ptr<DMABus> GetDMABus(DMAPeripheralType type) { return dmaBuses[type]; }
    Emulator& GetEmulator() { return emulator; }

    u32 Read32(u32 offset) override;
    void Write32(u32 offset, u32 value) override;

    void ProcessWithInterrupt(int ivg) override;

    // Channels that can make progress: enabled and running (bit n = channel n).
    // Any other channel's ProcessTransfer() returns 0 without side effects, so
    // the per-block pass skips it. Equivalence tests turn the skip off.
    uint16_t ActiveChannels() const { return activeChannels; }
    void SetActiveChannelTracking(bool enabled) { activeTracking = enabled; }

protected:
    // A channel's enabled/running state changes only through its registers
    // (DMA::Write32, including the CONFIG write callback) or inside its
    // ProcessTransfer() (completion, descriptor fetch). Each such path
    // refreshes the bits.
    void RefreshActive(size_t channel);
    void RefreshAllActive();
    uint16_t activeChannels = 0;
    bool activeTracking = true;

    bool ServiceFinite2DMDMACompletion();
    Emulator& emulator;
    std::array<std::shared_ptr<DMAChannel>, 16> channels;
    std::map<DMAPeripheralType, std::shared_ptr<DMABus>> dmaBuses;
};
