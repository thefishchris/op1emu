// Deterministic guest clock (docs/guest-clock-architecture.md D1/D2):
// exact ratio arithmetic, PLL-derived ratios, and awake CPU advancement.
#include "cpu/cpu.h"
#include "cpu/guest_clock.h"
#include "cpu/nand.h"
#include "cpu/pll.h"
#include "shadow_timing.h"
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
void Check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "guest clock: %s\n", what);
        std::exit(1);
    }
}

class ScopedEnv {
public:
    ScopedEnv(const char* name, const char* value) : name_(name) {
        if (const char* old = std::getenv(name)) old_ = old;
        Check((value ? setenv(name, value, 1) : unsetenv(name)) == 0, "set test environment");
    }
    ~ScopedEnv() { if (old_) setenv(name_, old_->c_str(), 1); else unsetenv(name_); }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;
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

constexpr ClockRatios kBoundary{16, 1, 5}; // PLL_CTL=0x2000, PLL_DIV=5 (E13).

bool Same(const GuestClock& a, const GuestClock& b) {
    return a.ClkinTicks() == b.ClkinTicks() && a.CclkCycles() == b.CclkCycles() && a.SclkCycles() == b.SclkCycles() &&
           a.CclkWithoutRatio() == b.CclkWithoutRatio() && a.RatioAlignments() == b.RatioAlignments();
}

void InitialAndUnknownRatio() {
    GuestClock c;
    Check(!c.Ratios() && c.ClkinTicks() == 0 && c.CclkCycles() == 0 && c.SclkCycles() == 0 &&
          c.CclkWithoutRatio() == 0 && c.RatioAlignments() == 0 && !c.ClkinTickForCclk(1), "initial state");
    c.AdvanceAwakeCclk(0);
    Check(c.CclkCycles() == 0, "zero work does not advance");
    c.AdvanceAwakeCclk(10);
    Check(c.CclkCycles() == 10 && c.CclkWithoutRatio() == 10 && c.ClkinTicks() == 0 && c.SclkCycles() == 0,
          "unknown ratio counts CCLK but derives no CLKIN/SCLK");
    c.AdvanceIdleClkin(4);
    Check(c.ClkinTicks() == 4 && c.CclkCycles() == 10 && c.SclkCycles() == 0, "idle CLKIN without ratio holds CCLK and SCLK");
}

void ExactRatios() {
    GuestClock bypass;
    bypass.SetRatios(ClockRatios{});
    bypass.AdvanceAwakeCclk(7);
    Check(bypass.ClkinTicks() == 7 && bypass.SclkCycles() == 7, "bypass is 1:1:1");

    GuestClock c;
    c.SetRatios(kBoundary);
    c.AdvanceAwakeCclk(16);
    Check(c.ClkinTicks() == 1 && c.SclkCycles() == 3, "16 CCLK = 1 CLKIN = 3 whole SCLK");
    c.AdvanceAwakeCclk(4);
    Check(c.ClkinTicks() == 1 && c.SclkCycles() == 4, "fractions carried exactly");
    c.AdvanceAwakeCclk(60);
    Check(c.ClkinTicks() == 5 && c.SclkCycles() == 16 && c.CclkCycles() == 80, "80 CCLK = 5 CLKIN = 16 SCLK");

    GuestClock drift;
    drift.SetRatios(kBoundary);
    for (unsigned i = 0; i < 1600000; ++i) drift.AdvanceAwakeCclk(1);
    Check(drift.ClkinTicks() == 100000 && drift.SclkCycles() == 320000 && drift.RatioAlignments() == 0,
          "long run of single cycles does not drift");

    Check(c.ClkinTickForCclk(80) == 5 && c.ClkinTickForCclk(96) == 6 && c.ClkinTickForCclk(97) == 7 &&
          c.ClkinTickForCclk(0) == 5, "deadline conversion rounds up to a CLKIN tick");

    GuestClock big;
    big.SetRatios(ClockRatios{16, 2, 10});
    bool threw = false;
    try { big.AdvanceAwakeCclk(std::numeric_limits<uint64_t>::max()); } catch (const std::overflow_error&) { threw = true; }
    Check(threw, "overflow is detected, not wrapped");
}

void RatioTransitions() {
    GuestClock c;
    c.SetRatios(kBoundary);
    c.AdvanceAwakeCclk(8); // Half a CLKIN tick.
    c.SetRatios(kBoundary);
    Check(c.RatioAlignments() == 0, "re-applying the same ratio is a no-op");
    c.SetRatios(ClockRatios{16, 2, 5}); // PLL_DIV CSEL change: same unit base.
    Check(c.ClkinTicks() == 0 && c.RatioAlignments() == 0, "divider change keeps the CLKIN fraction");
    c.AdvanceAwakeCclk(4); // 8 more units at the new CCLK period.
    Check(c.ClkinTicks() == 1 && c.SclkCycles() == 3, "subsequent conversions use the new ratio");
    c.AdvanceAwakeCclk(3); // 6 units: sub-tick fraction again.
    c.SetRatios(ClockRatios{}); // MSEL/bypass change: unit base changes.
    Check(c.ClkinTicks() == 2 && c.RatioAlignments() >= 1, "base change aligns up to the next CLKIN edge");
    c.AdvanceAwakeCclk(3);
    Check(c.ClkinTicks() == 5, "bypass after alignment is exact");
    c.SetRatios(std::nullopt);
    c.AdvanceAwakeCclk(2);
    Check(c.ClkinTicks() == 5 && c.CclkWithoutRatio() == 2, "losing the ratio stops CLKIN derivation");

    GuestClock idle;
    idle.SetRatios(kBoundary);
    idle.AdvanceAwakeCclk(3);
    idle.AdvanceIdleClkin(10);
    Check(idle.ClkinTicks() == 10 && idle.CclkCycles() == 3 && idle.SclkCycles() == 32,
          "idle CLKIN advances SCLK and holds CCLK, keeping the sub-tick phase");
}

void PllRatios() {
    BF524PLL reset;
    Check(!reset.Ratios(), "reset ratio is not inferred");
    auto program = [](BF524PLL& p, u32 div, u32 ctl) {
        p.Write32(4, div);
        p.Write32(0, ctl);
        Check(p.EnterIdle(), "programming accepted");
        Check(p.Ratios() == ClockRatios{}, "relock runs in temporary bypass (1:1)");
        p.AdvanceClkin(512);
    };
    BF524PLL p;
    program(p, 5, 0x2000);
    Check(p.Ratios() == kBoundary && !p.CoreHz(), "boundary ratio known without any absolute CLKIN");
    p.Write32(4, 0x15);
    Check(p.Ratios() == ClockRatios{16, 2, 5}, "PLL_DIV CSEL change applies immediately");
    p.Write32(4, 0x34);
    Check(p.Ratios() == ClockRatios{16, 8, 8}, "SSEL raised to the CCLK divider");
    BF524PLL df;
    program(df, 5, 0x2001);
    Check(df.Ratios() == ClockRatios{16, 2, 10}, "DF halves the VCO input");
    BF524PLL msel0;
    program(msel0, 5, 0x0000);
    Check(msel0.Ratios() == ClockRatios{64, 1, 5}, "MSEL=0 means 64");
    BF524PLL bypass;
    bypass.Write32(0, 0x2100);
    Check(bypass.EnterIdle() && bypass.Ratios() == ClockRatios{}, "BYPASS runs CCLK=SCLK=CLKIN");
    BF524PLL stop;
    stop.Write32(0, 0x2008);
    Check(stop.EnterIdle() && stop.Unsupported() && !stop.Ratios(), "unsupported power mode gives no ratio");
}

void NoHostTime() {
    GuestClock a, b;
    a.SetRatios(kBoundary);
    b.SetRatios(kBoundary);
    a.AdvanceAwakeCclk(1234);
    std::this_thread::sleep_for(std::chrono::milliseconds(5)); // Host time passes; guest time must not.
    b.AdvanceAwakeCclk(1234);
    Check(Same(a, b), "host time does not enter guest time");
}

void CpuDefaultModeUntouched() {
    ScopedEnv mode("OP1EMU_EXPERIMENTAL_GUEST_CLOCK", nullptr);
    BlackFinCpu cpu;
    cpu.AttachNandFlash(std::make_shared<IdleNand>());
    const std::array<uint8_t, 4> code{0x00, 0x00, 0x00, 0x20}; // NOP; JUMP.S 0
    cpu.GetEmulator().MemoryWrite(0x100, code.data(), static_cast<int>(code.size()));
    cpu.SetPC(0x100);
    cpu.Run();
    Check(!cpu.GuestClockMode() && cpu.PacketEntryCount() == 2 && cpu.GuestTime().CclkCycles() == 0,
          "default mode leaves the guest clock idle");
}

void CpuPacketDeltas() {
    ScopedEnv mode("OP1EMU_EXPERIMENTAL_GUEST_CLOCK", "1");
    BlackFinCpu cpu;
    cpu.AttachNandFlash(std::make_shared<IdleNand>());
    Check(cpu.GuestClockMode() && cpu.GuestClockCostModel() == GuestCostModel::MinCost, "opt-in min-cost mode");
    const std::array<uint8_t, 4> code{0x00, 0x00, 0x00, 0x20};
    cpu.GetEmulator().MemoryWrite(0x100, code.data(), static_cast<int>(code.size()));
    cpu.SetPC(0x100);
    cpu.Run();
    const GuestClock& t = cpu.GuestTime();
    Check(cpu.PacketEntryCount() == 2 && t.CclkCycles() == 2 && t.CclkWithoutRatio() == 2 && t.ClkinTicks() == 0,
          "first block charges its two packet entries; reset ratio unknown");
    cpu.Run();
    Check(cpu.PacketEntryCount() == 3 && t.CclkCycles() == 3, "cached block charges one more");
    for (unsigned i = 0; i < 10; ++i) cpu.Run();
    Check(cpu.PacketEntryCount() == 13 && t.CclkCycles() == 13, "multiple blocks: CCLK equals summed packet deltas");
}

void CpuZeroAndFailedExecution() {
    ScopedEnv mode("OP1EMU_EXPERIMENTAL_GUEST_CLOCK", "1");
    class ZeroCpu : public BlackFinCpu {
    protected:
        bool ExecuteCoreBlock(u32) override { return true; } // Core ran, issued nothing.
    };
    ZeroCpu zero;
    zero.AttachNandFlash(std::make_shared<IdleNand>());
    zero.SetPC(0x100);
    for (unsigned i = 0; i < 3; ++i) zero.Run();
    Check(zero.PacketEntryCount() == 0 && zero.GuestTime().CclkCycles() == 0, "zero-packet call does not advance");

    class FailureCpu : public BlackFinCpu {
    protected:
        bool ExecuteCoreBlock(u32) override { return false; }
    };
    FailureCpu failed;
    failed.AttachNandFlash(std::make_shared<IdleNand>());
    failed.SetPC(0x100);
    failed.Run();
    Check(failed.Failed() && failed.GuestTime().CclkCycles() == 0, "failed execution does not advance");
}

void CpuPllRatioChangeAndIdle() {
    ScopedEnv mode("OP1EMU_EXPERIMENTAL_GUEST_CLOCK", "1");
    BlackFinCpu cpu;
    cpu.AttachNandFlash(std::make_shared<IdleNand>());
    cpu.GetEmulator().MemoryWrite16(0x110, 0x0020); // IDLE
    cpu.GetEmulator().MemoryWrite16(0x112, 0x2000); // JUMP.S 0
    cpu.GetEmulator().MemoryWrite16(0xFFC00004, 5);
    cpu.GetEmulator().MemoryWrite16(0xFFC00000, 0x2000);
    cpu.SetPC(0x110);
    cpu.Run();
    const GuestClock& t = cpu.GuestTime();
    Check(!cpu.IsIdle() && cpu.PC() == 0x112 && cpu.ClockState().ClkinTicks() == 512,
          "PLL lock wakes the CPU (existing behavior)");
    Check(t.CclkCycles() == 1 && t.CclkWithoutRatio() == 1 && t.ClkinTicks() == 512 && t.SclkCycles() == 512,
          "IDLE's entry charged awake; lock is CLKIN time in 1:1 bypass with CCLK held");
    cpu.Run();
    Check(t.Ratios() == kBoundary && t.CclkCycles() == 2 && t.ClkinTicks() == 512, "new ratio applies to the next block");
    for (unsigned i = 0; i < 15; ++i) cpu.Run();
    Check(t.CclkCycles() == 17 && t.ClkinTicks() == 513 && t.SclkCycles() == 515,
          "16 CCLK at 16:1 is exactly one CLKIN and 16/5 SCLK");

    cpu.GetEmulator().MemoryWrite16(0x120, 0x0020);
    cpu.SetPC(0x120);
    cpu.Run();
    Check(cpu.IsIdle() && cpu.UnsupportedIdle() && t.CclkCycles() == 18, "IDLE entry is the last awake charge");
    const uint64_t clkin = t.ClkinTicks(), packets = cpu.PacketEntryCount();
    for (unsigned i = 0; i < 5; ++i) cpu.Run();
    Check(cpu.IsIdle() && t.CclkCycles() == 18 && t.ClkinTicks() == clkin && cpu.PacketEntryCount() == packets,
          "idle early returns neither execute nor advance guest time");
}

void CpuCostModels() {
    {
        ScopedEnv mode("OP1EMU_EXPERIMENTAL_GUEST_CLOCK", "1");
        ScopedEnv shadow("OP1EMU_SHADOW_TIMING", "1");
        ScopedEnv cost("OP1EMU_GUEST_CLOCK_COST", "shadow-estimate");
        BlackFinCpu cpu;
        cpu.AttachNandFlash(std::make_shared<IdleNand>());
        const std::array<uint8_t, 4> code{0x00, 0x00, 0x00, 0x20};
        cpu.GetEmulator().MemoryWrite(0x100, code.data(), static_cast<int>(code.size()));
        cpu.SetPC(0x100);
        cpu.Run();
        cpu.Run();
        Check(cpu.GuestClockCostModel() == GuestCostModel::ShadowEstimate &&
              cpu.GuestTime().CclkCycles() == cpu.ShadowTimingStats()->EstimatedCycles(),
              "shadow estimate is selectable and charged as its own deltas");
    }
    auto throws = [](const char* mode, const char* shadow, const char* cost) {
        ScopedEnv m("OP1EMU_EXPERIMENTAL_GUEST_CLOCK", mode);
        ScopedEnv s("OP1EMU_SHADOW_TIMING", shadow);
        ScopedEnv c("OP1EMU_GUEST_CLOCK_COST", cost);
        try { BlackFinCpu cpu; } catch (const std::invalid_argument&) { return true; }
        return false;
    };
    Check(throws("2", nullptr, nullptr), "guest clock flag must be 0 or 1");
    Check(throws("1", nullptr, "fast"), "unknown cost model rejected");
    Check(throws("1", "0", "shadow-estimate"), "shadow estimate requires shadow timing");
}
}

int main() {
    InitialAndUnknownRatio();
    ExactRatios();
    RatioTransitions();
    PllRatios();
    NoHostTime();
    CpuDefaultModeUntouched();
    CpuPacketDeltas();
    CpuZeroAndFailedExecution();
    CpuPllRatioChangeAndIdle();
    CpuCostModels();
    std::puts("guest clock tests passed");
    return 0;
}
