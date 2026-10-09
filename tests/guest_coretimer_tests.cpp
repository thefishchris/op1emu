// Guest-clock core timer: awake semantics per BF52x HRM chapter 11, and the
// held CCLK-core domain during plain IDLE (docs/guest-clock-architecture.md D6/D7).
#include "cpu/cpu.h"
#include "cpu/guest_clock.h"
#include "cpu/guest_coretimer.h"
#include "cpu/nand.h"
#include "cpu/pll.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>

namespace {
void Check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "guest core timer: %s\n", what);
        std::exit(1);
    }
}

constexpr u32 TCNTL = 0x0, TPERIOD = 0x4, TSCALE = 0x8, TCOUNT = 0xC;
constexpr u32 TMPWR = 1, TMREN = 2, TAUTORLD = 4, TINT = 8;

struct Rig {
    GuestClock clock;
    GuestCoreTimer timer{0xFFE03000, clock};
    unsigned raises = 0;
    Rig() {
        timer.BindInterrupt(6, [this](int ivg, int level) {
            Check(ivg == 6 && level == 1, "IVTMR is an IVG6 edge");
            ++raises;
        });
    }
    uint64_t Advance(uint64_t cclk) {
        clock.AdvanceAwakeCclk(cclk);
        return timer.Sync();
    }
    u32 Reg(u32 offset) { return timer.Read32(offset); }
    void Set(u32 offset, u32 value) { timer.Write32(offset, value); }
};

void ResetAndPower() {
    Rig r;
    Check(r.Reg(TCNTL) == 0 && r.Reg(TCOUNT) == 0 && r.Reg(TPERIOD) == 0 && r.Reg(TSCALE) == 0 &&
          !r.timer.Counting() && !r.timer.NextExpiryCclk(), "reset: stopped, undefined counts modelled as 0");
    r.Set(TPERIOD, 10);
    Check(r.Reg(TCOUNT) == 10 && r.Reg(TPERIOD) == 10, "TPERIOD write copies to TCOUNT");
    r.Set(TCOUNT, 5);
    Check(r.Reg(TCOUNT) == 5 && r.Reg(TPERIOD) == 10, "direct TCOUNT write before enable");
    r.Set(TCNTL, TMREN);
    r.Advance(100);
    Check(!r.timer.Counting() && r.Reg(TCOUNT) == 5 && r.raises == 0, "TMREN without TMPWR does not count (undefined)");
    r.Set(TCNTL, TMPWR);
    r.Advance(100);
    Check(!r.timer.Counting() && r.Reg(TCOUNT) == 5 && r.Reg(TCNTL) == TMPWR, "TMPWR alone does not count");
    r.Set(TSCALE, 0x1FF);
    Check(r.Reg(TSCALE) == 0xFF, "TSCALE is 8 bits");
}

void ScaleDecrementAndOneShot() {
    Rig r;
    r.Set(TSCALE, 3); // One decrement every 4 CCLK.
    r.Set(TCOUNT, 5);
    r.Advance(7);     // Time before enable is not counted.
    r.Set(TCNTL, TMPWR | TMREN);
    Check(r.timer.Counting() && r.timer.NextExpiryCclk() == 7 + 20, "expiry at enable + TCOUNT*(TSCALE+1)");
    r.Advance(3);
    Check(r.Reg(TCOUNT) == 5, "no decrement inside the first prescale interval");
    r.Advance(1);
    Check(r.Reg(TCOUNT) == 4, "decrement after TSCALE+1 CCLK");
    r.Advance(15);
    Check(r.Reg(TCOUNT) == 1 && r.raises == 0 && !(r.Reg(TCNTL) & TINT), "one CCLK before the boundary");
    Check(r.Advance(1) == 1 && r.raises == 1, "expires exactly on the boundary");
    Check((r.Reg(TCNTL) & TINT) && r.Reg(TCOUNT) == 0 && !r.timer.Counting() && (r.Reg(TCNTL) & TMREN),
          "one-shot: TINT set, TCOUNT 0, halted, TMREN readback kept (ASSUMED)");
    r.Advance(1000);
    Check(r.raises == 1 && r.timer.Expiries() == 1, "halted one-shot does not expire again");
    r.Set(TCOUNT, 3);
    r.Set(TCNTL, TMPWR | TMREN);
    r.Advance(100);
    Check(r.raises == 1, "re-writing TMREN=1 without clearing it does not re-arm (ASSUMED)");
    r.Set(TCNTL, TMPWR);
    r.Set(TCNTL, TMPWR | TMREN);
    r.Advance(11);
    Check(r.raises == 1 && r.Reg(TCOUNT) == 1, "re-armed count runs from the new enable");
    r.Advance(1);
    Check(r.raises == 2, "re-armed one-shot expires after TCOUNT*(TSCALE+1)");
}

void AutoReloadWithoutDrift() {
    Rig r;
    r.Set(TPERIOD, 1000);
    r.Set(TCNTL, TMPWR | TMREN | TAUTORLD);
    const uint64_t chunks[] = {7, 13, 999, 1, 2500, 333, 47};
    uint64_t total = 0, expiries = 0;
    unsigned i = 0;
    while (total < 1000000) {
        const uint64_t step = std::min<uint64_t>(chunks[i++ % 7], 1000000 - total);
        total += step;
        if (r.Advance(step)) {
            ++expiries;
            r.Set(TCNTL, TMPWR | TMREN | TAUTORLD); // ISR clears TINT; must not re-anchor.
        }
    }
    Check(r.timer.Expiries() == 1000 && !(r.Reg(TCNTL) & TINT), "1000 periods of 1000 CCLK: exactly 1000 expiries");
    Check(r.timer.NextExpiryCclk() == 1001000 && r.Reg(TCOUNT) == 1000, "no phase drift after irregular syncs");
    r.Advance(500);
    Check(r.Reg(TCOUNT) == 500, "count mid-period");
    Check(r.raises == expiries, "one IVTMR request per sync with expiries");
}

void MultipleExpiriesAndTint() {
    Rig r;
    r.Set(TPERIOD, 10);
    r.Set(TCNTL, TMPWR | TMREN | TAUTORLD);
    Check(r.Advance(35) == 3 && r.raises == 1 && r.timer.Expiries() == 3 && r.Reg(TCOUNT) == 5,
          "three expiries in one sync latch one IVTMR request");
    r.Set(TCNTL, TMPWR | TMREN | TAUTORLD | TINT);
    Check(r.Reg(TCNTL) & TINT, "writing 1 to TINT does not clear it (not W1C)");
    r.Advance(5);
    Check(r.raises == 2 && (r.Reg(TCNTL) & TINT), "sticky TINT does not suppress later requests; no enable bit");
    r.Set(TCNTL, TMPWR | TMREN | TAUTORLD);
    Check(!(r.Reg(TCNTL) & TINT) && r.timer.Counting(), "writing 0 clears TINT while running");
    r.timer.ProcessWithInterrupt(6);
    r.Set(TCNTL, TMPWR | TMREN | TAUTORLD | TINT);
    Check(!(r.Reg(TCNTL) & TINT), "servicing IVG6 does not set or clear TINT; writing 1 does not set it");
}

void WritesWhileRunningAndStop() {
    Rig r;
    r.Set(TPERIOD, 100);
    r.Set(TCNTL, TMPWR | TMREN | TAUTORLD);
    r.Advance(30);
    r.Set(TPERIOD, 7);
    r.Set(TCOUNT, 7);
    Check(r.Reg(TPERIOD) == 100 && r.Reg(TCOUNT) == 70, "TPERIOD/TCOUNT writes ignored while running");
    r.Set(TCNTL, TMPWR | TAUTORLD); // Clear TMREN.
    r.Advance(50);
    Check(!r.timer.Counting() && r.Reg(TCOUNT) == 70, "disabling freezes TCOUNT");
    r.Set(TCOUNT, 40);
    Check(r.Reg(TCOUNT) == 40, "TCOUNT writable while stopped");
    r.Set(TCNTL, TMPWR | TMREN | TAUTORLD);
    r.Advance(39);
    Check(r.Reg(TCOUNT) == 1 && r.raises == 0, "re-enable counts from the written value");
    r.Advance(1);
    Check(r.raises == 1 && r.Reg(TCOUNT) == 100, "auto-reload from TPERIOD at zero (ASSUMED same step)");
    r.Advance(20);
    r.Set(TCNTL, TMREN | TAUTORLD); // Clear TMPWR.
    r.Advance(500);
    Check(!r.timer.Counting() && r.Reg(TCOUNT) == 80, "TMPWR=0 freezes TCOUNT");
    r.Set(TCNTL, TMPWR | TMREN | TAUTORLD);
    r.Advance(10);
    r.Set(TSCALE, 1);
    r.Advance(20);
    Check(r.Reg(TCOUNT) == 60, "TSCALE change while running re-anchors at the frozen count (ASSUMED)");
}

void ZeroCountsAndLargeValues() {
    Rig r;
    r.Set(TCNTL, TMPWR | TMREN);
    r.Advance(100);
    Check(!r.timer.Counting() && r.raises == 0, "start with TCOUNT=0 does not count (UNKNOWN)");
    r.Set(TCOUNT, 4);
    Check(r.timer.Counting() && r.timer.NextExpiryCclk() == 104, "TCOUNT write re-arms from now, not a stale anchor");
    r.Advance(4);
    Check(r.raises == 1, "re-armed timer expires");

    Rig z;
    z.Set(TPERIOD, 0);
    z.Set(TCOUNT, 2);
    z.Set(TCNTL, TMPWR | TMREN | TAUTORLD);
    z.Advance(50);
    Check(z.raises == 1 && !z.timer.Counting() && z.Reg(TCOUNT) == 0, "auto-reload of TPERIOD=0 stops (UNKNOWN)");

    Rig big;
    big.Set(TSCALE, 255);
    big.Set(TCOUNT, 0xFFFFFFFFu);
    big.Set(TCNTL, TMPWR | TMREN);
    const uint64_t span = uint64_t{0xFFFFFFFFu} * 256u;
    Check(big.timer.NextExpiryCclk() == span, "64-bit expiry for maximum count and scale");
    big.Advance(span - 1);
    Check(big.Reg(TCOUNT) == 1 && big.raises == 0, "one CCLK before a ~1.1e12-cycle expiry");
    big.Advance(1);
    Check(big.raises == 1 && big.Reg(TCOUNT) == 0, "maximum one-shot expires exactly");
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

constexpr u32 kTimer = 0xFFE03000, kIlat = 0xFFE0210C;
constexpr const char* kHeld = "held-domain-active device=coretimer domain=cclk-core idle-state=plain-idle";

void CpuLegacyModeKeepsLegacyTimer() {
    ScopedEnv mode("OP1EMU_EXPERIMENTAL_GUEST_CLOCK", nullptr);
    BlackFinCpu cpu;
    Check(!cpu.GuestTimer(), "default mode keeps the legacy core timer");
}

void CpuAwakeExpiryLatchesIvg6() {
    ScopedEnv mode("OP1EMU_EXPERIMENTAL_GUEST_CLOCK", "1");
    BlackFinCpu cpu;
    cpu.AttachNandFlash(std::make_shared<IdleNand>());
    cpu.GetEmulator().MemoryWrite16(0x100, 0x2000); // JUMP.S 0: one packet per run.
    cpu.GetEmulator().MemoryWrite32(kTimer + TPERIOD, 5);
    cpu.GetEmulator().MemoryWrite32(kTimer + TCNTL, TMPWR | TMREN | TAUTORLD);
    cpu.SetPC(0x100);
    for (unsigned i = 0; i < 4; ++i) cpu.Run();
    Check(cpu.GuestTimer()->CurrentCount() == 1 && !(cpu.GetEmulator().MemoryRead32(kIlat) & 0x40),
          "awake decrement on guest CCLK; no request before expiry");
    cpu.Run();
    Check(cpu.GuestTime().CclkCycles() == 5 && cpu.GuestTimer()->Expiries() == 1 &&
          (cpu.GetEmulator().MemoryRead32(kIlat) & 0x40) &&
          (cpu.GetEmulator().MemoryRead32(kTimer + TCNTL) & TINT),
          "expiry in the block that reaches it latches IVG6 in ILAT and sets TINT");
}

void CpuIdleHoldsCoreTimerDomain() {
    ScopedEnv mode("OP1EMU_EXPERIMENTAL_GUEST_CLOCK", "1");
    BlackFinCpu cpu;
    cpu.AttachNandFlash(std::make_shared<IdleNand>());
    cpu.GetEmulator().MemoryWrite16(0x100, 0x0020); // IDLE
    cpu.GetEmulator().MemoryWrite16(0x102, 0x2000);
    cpu.GetEmulator().MemoryWrite32(kTimer + TPERIOD, 1000);
    cpu.GetEmulator().MemoryWrite32(kTimer + TCNTL, TMPWR | TMREN | TAUTORLD);
    cpu.SetPC(0x100);
    cpu.Run();
    Check(cpu.IsIdle() && cpu.UnsupportedIdle() && cpu.UnsupportedIdleReason() &&
          std::strcmp(cpu.UnsupportedIdleReason(), kHeld) == 0, "running core timer in plain IDLE is a classified stop");
    const uint64_t cclk = cpu.GuestTime().CclkCycles(), clkin = cpu.GuestTime().ClkinTicks();
    for (unsigned i = 0; i < 10; ++i) cpu.Run();
    Check(cpu.IsIdle() && cpu.GuestTime().CclkCycles() == cclk && cpu.GuestTime().ClkinTicks() == clkin &&
          cpu.GuestTimer()->CurrentCount() == 999 && cpu.GuestTimer()->Expiries() == 0 &&
          !(cpu.GetEmulator().MemoryRead32(kIlat) & 0x40) && cpu.PacketEntryCount() == 1,
          "no wake, no deadline advance, no host time, no packet spin");
}

void CpuIdleWithStoppedTimerKeepsExistingReason() {
    ScopedEnv mode("OP1EMU_EXPERIMENTAL_GUEST_CLOCK", "1");
    BlackFinCpu cpu;
    cpu.AttachNandFlash(std::make_shared<IdleNand>());
    cpu.GetEmulator().MemoryWrite16(0x100, 0x0020);
    cpu.SetPC(0x100);
    cpu.Run();
    Check(cpu.UnsupportedIdle() && std::strcmp(cpu.UnsupportedIdleReason(),
          "no known eligible wake completion or deterministic deadline") == 0,
          "a stopped core timer does not hold IDLE; the existing reason stands");
}

void CpuPllLockWithRunningTimerIsHeld() {
    ScopedEnv mode("OP1EMU_EXPERIMENTAL_GUEST_CLOCK", "1");
    BlackFinCpu cpu;
    cpu.AttachNandFlash(std::make_shared<IdleNand>());
    cpu.GetEmulator().MemoryWrite16(0x100, 0x0020);
    cpu.GetEmulator().MemoryWrite16(0xFFC00004, 5);
    cpu.GetEmulator().MemoryWrite16(0xFFC00000, 0x2000);
    cpu.GetEmulator().MemoryWrite32(kTimer + TPERIOD, 1000);
    cpu.GetEmulator().MemoryWrite32(kTimer + TCNTL, TMPWR | TMREN);
    cpu.SetPC(0x100);
    cpu.Run();
    Check(cpu.UnsupportedIdle() && std::strcmp(cpu.UnsupportedIdleReason(), kHeld) == 0 &&
          cpu.ClockState().ClkinTicks() == 0 && cpu.GuestTime().ClkinTicks() == 0,
          "even a CONFIRMED CLKIN deadline is not taken across a held running core timer");
}
}

int main() {
    ResetAndPower();
    ScaleDecrementAndOneShot();
    AutoReloadWithoutDrift();
    MultipleExpiriesAndTint();
    WritesWhileRunningAndStop();
    ZeroCountsAndLargeValues();
    CpuLegacyModeKeepsLegacyTimer();
    CpuAwakeExpiryLatchesIvg6();
    CpuIdleHoldsCoreTimerDomain();
    CpuIdleWithStoppedTimerKeepsExistingReason();
    CpuPllLockWithRunningTimerIsHeld();
    std::puts("guest core timer tests passed");
    return 0;
}
