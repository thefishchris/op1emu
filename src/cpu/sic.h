#pragma once

#include "io.h"
#include <cstdint>
#include <functional>

class SIC : public RegisterDevice {
public:
    // ProcessWithInterrupt() is the inherited no-op, so skipping it changes nothing.
    bool ServiceIsNoOp() override { return true; }
    SIC(u32 baseAddr);

    // Level-sensitive model (docs/sic-interrupt-delivery.md). Peripheral
    // inputs are levels held in SIC_ISR. The SIC drives one line per IVG:
    // the OR of asserted, SIC_IMASK-enabled sources routed there by SIC_IARx.
    // The callback is invoked with level 1 once per rising edge of a line; an
    // unchanged level, mask or routing produces nothing.
    void SetInterruptForwardCallback(std::function<void(int ivg, int level)> callback) {
        forwardInterrupt = callback;
    }

    // Called when a peripheral raises/lowers an interrupt
    void SetInterruptLevel(int pin, int level);
    // Whether the line into the CEC for `ivg` is asserted.
    bool LineAsserted(int ivg) const { return ivg >= 0 && ivg < 32 && (lines & (1u << ivg)) != 0; }

    struct Stats { uint64_t levelWrites = 0, levelChanges = 0, lineRises = 0; };
    const Stats& Counters() const { return stats; }
    bool WakePending() const {
        return ((isr[0] & iwr[0]) | (isr[1] & iwr[1])) != 0;
    }

    void SetBootMode(u8 mode) { bmode = mode; }

private:
    void InitRegisters();
    // Recompute the IVG lines; forward each rising edge.
    void UpdateLines();

    u16 rvect = 0;
    u8 bmode = 0;
    u32 imask[2] = {0};
    u32 iar[8] = {0};
    u32 isr[2] = {0};
    u32 iwr[2] = {0};
    u32 lines = 0; // Bit n: line into IVGn asserted.
    Stats stats;

    std::function<void(int ivg, int level)> forwardInterrupt;
};
