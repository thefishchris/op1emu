#pragma once

#include "io.h"

class EBIU : public RegisterDevice {
public:
    // ProcessWithInterrupt() is the inherited no-op, so skipping it changes nothing.
    bool ServiceIsNoOp() override { return true; }
    EBIU(u32 baseAddr);

private:
    int type;

    // Register values
    u32 sdgctl;
    u16 sdbctl;
    u16 sdrrc;
    u16 sdstat;
    u16 sdstat_sdease;
};