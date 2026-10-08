#include "cpu/pll.h"
#include "cpu/sic.h"
#include <cstdio>
#include <cstdlib>

namespace {
void Check(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "PLL/IDLE: %s\n", message); std::exit(1); }
}
void Program(BF524PLL& p, u32 ctl) { p.Write32(0, ctl); Check(p.EnterIdle(), "pending programming"); }
}

int main() {
    BF524PLL p(25000000); // Explicit PROVISIONAL experimental assumption.
    Check(p.Read32(0) == 0x0B00 && p.Read32(4) == 4 && p.Read32(8) == 0x70B0 &&
          p.Read32(12) == 0xA2 && p.Read32(16) == 512, "BF524 reset readbacks");
    Check(!p.ActiveCtl() && !p.CoreHz() && !p.EnterIdle(), "no inferred reset active clock or fabricated deadline");
    p.Write32(12, 0);
    Check(p.Read32(12) == 0xA2, "status is read-only");
    const u16 div = 5;
    p.Write(4, &div, 2);
    u16 readback = 0;
    p.Read(4, &readback, 2);
    Check(readback == 5, "16-bit MMR access");
    p.Write32(0, 0x2000);
    Check(p.Read32(0) == 0x2000 && !p.ActiveCtl() && !p.NextDeadlineTicks(), "request is not activation");
    p.AdvanceClkin(100);
    Check(p.EnterIdle() && p.ActiveCtl() == 0x2000 && p.NextDeadlineTicks() == 512 &&
          !p.WakeAsserted() && !(p.Status() & 0x20) && p.CoreHz() == 25000000,
          "IDLE starts documented lock countdown in bypass");
    p.AdvanceClkin(511);
    Check(p.NextDeadlineTicks() == 1 && !p.WakeAsserted(), "boundary before lock");
    p.AdvanceClkin(1);
    Check(!p.NextDeadlineTicks() && p.WakeAsserted() && p.Status() == 0xA2 &&
          p.CoreHz() == 400000000 && p.SystemHz() == 80000000, "exact lock condition and provisional frequencies");
    p.AcknowledgeWake();
    p.Write32(4, 0x25);
    Check(p.CoreHz() == 100000000 && p.SystemHz() == 80000000 && !p.NextDeadlineTicks(), "dynamic divider without relock");
    p.Write32(4, 0x31);
    Check(p.SystemHz() == 50000000, "effective SSEL raised to CCLK divider");
    p.Write32(4, 5);
    Program(p, 0x2000);
    Check(p.WakeAsserted() && p.NextDeadlineTicks() == 512 && (p.Status() & 0x20) &&
          p.CoreHz() == 25000000, "same-value wake is not multiplied-clock restoration");
    p.AcknowledgeWake();
    p.AdvanceClkin(512);
    Check(!p.WakeAsserted() && p.CoreHz() == 400000000, "same-value bypass expiry has no second wake");
    Program(p, 0x2102); // Active PLL-disabled.
    Check(p.WakeAsserted() && p.Status() == 0x88 && p.CoreHz() == 25000000, "PLL disabled in bypass");
    p.AcknowledgeWake();
    Program(p, 0x2000);
    Check(p.NextDeadlineTicks() == 512 && !p.WakeAsserted(), "power reapplication requires lock");
    p.AdvanceClkin(1000);
    Check(p.WakeAsserted() && !p.NextDeadlineTicks(), "large deterministic advance crosses lock once");
    p.AcknowledgeWake();
    p.Write32(8, 0x30B0); // Clock buffer off, no BF524 voltage ramp.
    Check(p.ActiveVR() == 0x70B0 && p.EnterIdle() && p.ActiveVR() == 0x30B0 &&
          p.WakeAsserted() && !p.NextDeadlineTicks(), "BF524 normal VR clock-buffer request commits without fabricated voltage settling");
    BF524PLL zero;
    zero.Write32(16, 0);
    Program(zero, 0x2000);
    Check(zero.WakeAsserted() && !zero.NextDeadlineTicks() && !zero.CoreHz(), "zero lock count, no implicit CLKIN assumption");
    BF524PLL masks;
    masks.Write32(0, 0xFFFF); masks.Write32(4, 0xFFFF); masks.Write32(8, 0xFFFF); masks.Write32(16, 0x12345);
    Check(masks.Read32(0) == 0x7FEB && masks.Read32(4) == 0x3F && masks.Read32(8) == 0xF7F0 &&
          masks.Read32(16) == 0x2345, "documented fields and reserved-bit masks");
    BF524PLL unsupported;
    Program(unsupported, 0x2028);
    Check(unsupported.Unsupported() && !unsupported.WakeAsserted() && !unsupported.NextDeadlineTicks(), "unsupported PLL power modes are explicit");
    BF524PLL hibernate;
    hibernate.Write32(8, 0);
    Check(hibernate.EnterIdle() && hibernate.Unsupported() && !hibernate.WakeAsserted(), "no BF523 internal-regulator transplant or invented hibernate wake");
    BF524PLL romVR(25000000);
    romVR.Write32(8, 0x70B0); // Actual first programming sequence in controlled trial.
    Check(romVR.EnterIdle() && romVR.WakeAsserted() && romVR.NextDeadlineTicks() == 512 &&
          romVR.Status() == 0xA1 && romVR.CoreHz() == 25000000 && romVR.SystemHz() == 25000000 &&
          !romVR.ActiveCtl(), "same reset VR write: immediate wake and known bypass, no invented reset multiplier");
    romVR.AcknowledgeWake();
    romVR.AdvanceClkin(512);
    Check(!romVR.WakeAsserted() && !romVR.CoreHz() && romVR.Status() == 0xA2,
          "reset multiplier still unknown after same-VR bypass expires");
    BF524PLL awake(25000000);
    awake.Write32(8, 0x70B0); awake.EnterIdle(); awake.AcknowledgeWake();
    Check(awake.AdvanceAwakeBypass(512, true, false).consumed == 0 &&
          awake.AdvanceAwakeBypass(512, false, true).consumed == 0 &&
          awake.AdvanceAwakeBypass(0, true, true).consumed == 0 && awake.ClkinTicks() == 0,
          "no experiment, idle, or no new cycles never consumes awake ticks");
    const auto exact = awake.AdvanceAwakeBypass(512, true, true);
    Check(exact.consumed == 512 && exact.overshoot == 0 && exact.expired &&
          !awake.CanAdvanceAwakeBypass(true) && !awake.CoreHz(), "exact bounded expiry without inventing post-bypass clock");
    Check(!awake.AdvanceAwakeBypass(1000, true, true).expired && awake.ClkinTicks() == 512,
          "no second expiry or conversion after deadline");
    BF524PLL noClock;
    noClock.Write32(8, 0x70B0); noClock.EnterIdle(); noClock.AcknowledgeWake();
    Check(!noClock.CanAdvanceAwakeBypass(true) && noClock.AdvanceAwakeBypass(512, true, true).consumed == 0,
          "provisional clock must be explicit even in known logical bypass");
    SIC sic(0xFFC00100);
    sic.SetInterruptForwardCallback([](int, int) { Check(false, "masked SIC must not deliver CEC interrupt"); });
    sic.SetInterruptLevel(0, 1);
    Check(sic.WakePending(), "SIC wake independent of SIC/CEC interrupt masks");
    sic.Write32(0x24, 0);
    Check(!sic.WakePending(), "SIC IWR gates PLL wake");
    sic.Write32(0x24, 1);
    Check(sic.WakePending(), "pending source can wake when enabled");
    sic.SetInterruptLevel(0, 0);
    Check(!sic.WakePending(), "completion acknowledgment removes wake");
    std::puts("PLL/IDLE: deterministic state, countdown and wake tests passed");
}
