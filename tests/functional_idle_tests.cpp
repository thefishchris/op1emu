// Functional-profile architectural IDLE (docs/emulator-profiles.md): deadline
// arbitration between the PLL lock and the ASSUMED plain-IDLE core timer, and
// regressions proving the assumption never reaches strict.
#include "cpu/cpu.h"
#include "cpu/guest_coretimer.h"
#include "cpu/nand.h"
#include "cpu/pll.h"
#include "cec.h"
#include "cpu_state.h"
#include "evt.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>

namespace {
void Check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "functional idle: %s\n", what);
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

class TestCpu : public BlackFinCpu {
public:
    TestCpu() { AttachNandFlash(std::make_shared<IdleNand>()); }
    CpuState& State() { return *cpuState_; }
    u32 Timer(u32 offset) { return GetEmulator().MemoryRead32(0xFFE03000 + offset); }
    void SetTimer(u32 offset, u32 value) { GetEmulator().MemoryWrite32(0xFFE03000 + offset, value); }
};

constexpr u32 TCNTL = 0x0, TPERIOD = 0x4, TCOUNT = 0xC, TINT = 8;
constexpr u16 IDLE = 0x0020, RTI = 0x0011, JUMP_SELF = 0x2000;
constexpr const char* kHeld = "held-domain-active device=coretimer domain=cclk-core idle-state=plain-idle";

// Program: PLL_DIV=5/PLL_CTL=0x2000 pending, then at 0x110 IDLE (relock),
// 0x112 RTI out of reset into IVG15 at 0x200, where 0x200 IDLE waits; the
// IVG6 handler at 0x300 spins. The timer is armed at CCLK 0.
void LoadProgram(TestCpu& cpu, u32 tperiod, bool unmaskIvg6) {
    auto& memory = cpu.GetEmulator();
    memory.MemoryWrite16(0x110, IDLE);
    memory.MemoryWrite16(0x112, RTI);
    memory.MemoryWrite16(0x200, IDLE);
    memory.MemoryWrite16(0x202, JUMP_SELF);
    memory.MemoryWrite16(0x300, JUMP_SELF);
    memory.MemoryWrite16(0xFFC00004, 5);
    memory.MemoryWrite16(0xFFC00000, 0x2000);
    cpu.State().reti = 0x200;
    evt_set(15, 0x200);
    evt_set(6, 0x300);
    cec_mmr_write(&cpu.State(), CEC_MMR_BASE + 4, (1u << 15) | (unmaskIvg6 ? (1u << 6) : 0u));
    cec_raise(&cpu.State(), 15);
    cpu.SetTimer(TPERIOD, tperiod);
    cpu.SetTimer(TCNTL, 7); // TMPWR | TMREN | TAUTORLD
    cpu.SetPC(0x110);
}

void PllWinsThenTimerWakes() {
    ScopedEnv profile("OP1EMU_PROFILE", "functional");
    TestCpu cpu;
    LoadProgram(cpu, 2000, true);
    const GuestClock& t = cpu.GuestTime();
    cpu.Run();
    Check(!cpu.IsIdle() && cpu.PC() == 0x112 && t.ClkinTicks() == 512 && t.CclkCycles() == 513 &&
          cpu.Timer(TCOUNT) == 2000 - 513 && cpu.PacketEntryCount() == 1 && cpu.GuestTimer()->Expiries() == 0,
          "PLL lock (512 CLKIN at 1:1) is earlier; core timer advanced over the same interval; no idle packets");
    Check(cpu.ClockState().Ratios() == ClockRatios{16, 1, 5}, "PLL state committed (16:1 CCLK, 16:5 SCLK)");
    cpu.Run(); // RTI into the IVG15 handler.
    Check(cpu.PC() == 0x200, "entered IVG15");
    cec_push_reti(&cpu.State()); // Re-enable nesting, as an IVG15 handler would.
    cpu.Run(); // IDLE at 0x200, then the core timer deadline.
    Check(!cpu.IsIdle() && !cpu.UnsupportedIdle() && cpu.PC() == 0x300 && cpu.State().reti == 0x202,
          "IVG6 from the core timer wakes IDLE and vectors to EVT6 with the post-IDLE return");
    Check(t.CclkCycles() == 2000 && cpu.GuestTimer()->Expiries() == 1 && cpu.Timer(TCOUNT) == 2000 &&
          (cpu.Timer(TCNTL) & TINT) && cpu.GuestTimer()->NextExpiryCclk() == 4000,
          "guest time lands exactly on expiry; auto-reload keeps the phase; TINT latched");
    Check(t.ClkinTicks() == 512 + (2000 - 513) / 16 && cpu.PacketEntryCount() == 3,
          "CLKIN follows the 16:1 ratio exactly; packets counted only for executed code");
}

void TimerEarlierThanPll() {
    ScopedEnv profile("OP1EMU_PROFILE", "functional");
    TestCpu cpu;
    LoadProgram(cpu, 100, false); // IVG6 masked: expiries latch but cannot wake.
    const GuestClock& t = cpu.GuestTime();
    cpu.Run();
    Check(cpu.IsIdle() && !cpu.UnsupportedIdle() && t.CclkCycles() == 100 && t.ClkinTicks() == 99 &&
          cpu.ClockState().NextDeadlineTicks() == 413 && cpu.GuestTimer()->Expiries() == 1 &&
          cpu.PacketEntryCount() == 1, "core timer is earlier: time stops exactly at its expiry, still idle");
    for (unsigned i = 0; i < 4; ++i) cpu.Run();
    Check(cpu.IsIdle() && cpu.GuestTimer()->Expiries() == 5 && t.CclkCycles() == 500, "one deadline per step");
    cpu.Run();
    Check(!cpu.IsIdle() && cpu.PC() == 0x112 && t.ClkinTicks() == 512 && t.CclkCycles() == 513 &&
          cpu.GuestTimer()->Expiries() == 5 && cpu.PacketEntryCount() == 1,
          "the PLL lock then wins and wakes; no packet inflation from idle waiting");
}

void SimultaneousDeadlines() {
    ScopedEnv profile("OP1EMU_PROFILE", "functional");
    TestCpu cpu;
    LoadProgram(cpu, 513, false); // Expiry at CCLK 513 == PLL lock after the IDLE's own cycle.
    cpu.Run();
    Check(!cpu.IsIdle() && cpu.GuestTime().ClkinTicks() == 512 && cpu.GuestTime().CclkCycles() == 513 &&
          cpu.GuestTimer()->Expiries() == 1 && (cpu.Timer(TCNTL) & TINT) && cpu.Timer(TCOUNT) == 513 &&
          (cec_mmr_read(&cpu.State(), CEC_MMR_BASE + 12) & (1u << 6)),
          "both events serviced at the same guest timestamp in one step before the wake");
}

void DisabledTimerAndUnknownRatio() {
    ScopedEnv profile("OP1EMU_PROFILE", "functional");
    {
        TestCpu cpu;
        cpu.GetEmulator().MemoryWrite16(0x100, IDLE);
        cpu.SetPC(0x100);
        cpu.Run();
        Check(cpu.UnsupportedIdle() && std::strcmp(cpu.UnsupportedIdleReason(),
              "no known eligible wake completion or deterministic deadline") == 0 &&
              cpu.GuestTime().ClkinTicks() == 0, "a disabled timer is not a deadline");
    }
    {
        TestCpu cpu;
        cpu.GetEmulator().MemoryWrite16(0x100, IDLE);
        cpu.SetTimer(TPERIOD, 1000);
        cpu.SetTimer(TCNTL, 7);
        cpu.SetPC(0x100);
        cpu.Run();
        Check(cpu.UnsupportedIdle() && std::strcmp(cpu.UnsupportedIdleReason(),
              "functional idle deadline needs known clock ratios") == 0 && cpu.GuestTimer()->Expiries() == 0,
              "an unknown CCLK:CLKIN ratio is a controlled stop, not a guess");
    }
}

void StrictNeverAssumes() {
    ScopedEnv profile("OP1EMU_PROFILE", "strict");
    ScopedEnv guest("OP1EMU_EXPERIMENTAL_GUEST_CLOCK", "1");
    TestCpu cpu;
    Check(cpu.GuestClockMode() && !cpu.Timing().coreTimerRunsInIdle, "strict guest clock without the assumption");
    LoadProgram(cpu, 2000, true);
    cpu.Run();
    const uint64_t cclk = cpu.GuestTime().CclkCycles();
    for (unsigned i = 0; i < 5; ++i) cpu.Run();
    Check(cpu.IsIdle() && cpu.UnsupportedIdle() && std::strcmp(cpu.UnsupportedIdleReason(), kHeld) == 0 &&
          cpu.GuestTime().ClkinTicks() == 0 && cpu.GuestTime().CclkCycles() == cclk &&
          cpu.ClockState().ClkinTicks() == 0 && cpu.GuestTimer()->Expiries() == 0 &&
          !(cec_mmr_read(&cpu.State(), CEC_MMR_BASE + 12) & (1u << 6)) && cpu.PacketEntryCount() == 1,
          "strict: active core timer in plain IDLE stays a held stop; no advance, no IVG6, no packets");
}

void UnsetGuestClockNeverAssumes() {
    ScopedEnv profile("OP1EMU_PROFILE", nullptr);
    ScopedEnv guest("OP1EMU_EXPERIMENTAL_GUEST_CLOCK", "1");
    TestCpu cpu;
    LoadProgram(cpu, 2000, true);
    cpu.Run();
    Check(cpu.UnsupportedIdle() && std::strcmp(cpu.UnsupportedIdleReason(), kHeld) == 0 &&
          !cpu.Timing().coreTimerRunsInIdle, "unset profile keeps the strict hold");
}
}

int main() {
    PllWinsThenTimerWakes();
    TimerEarlierThanPll();
    SimultaneousDeadlines();
    DisabledTimerAndUnknownRatio();
    StrictNeverAssumes();
    UnsetGuestClockNeverAssumes();
    std::puts("functional idle tests passed");
    return 0;
}
