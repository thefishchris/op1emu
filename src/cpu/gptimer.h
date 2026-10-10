#pragma once

#include "io.h"
#include <array>
#include <memory>

class GPTimerImpl;

enum GPTimerClockType {
    GPTimerClockTypeSCLK = 0,
    GPTimerClockTypeTACLK = 1,
    GPTimerClockTypeTMRCLK = 2,
};

class GPTimer : public RegisterDevice {
public:
    // ProcessWithInterrupt() is the inherited no-op, so skipping it changes nothing.
    bool ServiceIsNoOp() override { return true; }
    // The CPU loop also calls Tick() on host-time pacing; that is a no-op
    // while no timer is running (GPTimerImpl::Tick returns at once).
    bool TicksAreNoOp() const;
    GPTimer(u32 baseAddr);
    ~GPTimer();

    u32 Read32(u32 offset) override;
    void Write32(u32 offset, u32 value) override;

    void BindInterrupt(int id, int q, InterruptHandler callback);

    void Tick(GPTimerClockType clockType);

private:
    std::array<std::shared_ptr<GPTimerImpl>, 8> timers;
};