// SIC -> CEC raise de-duplication (BlackFinCpu::QueueInterruptRaise): within
// one event pass a repeated raise of the same IVG is not queued. These tests
// prove the guest sees exactly the same interrupt behaviour with and without
// it, including the current re-latch-while-in-service quirk (to be revisited
// separately), and that only same-pass duplicates are collapsed.
#include "cpu/cpu.h"
#include "cpu/nand.h"
#include "cec.h"
#include "cpu_state.h"
#include "evt.h"
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <vector>

namespace {
void Check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "interrupt raise dedupe: %s\n", what);
        std::exit(1);
    }
}

class NoNand final : public NandFlash {
public:
    void SendCommand(u8) override {}
    void SendAddress(u8) override {}
    u8 ReadData() override { return 0; }
    void WriteData(u8) override {}
    bool IsDataReady() const override { return false; }
    bool IsBusy() const override { return false; }
    void StartPageRead() override {}
    void StartPageWrite() override {}
    u32 PageRead(u8*, u32) override { return 0; }
    u32 PageWrite(const u8*, u32) override { return 0; }
    void SetReadCallback(ReadCallback) override {}
};

class TestCpu : public BlackFinCpu {
public:
    TestCpu() { AttachNandFlash(std::make_shared<NoNand>()); }
    CpuState& State() { return *cpuState_; }
    void Pin(int pin, int level) { ProcessInterrupt(pin, level); }
    void W32(u32 a, u32 v) { GetEmulator().MemoryWrite32(a, v); }
    u32 R32(u32 a) { return GetEmulator().MemoryRead32(a); }
};

constexpr u32 SIC_IMASK0 = 0xFFC0010C, SIC_IAR0 = 0xFFC00110, SIC_ISR0 = 0xFFC00120;
constexpr u16 RTI = 0x0011, JUMP_SELF = 0x2000, JUMP_NEXT = 0x2001;
constexpr int PIN_A = 2, PIN_B = 3, PIN_C = 15; // A, B -> IVG7 (IAR0 reset 0); C -> IVG8 (IAR1 reset 0x11000000)
constexpr u32 IV7 = 1u << 7, IV8 = 1u << 8, A = 1u << PIN_A, B = 1u << PIN_B, C = 1u << PIN_C;

// IVG15 loop at 0x200; 3-block ISRs at 0x300 (IVG7) and 0x400 (IVG8).
void Load(TestCpu& cpu, u32 cecImask, u32 sicImask) {
    auto& m = cpu.GetEmulator();
    m.MemoryWrite16(0x100, RTI);
    m.MemoryWrite16(0x200, JUMP_SELF);
    for (u32 base : {0x300u, 0x400u}) {
        m.MemoryWrite16(base, JUMP_NEXT);
        m.MemoryWrite16(base + 2, JUMP_NEXT);
        m.MemoryWrite16(base + 4, RTI);
    }
    cpu.State().reti = 0x200;
    evt_set(15, 0x200);
    evt_set(7, 0x300);
    evt_set(8, 0x400);
    cec_mmr_write(&cpu.State(), CEC_MMR_BASE + 4, (1u << 15) | cecImask);
    cec_raise(&cpu.State(), 15);
    cpu.W32(SIC_IMASK0, sicImask);
    cpu.SetPC(0x100);
    cpu.Run();                  // RTI -> IVG15 at 0x200
    cec_push_reti(&cpu.State()); // Nesting enabled, as an IVG15 handler would.
}

struct Trace {
    std::vector<u32> rows; // Per block: PC, RETI, SEQSTAT, IMASK, IPEND, ILAT, SIC ISR/IMASK/IAR0.
    unsigned entries7 = 0, entries8 = 0;
    BlackFinCpu::InterruptRaiseStats stats;
    std::vector<int> entryOrder;
};

using Body = std::function<void(TestCpu&, const std::function<void(int, const std::function<void(TestCpu&)>&)>&)>;

Trace Run(bool dedupe, u32 cecImask, u32 sicImask, const Body& body) {
    TestCpu cpu;
    cpu.SetInterruptRaiseDedupe(dedupe);
    Load(cpu, cecImask, sicImask);
    Trace t;
    auto steps = [&](int blocks, const std::function<void(TestCpu&)>& hook) {
        for (int i = 0; i < blocks; ++i) {
            if (cpu.PC() == 0x300) { ++t.entries7; t.entryOrder.push_back(7); }
            if (cpu.PC() == 0x400) { ++t.entries8; t.entryOrder.push_back(8); }
            if (hook) hook(cpu);
            cpu.Run();
            CpuState& s = cpu.State();
            t.rows.insert(t.rows.end(), {cpu.PC(), s.reti, s.seqstat,
                                         cec_mmr_read(&s, CEC_MMR_BASE + 4), cec_mmr_read(&s, CEC_MMR_BASE + 8),
                                         cec_mmr_read(&s, CEC_MMR_BASE + 12), cpu.R32(SIC_ISR0), cpu.R32(SIC_IMASK0),
                                         cpu.R32(SIC_IAR0)});
        }
    };
    body(cpu, steps);
    t.stats = cpu.RaiseStats();
    return t;
}

// Runs a scenario with and without de-duplication and requires identical traces.
Trace Equivalent(const char* name, u32 cecImask, u32 sicImask, const Body& body) {
    const Trace off = Run(false, cecImask, sicImask, body);
    const Trace on = Run(true, cecImask, sicImask, body);
    if (off.rows != on.rows || off.entryOrder != on.entryOrder) {
        std::fprintf(stderr, "scenario: %s\n", name);
        Check(false, "guest-visible interrupt trace differs with de-duplication");
    }
    Check(off.stats.queued == off.stats.requested, "reference path queues every raise");
    Check(on.stats.requested == off.stats.requested && on.stats.queued <= off.stats.queued,
          "same raise requests; never more queued");
    std::printf("%-46s IVG7 entries %u, IVG8 entries %u; raises requested %llu, queued %llu -> %llu\n", name, on.entries7,
                on.entries8, static_cast<unsigned long long>(on.stats.requested),
                static_cast<unsigned long long>(off.stats.queued), static_cast<unsigned long long>(on.stats.queued));
    return on;
}

void Scenarios() {
    auto held = Equivalent("1 LOW->HIGH, held HIGH 120 blocks", IV7, A, [](TestCpu& c, auto steps) {
        c.Pin(PIN_A, 1);
        steps(120, nullptr);
    });
    Check(held.entries7 >= 30, "current re-pend across RTI preserved: re-entered after nearly every RTI");
    Equivalent("2 HIGH, ISR clears at its first instruction", IV7, A, [](TestCpu& c, auto steps) {
        c.Pin(PIN_A, 1);
        steps(60, [](TestCpu& t) { if (t.PC() == 0x300) t.Pin(PIN_A, 0); });
    });
    auto late = Equivalent("3 HIGH, ISR clears just before RTI", IV7, A, [](TestCpu& c, auto steps) {
        c.Pin(PIN_A, 1);
        steps(60, [](TestCpu& t) { if (t.PC() == 0x304) t.Pin(PIN_A, 0); });
    });
    Check(late.entries7 >= 2, "current extra entry from re-latch while in service preserved (for now)");
    auto dup = Equivalent("4 ten HIGH writes per IVG15 block, clear at entry", IV7, A, [](TestCpu& c, auto steps) {
        c.Pin(PIN_A, 1);
        steps(60, [](TestCpu& t) {
            if (t.PC() == 0x300) t.Pin(PIN_A, 0);
            else if (t.PC() == 0x200) for (int k = 0; k < 10; ++k) t.Pin(PIN_A, 1);
        });
    });
    Check(dup.stats.queued * 10 < dup.stats.requested, "repeated HIGH writes collapse to far fewer queued raises");
    Equivalent("5 HIGH->LOW before service while CEC-masked", 0, A, [](TestCpu& c, auto steps) {
        c.Pin(PIN_A, 1);
        steps(10, nullptr);
        c.Pin(PIN_A, 0);
        steps(10, nullptr);
        cec_mmr_write(&c.State(), CEC_MMR_BASE + 4, (1u << 15) | IV7);
        steps(30, nullptr);
    });
    Equivalent("6 SIC-masked while HIGH, unmask, clear at entry", IV7, 0, [](TestCpu& c, auto steps) {
        c.Pin(PIN_A, 1);
        steps(20, nullptr);
        c.W32(SIC_IMASK0, A);
        steps(40, [](TestCpu& t) { if (t.PC() == 0x300) t.Pin(PIN_A, 0); });
    });
    Equivalent("7 SIC-masked while HIGH, LOW before unmask", IV7, 0, [](TestCpu& c, auto steps) {
        c.Pin(PIN_A, 1);
        steps(20, nullptr);
        c.Pin(PIN_A, 0);
        c.W32(SIC_IMASK0, A);
        steps(40, nullptr);
    });
    Equivalent("8 two sources on IVG7, one cleared per entry", IV7, A | B, [](TestCpu& c, auto steps) {
        c.Pin(PIN_A, 1);
        c.Pin(PIN_B, 1);
        int n = 0;
        steps(80, [&n](TestCpu& t) { if (t.PC() == 0x300) t.Pin(n++ == 0 ? PIN_A : PIN_B, 0); });
    });
    auto two = Equivalent("9 IVG7 and IVG8 both HIGH, clear at entry", IV7 | IV8, A | C, [](TestCpu& c, auto steps) {
        c.Pin(PIN_A, 1);
        c.Pin(PIN_C, 1);
        steps(80, [](TestCpu& t) { if (t.PC() == 0x300) t.Pin(PIN_A, 0); if (t.PC() == 0x400) t.Pin(PIN_C, 0); });
    });
    Check(two.entries7 >= 1 && two.entries8 >= 1 && !two.entryOrder.empty() && two.entryOrder.front() == 7,
          "different IVGs are not collapsed; IVG7 (higher priority) first");
    Equivalent("10 IAR remaps IVG7->IVG8 while HIGH", IV8, A, [](TestCpu& c, auto steps) {
        c.W32(SIC_IAR0, 1u << (PIN_A * 4));
        c.Pin(PIN_A, 1);
        steps(60, [](TestCpu& t) { if (t.PC() == 0x400) t.Pin(PIN_A, 0); });
    });
}

// Direct per-pass accounting: duplicates between two passes collapse, the
// same raise after the next pass is queued again, other IVGs are separate.
void PassScope() {
    TestCpu cpu;
    Load(cpu, 0, A | B | C); // CEC-masked: latches only, no dispatch noise.
    const auto base = cpu.RaiseStats();
    cpu.Pin(PIN_A, 1);
    for (int k = 0; k < 9; ++k) cpu.Pin(PIN_A, 1);
    auto s = cpu.RaiseStats();
    Check(s.requested - base.requested == 10 && s.queued - base.queued == 1,
          "ten forwards of IVG7 before one pass queue one raise");
    cpu.Pin(PIN_C, 1);
    s = cpu.RaiseStats();
    Check(s.queued - base.queued == 2, "a different IVG (IVG8) is queued separately");
    const auto before = cpu.RaiseStats();
    cpu.Run(); // One block: one ProcessEvents pass.
    cpu.Pin(PIN_A, 1);
    s = cpu.RaiseStats();
    Check(s.queued > before.queued, "after a pass the same IVG is queued again");
    for (int i = 0; i < 3; ++i) cpu.Run(); // Queued raises execute within the next passes.
    Check(cec_mmr_read(&cpu.State(), CEC_MMR_BASE + 12) & IV7, "IVG7 latched in ILAT (masked at the CEC)");
    Check(cec_mmr_read(&cpu.State(), CEC_MMR_BASE + 12) & IV8, "IVG8 latched in ILAT (masked at the CEC)");
}
}

int main() {
    Scenarios();
    PassScope();
    std::puts("interrupt raise dedupe tests passed");
    return 0;
}
