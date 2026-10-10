#pragma once

#include "io.h"

class Jtag : public RegisterDevice {
public:
    // ProcessWithInterrupt() is the inherited no-op, so skipping it changes nothing.
    bool ServiceIsNoOp() override { return true; }
    Jtag(u32 baseAddr, u32 dspid);
    ~Jtag() override = default;

private:
    u32 dspid;
};