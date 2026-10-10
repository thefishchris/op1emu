// Multi-block execution groundwork, milestone M0 (docs/multiblock-execution.md):
//  - every block judged eligible is verified by running the real service pass
//    and comparing the full captured state (no mismatch allowed);
//  - each barrier fires for the workload that must produce it;
//  - diagnostics are passive: the same workload with diagnostics off and on
//    produces identical per-block state and MMIO sequences.
#include "cpu/cpu.h"
#include "cpu/dma.h"
#include "cpu/nand.h"
#include "cpu/sport.h"
#include "cpu/usb.h"
#include "cec.h"
#include "cpu_state.h"
#include "evt.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {
int failures = 0;
void Check(bool condition, const char* scenario, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "multiblock eligibility: %s: %s\n", scenario, what);
        ++failures;
    }
}

class ScopedEnv {
public:
    ScopedEnv(const char* name, const char* value) : name_(name) {
        if (const char* old = std::getenv(name)) old_ = old;
        if (value) setenv(name, value, 1); else unsetenv(name);
    }
    ~ScopedEnv() { if (old_) setenv(name_, old_->c_str(), 1); else unsetenv(name_); }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;
private:
    const char* name_;
    std::optional<std::string> old_;
};

// A NAND with controllable busy / pending-completion state.
class ControlledNand final : public NandFlash {
public:
    bool busy = false, pending = false;
    void SendCommand(u8) override {}
    void SendAddress(u8) override {}
    u8 ReadData() override { return 0; }
    void WriteData(u8) override {}
    bool IsDataReady() const override { return false; }
    bool IsBusy() const override { return busy; }
    bool CompletePendingPageRead() override { if (!pending) return false; pending = false; busy = false; return true; }
    bool HasPendingCompletion() const override { return pending; }
    void StartPageRead() override {}
    void StartPageWrite() override {}
    u32 PageRead(u8*, u32) override { return 0; }
    u32 PageWrite(const u8*, u32) override { return 0; }
    void SetReadCallback(ReadCallback) override {}
};

class TestCpu : public BlackFinCpu {
public:
    std::shared_ptr<ControlledNand> nand = std::make_shared<ControlledNand>();
    TestCpu() { AttachNandFlash(nand); }
    CpuState& State() { return *cpuState_; }
    DMA& Dma() { return *dma; }
    void Pin(int pin, int level) { ProcessInterrupt(pin, level); }
    u32 Rd(u32 a) { return GetEmulator().MemoryRead32(a); }
    void Wr(u32 a, u32 v) { GetEmulator().MemoryWrite32(a, v); }
};

constexpr u16 IDLE = 0x0020, RTI = 0x0011, JUMP_SELF = 0x2000, JUMP_BACK = 0x2FFF, NOP = 0x0000, STORE_P0_R0 = 0x9300;

// Functional profile: PLL relock IDLE at 0x110, RTI into IVG15 at 0x200.
void Boot(TestCpu& c, const std::function<void(TestCpu&)>& program) {
    auto& m = c.GetEmulator();
    m.MemoryWrite16(0x110, IDLE);
    m.MemoryWrite16(0x112, RTI);
    m.MemoryWrite16(0xFFC00004, 5);
    m.MemoryWrite16(0xFFC00000, 0x2000);
    c.State().reti = 0x200;
    evt_set(15, 0x200);
    cec_mmr_write(&c.State(), CEC_MMR_BASE + 4, 1u << 15);
    cec_raise(&c.State(), 15);
    program(c);
    c.SetPC(0x110);
    c.Run(); // PLL lock (IDLE)
    c.Run(); // RTI into IVG15
}

struct Outcome {
    BlackFinCpu::BatchDiagnostics stats;
    bool barrierSeen[BlackFinCpu::kBatchBarrierCount] = {};
};

// Runs `blocks` blocks with verified diagnostics; `hook` runs before each block.
Outcome RunVerified(TestCpu& c, int blocks, const std::function<void(TestCpu&, int)>& hook = nullptr) {
    c.SetBatchDiagnostics(true, true);
    Outcome o;
    for (int i = 0; i < blocks; ++i) {
        if (hook) hook(c, i);
        c.Run();
        for (unsigned b = 0; b < BlackFinCpu::kBatchBarrierCount; ++b)
            if (c.LastBatchBarriers() & (1u << b)) o.barrierSeen[b] = true;
    }
    o.stats = c.BatchStats();
    return o;
}

unsigned BitIndex(uint32_t barrier) { unsigned i = 0; while (!(barrier & 1u)) { barrier >>= 1; ++i; } return i; }
bool Saw(const Outcome& o, uint32_t barrier) { return o.barrierSeen[BitIndex(barrier)]; }

void ExpectClean(const char* scenario, const Outcome& o) {
    Check(o.stats.mismatches == 0, scenario, o.stats.firstMismatch.empty() ? "a predicted no-op pass changed state"
                                                                          : o.stats.firstMismatch.c_str());
    std::printf("%-34s blocks %6llu eligible %6llu verified %6llu mismatches %llu\n", scenario,
                static_cast<unsigned long long>(o.stats.blocks), static_cast<unsigned long long>(o.stats.eligible),
                static_cast<unsigned long long>(o.stats.verified), static_cast<unsigned long long>(o.stats.mismatches));
}

void QuietCpu() {
    ScopedEnv profile("OP1EMU_PROFILE", "functional");
    TestCpu c;
    Boot(c, [](TestCpu& t) { t.GetEmulator().MemoryWrite16(0x200, JUMP_SELF); });
    auto o = RunVerified(c, 2000);
    ExpectClean("quiet CPU loop", o);
    Check(o.stats.eligible * 10 >= o.stats.blocks * 9, "quiet CPU loop", "at least 90% of quiet blocks are eligible");
    Check(o.stats.verified == o.stats.eligible, "quiet CPU loop", "every eligible block was verified");
}

void ShortHardwareLoop() {
    ScopedEnv profile("OP1EMU_PROFILE", "functional");
    TestCpu c;
    Boot(c, [](TestCpu& t) { t.GetEmulator().MemoryWrite16(0x200, NOP); t.GetEmulator().MemoryWrite16(0x202, NOP); });
    c.State().lt[0] = 0x200; c.State().lb[0] = 0x202; c.State().lc[0] = 0x7FFFFFFF;
    auto o = RunVerified(c, 2000);
    ExpectClean("2-instruction hardware loop", o);
    Check(o.stats.eligible * 10 >= o.stats.blocks * 9 && c.PC() == 0x200, "2-instruction hardware loop", "loop iterations are eligible");
}

void MmioBlocks() {
    ScopedEnv profile("OP1EMU_PROFILE", "functional");
    TestCpu c;
    Boot(c, [](TestCpu& t) { t.GetEmulator().MemoryWrite16(0x200, STORE_P0_R0); t.GetEmulator().MemoryWrite16(0x202, JUMP_BACK); });
    c.State().dpregs[8] = 0xFFC00110;           // P0 = SIC_IAR0
    c.State().dpregs[0] = c.Rd(0xFFC00110);     // R0 = its current value
    std::vector<MMIOAccess> log;
    c.SetMMIOLog(&log);
    auto o = RunVerified(c, 500);
    c.SetMMIOLog(nullptr);
    ExpectClean("MMIO store loop", o);
    Check(o.stats.barriers[BitIndex(BlackFinCpu::BarrierMmio)] == o.stats.blocks && o.stats.eligible == 0,
          "MMIO store loop", "every block with an MMR access is a barrier");
    Check(log.size() == 500 && log[0].address == 0xFFC00110 && log[0].write && log[0].pc == 0x200,
          "MMIO store loop", "the exact MMIO sequence is recorded");
}

void CoreTimerDeadline() {
    ScopedEnv profile("OP1EMU_PROFILE", "functional");
    TestCpu c;
    Boot(c, [](TestCpu& t) { t.GetEmulator().MemoryWrite16(0x200, JUMP_SELF); });
    c.Wr(0xFFE03004, 300); // TPERIOD (CCLK); IVG6 masked: expiries latch only
    c.Wr(0xFFE03000, 7);
    auto o = RunVerified(c, 3000);
    ExpectClean("core timer expiries", o);
    Check(Saw(o, BlackFinCpu::BarrierGuestDeadline) && Saw(o, BlackFinCpu::BarrierInterruptChange),
          "core timer expiries", "an expiry is a guest-deadline and interrupt barrier");
    Check(o.stats.eligible > 0, "core timer expiries", "blocks between expiries remain eligible");
}

// SPORT0 TX on a two-descriptor list (firmware CONFIG 0x7489) and RX autobuffer.
void StartSport(TestCpu& c) {
    auto& m = c.GetEmulator();
    for (u32 at : {0xFF902000u, 0xFF902010u}) {
        const u32 next = at == 0xFF902000u ? 0xFF902010u : 0xFF902000u, start = at == 0xFF902000u ? 0xFF801000u : 0xFF801100u;
        m.MemoryWrite16(at, static_cast<u16>(next & 0xFFFF)); m.MemoryWrite16(at + 2, static_cast<u16>(next >> 16));
        m.MemoryWrite16(at + 4, static_cast<u16>(start & 0xFFFF)); m.MemoryWrite16(at + 6, static_cast<u16>(start >> 16));
    }
    c.Wr(0xFFC0010C, 0xFFFFFFFF);
    c.Wr(0xFFC00804, 0x021F); c.Wr(0xFFC00824, 0x021F); c.Wr(0xFFC00800, 1); c.Wr(0xFFC00820, 1);
    c.Wr(0xFFC00D00, 0xFF902000); c.Wr(0xFFC00D10, 8); c.Wr(0xFFC00D14, 4); c.Wr(0xFFC00D08, 0x7489); // DMA4 TX
    c.Wr(0xFFC00CC4, 0xFF801200); c.Wr(0xFFC00CD0, 8); c.Wr(0xFFC00CD4, 4); c.Wr(0xFFC00CC8, 0x108B); // DMA3 RX
}

void SportDmaActivity() {
    ScopedEnv profile("OP1EMU_PROFILE", "functional");
    TestCpu c;
    Boot(c, [](TestCpu& t) { t.GetEmulator().MemoryWrite16(0x200, JUMP_SELF); });
    StartSport(c);
    auto o = RunVerified(c, 120000, [](TestCpu& t, int i) {
        if ((i & 2047) == 0) for (u32 ch : {0xFFC00CC0u, 0xFFC00D00u}) if (t.Rd(ch + 0x28) & 1u) t.Wr(ch + 0x28, 1);
    });
    ExpectClean("SPORT DMA (descriptor list)", o);
    Check(o.stats.deviceWork.size() > 0 && Saw(o, BlackFinCpu::BarrierDeviceWork), "SPORT DMA (descriptor list)",
          "a due SPORT frame is device work");
    Check(o.stats.eligible > o.stats.blocks / 2, "SPORT DMA (descriptor list)", "blocks between SPORT frames are eligible");
    Check(c.Sport(0).TransmitWords() > 0 && c.Sport(0).ReceiveWords() > 0, "SPORT DMA (descriptor list)", "SPORT moved words");
}

void NandNfcWork() {
    ScopedEnv profile("OP1EMU_PROFILE", "functional");
    TestCpu c;
    Boot(c, [](TestCpu& t) { t.GetEmulator().MemoryWrite16(0x200, JUMP_SELF); });
    auto o = RunVerified(c, 600, [](TestCpu& t, int i) {
        if (i == 100) { t.nand->busy = true; t.nand->pending = true; } // page read in progress
        if (i == 300) t.nand->busy = true;                              // busy without completion: NFC sees a change
        if (i == 302) t.nand->busy = false;
    });
    ExpectClean("NAND/NFC completions", o);
    const size_t nfc = [&] { for (size_t i = 0; i < o.stats.deviceWork.size(); ++i) if (c.DeviceName(i) == "NFC") return i; return SIZE_MAX; }();
    Check(nfc != SIZE_MAX && o.stats.deviceWork[nfc] >= 2, "NAND/NFC completions", "pending completion and busy changes are NFC work");
}

void TwiActivity() {
    ScopedEnv profile("OP1EMU_PROFILE", "functional");
    TestCpu c;
    Boot(c, [](TestCpu& t) { t.GetEmulator().MemoryWrite16(0x200, JUMP_SELF); });
    auto o = RunVerified(c, 400, [](TestCpu& t, int i) {
        if (i == 100) { t.Wr(0xFFC01404, 0x80); t.Wr(0xFFC01414, 0x0041); } // TWI_ENA; master read of 1 byte (MEN)
        if (i == 200) t.Wr(0xFFC01404, 0x00);
    });
    ExpectClean("TWI master transfer", o);
    const size_t twi = [&] { for (size_t i = 0; i < o.stats.deviceWork.size(); ++i) if (c.DeviceName(i) == "TWI") return i; return SIZE_MAX; }();
    Check(twi != SIZE_MAX && o.stats.deviceWork[twi] > 0, "TWI master transfer", "an enabled master is TWI work");
}

// GP timers tick in the pass only on its host-time sample (cycles % 10000, TEMPORARY), so a
// running timer is work exactly on those passes. Host time decides which blocks tick, so the
// check is that every eligible block verifies and that ticks did happen.
void GpTimerTicks() {
    ScopedEnv profile("OP1EMU_PROFILE", "functional");
    TestCpu c;
    Boot(c, [](TestCpu& t) { t.GetEmulator().MemoryWrite16(0x200, JUMP_SELF); });
    c.Wr(0xFFC00600, 0x0009);     // TIMER0_CONFIG: PWM_OUT, PERIOD_CNT, SCLK, no IRQ
    c.Wr(0xFFC00608, 0x7FFFFFFF); // PERIOD: never reached in this run
    c.Wr(0xFFC0060C, 1);
    c.Wr(0xFFC00680, 1);          // TIMER_ENABLE
    Outcome o;
    for (int round = 0; round < 50 && c.Rd(0xFFC00604) < 4; ++round) o = RunVerified(c, 2000);
    ExpectClean("GP timer host-time ticks", o);
    Check(c.Rd(0xFFC00604) >= 4, "GP timer host-time ticks", "the running timer was ticked by service passes");
    Check(o.stats.eligible > 0, "GP timer host-time ticks", "passes without a due tick stay eligible");
}

void UsbHostEvents() {
    ScopedEnv profile("OP1EMU_PROFILE", "functional");
    TestCpu c;
    Boot(c, [](TestCpu& t) { t.GetEmulator().MemoryWrite16(0x200, JUMP_SELF); });
    c.Wr(0xFFC03830, 0x0001 | (1u << 1) | (1u << 8)); // USB enabled, EP1 TX/RX
    bool done = false;
    auto o = RunVerified(c, 600, [&](TestCpu& t, int i) {
        if (i == 200) {
            const u8 data[2] = {1, 2};
            t.GetUSB().HandleDataWrite(1, 0, data, sizeof(data), [&done] { done = true; });
        }
        if (i == 400) { t.Rd(0xFFC03888); t.Wr(0xFFC0384C + 0x200, 0); }
    });
    ExpectClean("USB host data", o);
    const size_t usb = [&] { for (size_t i = 0; i < o.stats.deviceWork.size(); ++i) if (c.DeviceName(i) == "USB") return i; return SIZE_MAX; }();
    Check(usb != SIZE_MAX && o.stats.deviceWork[usb] >= 150, "USB host data", "a pending host request keeps USB not idle");
    Check(done, "USB host data", "the host request completed");
}

void InterruptAndRti() {
    ScopedEnv profile("OP1EMU_PROFILE", "functional");
    TestCpu c;
    Boot(c, [](TestCpu& t) {
        auto& m = t.GetEmulator();
        m.MemoryWrite16(0x200, JUMP_SELF);
        m.MemoryWrite16(0x300, NOP); m.MemoryWrite16(0x302, RTI);
        evt_set(7, 0x300);
    });
    cec_push_reti(&c.State());
    cec_mmr_write(&c.State(), CEC_MMR_BASE + 4, (1u << 15) | (1u << 7));
    c.Wr(0xFFC0010C, 1u << 2); // SIC pin 2 -> IVG7
    auto o = RunVerified(c, 400, [](TestCpu& t, int i) {
        if (i == 100 || i == 250) t.Pin(2, 1);
        if (t.PC() == 0x300) t.Pin(2, 0);
    });
    ExpectClean("SIC interrupt, ISR and RTI", o);
    Check(Saw(o, BlackFinCpu::BarrierQueuedEvents) && Saw(o, BlackFinCpu::BarrierInterruptChange), "SIC interrupt, ISR and RTI",
          "latch requests and dispatch/RTI are barriers");
}

void IdleAndPll() {
    ScopedEnv profile("OP1EMU_PROFILE", "functional");
    TestCpu c;
    c.GetEmulator().MemoryWrite16(0x100, IDLE);
    c.GetEmulator().MemoryWrite16(0x102, JUMP_SELF);
    c.GetEmulator().MemoryWrite16(0xFFC00008, 0x70B0); // same-value VR: awake PLL bypass countdown
    c.SetPC(0x100);
    auto o = RunVerified(c, 200);
    ExpectClean("IDLE and PLL bypass countdown", o);
    Check(Saw(o, BlackFinCpu::BarrierIdle) && Saw(o, BlackFinCpu::BarrierPllCountdown), "IDLE and PLL bypass countdown",
          "IDLE and an outstanding PLL countdown are barriers");
}

void SimultaneousDeadlines() {
    ScopedEnv profile("OP1EMU_PROFILE", "functional");
    TestCpu c;
    Boot(c, [](TestCpu& t) { t.GetEmulator().MemoryWrite16(0x200, JUMP_SELF); });
    StartSport(c);
    c.Wr(0xFFE03004, 9070); // about one SPORT frame of CCLK
    c.Wr(0xFFE03000, 7);
    auto o = RunVerified(c, 60000, [](TestCpu& t, int i) {
        if ((i & 2047) == 0) for (u32 ch : {0xFFC00CC0u, 0xFFC00D00u}) if (t.Rd(ch + 0x28) & 1u) t.Wr(ch + 0x28, 1);
    });
    ExpectClean("core timer + SPORT deadlines", o);
    Check(Saw(o, BlackFinCpu::BarrierGuestDeadline) && Saw(o, BlackFinCpu::BarrierDeviceWork), "core timer + SPORT deadlines",
          "both deadline kinds observed");
}

// Diagnostics must not change execution: identical per-block state and MMIO.
void DiagnosticsArePassive() {
    auto run = [](bool diagnostics) {
        ScopedEnv profile("OP1EMU_PROFILE", "functional");
        TestCpu c;
        c.Dma().SetWriteTracking(true);
        Boot(c, [](TestCpu& t) { t.GetEmulator().MemoryWrite16(0x200, STORE_P0_R0); t.GetEmulator().MemoryWrite16(0x202, JUMP_BACK); });
        c.State().dpregs[8] = 0xFFC00110;
        c.State().dpregs[0] = c.Rd(0xFFC00110);
        StartSport(c);
        c.Wr(0xFFE03004, 4000);
        c.Wr(0xFFE03000, 7);
        if (diagnostics) { c.SetBatchDiagnostics(true, true); c.Dma().SetWriteTracking(true); }
        std::vector<MMIOAccess> log;
        c.SetMMIOLog(&log);
        std::vector<BlackFinCpu::BlockState> states;
        for (int i = 0; i < 20000; ++i) {
            if ((i & 2047) == 0) for (u32 ch : {0xFFC00CC0u, 0xFFC00D00u}) if (c.Rd(ch + 0x28) & 1u) c.Wr(ch + 0x28, 1);
            c.Run();
            if ((i & 15) == 0) states.push_back(c.CaptureBlockState());
        }
        c.SetMMIOLog(nullptr);
        return std::make_pair(states, log);
    };
    const auto off = run(false);
    const auto on = run(true);
    bool same = off.first.size() == on.first.size() && off.second == on.second;
    for (size_t i = 0; same && i < off.first.size(); ++i)
        if (!BlackFinCpu::DiffBlockState(off.first[i], on.first[i]).empty()) {
            std::fprintf(stderr, "first difference at sample %zu: %s\n", i, BlackFinCpu::DiffBlockState(off.first[i], on.first[i]).c_str());
            same = false;
        }
    Check(same, "diagnostics passive", "state and MMIO sequence identical with diagnostics off and on");
    std::printf("%-34s %zu state samples and %zu MMIO accesses identical\n", "diagnostics passive", off.first.size(), off.second.size());
}
}

int main() {
    QuietCpu();
    ShortHardwareLoop();
    MmioBlocks();
    CoreTimerDeadline();
    SportDmaActivity();
    NandNfcWork();
    TwiActivity();
    GpTimerTicks();
    UsbHostEvents();
    InterruptAndRti();
    IdleAndPll();
    SimultaneousDeadlines();
    DiagnosticsArePassive();
    if (failures) return 1;
    std::puts("multiblock eligibility tests passed");
    return 0;
}
