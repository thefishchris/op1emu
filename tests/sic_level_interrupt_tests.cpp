// Level-sensitive SIC -> CEC delivery (docs/sic-interrupt-delivery.md),
// checked against the documented BF52x/Blackfin behaviour, not against the
// previous emulator:
//  - SIC inputs are levels; rewriting an unchanged level is not an event
//    (HRM 5-5, PGR 4-53);
//  - the CEC latches ILAT on a rising edge of an IVG line (PGR 4-53);
//  - a re-trigger of an IVG being serviced is not latched (PGR 4-41);
//  - if the source is still asserted when the service ends (RTI), ILAT is set
//    again at once and the ISR re-runs (HRM 5-13, PGR 4-54);
//  - a CEC-masked latch stays set (PGR 4-41); SIC_IMASK gates the request.
#include "cpu/cpu.h"
#include "cpu/nand.h"
#include "cpu/sic.h"
#include "cec.h"
#include "cpu_state.h"
#include "evt.h"
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <vector>

namespace {
int failures = 0;
void Expect(bool condition, const char* scenario, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "sic level interrupts: %s: %s\n", scenario, what);
        ++failures;
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
    const SIC::Stats& Sic() { return sic->Counters(); }
};

constexpr u32 SIC_IMASK0 = 0xFFC0010C, SIC_IAR0 = 0xFFC00110;
constexpr u16 RTI = 0x0011, JUMP_SELF = 0x2000, JUMP_NEXT = 0x2001;
// Unused SIC inputs: A, B -> IVG7 (IAR0 reset 0); C -> IVG8 (IAR1 reset 0x11000000).
constexpr int PIN_A = 2, PIN_B = 3, PIN_C = 15;
constexpr u32 IV7 = 1u << 7, IV8 = 1u << 8, A = 1u << PIN_A, B = 1u << PIN_B, C = 1u << PIN_C;
constexpr u32 ISR7 = 0x300, ISR8 = 0x400; // 3-block ISRs: entry, entry+2, entry+4 = RTI

// IVG15 loop at 0x200 (nesting enabled, as an IVG15 handler would).
void Load(TestCpu& cpu, u32 cecImask, u32 sicImask) {
    auto& m = cpu.GetEmulator();
    m.MemoryWrite16(0x100, RTI);
    m.MemoryWrite16(0x200, JUMP_SELF);
    for (u32 base : {ISR7, ISR8}) {
        m.MemoryWrite16(base, JUMP_NEXT);
        m.MemoryWrite16(base + 2, JUMP_NEXT);
        m.MemoryWrite16(base + 4, RTI);
    }
    cpu.State().reti = 0x200;
    evt_set(15, 0x200);
    evt_set(7, ISR7);
    evt_set(8, ISR8);
    cec_mmr_write(&cpu.State(), CEC_MMR_BASE + 4, (1u << 15) | cecImask);
    cec_raise(&cpu.State(), 15);
    cpu.W32(SIC_IMASK0, sicImask);
    cpu.SetPC(0x100);
    cpu.Run();
    cec_push_reti(&cpu.State());
}

struct Entry { int ivg; u32 reti; };
struct Result { std::vector<Entry> entries; unsigned count7 = 0, count8 = 0; };

// Runs `blocks` blocks; `hook` models the peripheral before each block. An ISR
// entry is a block starting at its vector; RETI there is the saved return PC.
struct Runner {
    TestCpu& cpu;
    Result r;
    void Steps(int blocks, const std::function<void(TestCpu&)>& hook = nullptr) {
        for (int i = 0; i < blocks; ++i) {
            const u32 pc = cpu.PC();
            if (pc == ISR7) { r.entries.push_back({7, cpu.State().reti}); ++r.count7; }
            if (pc == ISR8) { r.entries.push_back({8, cpu.State().reti}); ++r.count8; }
            if (hook) hook(cpu);
            cpu.Run();
        }
    }
};

template <typename Body> Result Scenario(u32 cecImask, u32 sicImask, Body body) {
    TestCpu cpu;
    Load(cpu, cecImask, sicImask);
    Runner run{cpu, {}};
    body(cpu, run);
    return run.r;
}

void HeldHighReVectorsAfterEveryRti() {
    const char* s = "held HIGH 120 blocks";
    auto r = Scenario(IV7, A, [](TestCpu& c, Runner& run) {
        c.Pin(PIN_A, 1);
        run.Steps(120);
    });
    // Each 3-block service ends in RTI; the asserted line re-pends at once, so
    // the ISR re-runs back to back with no IVG15 code in between.
    Expect(r.count7 >= 38 && r.count7 <= 40, s, "re-vectored after every RTI");
    for (size_t i = 1; i < r.entries.size(); ++i)
        Expect(r.entries[i].reti == r.entries[0].reti, s, "each re-entry returns to the interrupted IVG15 code");
}

void ClearAtEntry() {
    auto r = Scenario(IV7, A, [](TestCpu& c, Runner& run) {
        c.Pin(PIN_A, 1);
        run.Steps(60, [](TestCpu& t) { if (t.PC() == ISR7) t.Pin(PIN_A, 0); });
    });
    Expect(r.count7 == 1 && r.entries[0].reti == 0x200, "clear at ISR entry", "exactly one entry, returning to 0x200");
}

void ClearAfterEntry() {
    auto r = Scenario(IV7, A, [](TestCpu& c, Runner& run) {
        c.Pin(PIN_A, 1);
        run.Steps(60, [](TestCpu& t) { if (t.PC() == ISR7 + 2) t.Pin(PIN_A, 0); });
    });
    Expect(r.count7 == 1, "clear after ISR entry", "one entry");
}

void ClearJustBeforeRti() {
    auto r = Scenario(IV7, A, [](TestCpu& c, Runner& run) {
        c.Pin(PIN_A, 1);
        run.Steps(60, [](TestCpu& t) { if (t.PC() == ISR7 + 4) t.Pin(PIN_A, 0); });
    });
    Expect(r.count7 == 1, "clear just before RTI", "one entry: no latch while in service (old emulator: 2)");
}

void StillAssertedAtRtiThenCleared() {
    auto r = Scenario(IV7, A, [](TestCpu& c, Runner& run) {
        c.Pin(PIN_A, 1);
        int seen = 0;
        run.Steps(60, [&seen](TestCpu& t) { if (t.PC() == ISR7 && ++seen == 2) t.Pin(PIN_A, 0); });
    });
    Expect(r.count7 == 2, "asserted at first RTI, cleared in second service", "exactly two entries");
}

void RetriggerWhileInService() {
    auto r = Scenario(IV7, A, [](TestCpu& c, Runner& run) {
        c.Pin(PIN_A, 1);
        run.Steps(60, [](TestCpu& t) {
            if (t.PC() == ISR7) t.Pin(PIN_A, 0);                       // acknowledge
            if (t.PC() == ISR7 + 2 && t.Sic().levelChanges < 3) t.Pin(PIN_A, 1); // new request during service
            if (t.PC() == ISR7 + 4) t.Pin(PIN_A, 0);                   // cleared again before RTI
        });
    });
    Expect(r.count7 == 1, "re-trigger during service, cleared before RTI", "one entry: the in-service edge is not latched");
}

void RetriggerWhileInServiceStillHigh() {
    auto r = Scenario(IV7, A, [](TestCpu& c, Runner& run) {
        c.Pin(PIN_A, 1);
        int entries = 0;
        run.Steps(60, [&entries](TestCpu& t) {
            if (t.PC() == ISR7) { ++entries; t.Pin(PIN_A, 0); }
            if (t.PC() == ISR7 + 2 && entries == 1) t.Pin(PIN_A, 1); // new request, left asserted at RTI
        });
    });
    Expect(r.count7 == 2, "re-trigger during service, asserted at RTI", "re-pended at RTI: two entries");
}

void RepeatedHighWritesAreInvisible() {
    auto r = Scenario(IV7, A, [](TestCpu& c, Runner& run) {
        for (int k = 0; k < 10; ++k) c.Pin(PIN_A, 1);
        run.Steps(60, [](TestCpu& t) {
            if (t.PC() == ISR7) t.Pin(PIN_A, 0);
            else for (int k = 0; k < 5; ++k) t.Pin(PIN_A, 0); // unchanged LOW rewrites
        });
    });
    Expect(r.count7 == 1, "repeated HIGH/LOW writes", "one entry for one rising edge");
}

void CecMaskedLatchPersists() {
    auto r = Scenario(0, A, [](TestCpu& c, Runner& run) {
        c.Pin(PIN_A, 1);
        run.Steps(10);
        c.Pin(PIN_A, 0);
        run.Steps(10);
        Expect(cec_mmr_read(&c.State(), CEC_MMR_BASE + 12) & IV7, "CEC-masked edge", "ILAT[7] stays latched while masked");
        cec_mmr_write(&c.State(), CEC_MMR_BASE + 4, (1u << 15) | IV7);
        run.Steps(30);
    });
    Expect(r.count7 == 1, "CEC-masked, source gone, then unmasked", "the latched request is delivered once");
}

void SicMaskUnmask() {
    auto r = Scenario(IV7, 0, [](TestCpu& c, Runner& run) {
        c.Pin(PIN_A, 1);
        run.Steps(20);
        Expect(!(cec_mmr_read(&c.State(), CEC_MMR_BASE + 12) & IV7), "SIC-masked", "no latch while SIC-masked");
        c.W32(SIC_IMASK0, A);
        run.Steps(40, [](TestCpu& t) { if (t.PC() == ISR7) t.Pin(PIN_A, 0); });
    });
    Expect(r.count7 == 1, "SIC unmask while asserted", "unmask raises the line: one entry");
    auto gone = Scenario(IV7, 0, [](TestCpu& c, Runner& run) {
        c.Pin(PIN_A, 1);
        run.Steps(20);
        c.Pin(PIN_A, 0);
        c.W32(SIC_IMASK0, A);
        run.Steps(40);
    });
    Expect(gone.count7 == 0, "SIC unmask after deassert", "no request remains: no entry");
}

void TwoSourcesOneIvg() {
    auto r = Scenario(IV7, A | B, [](TestCpu& c, Runner& run) {
        c.Pin(PIN_A, 1);
        c.Pin(PIN_B, 1);
        int n = 0;
        run.Steps(80, [&n](TestCpu& t) { if (t.PC() == ISR7) t.Pin(n++ == 0 ? PIN_A : PIN_B, 0); });
    });
    Expect(r.count7 == 2, "two sources on IVG7, one serviced per entry", "two entries (line still high at first RTI)");
}

void TwoIvgsPriority() {
    auto r = Scenario(IV7 | IV8, A | C, [](TestCpu& c, Runner& run) {
        c.Pin(PIN_C, 1);
        c.Pin(PIN_A, 1);
        run.Steps(80, [](TestCpu& t) { if (t.PC() == ISR7) t.Pin(PIN_A, 0); if (t.PC() == ISR8) t.Pin(PIN_C, 0); });
    });
    Expect(r.count7 == 1 && r.count8 == 1, "IVG7 and IVG8 both asserted", "one entry each");
    Expect(r.entries.size() == 2 && r.entries[0].ivg == 7 && r.entries[1].ivg == 8, "IVG7 and IVG8 both asserted",
           "higher priority IVG7 first");
}

void RoutingChange() {
    auto r = Scenario(IV8, A, [](TestCpu& c, Runner& run) {
        c.W32(SIC_IAR0, 1u << (PIN_A * 4)); // pin 2 -> IVG8
        c.Pin(PIN_A, 1);
        run.Steps(60, [](TestCpu& t) { if (t.PC() == ISR8) t.Pin(PIN_A, 0); });
    });
    Expect(r.count8 == 1 && r.count7 == 0, "IAR routes the source to IVG8", "one entry at IVG8");
    auto moved = Scenario(IV8, A, [](TestCpu& c, Runner& run) {
        c.Pin(PIN_A, 1); // Asserted on IVG7, which is CEC-masked.
        run.Steps(10);
        c.W32(SIC_IAR0, 1u << (PIN_A * 4)); // Re-route while asserted: IVG8 line rises.
        run.Steps(60, [](TestCpu& t) { if (t.PC() == ISR8) t.Pin(PIN_A, 0); });
    });
    Expect(moved.count8 == 1, "re-route while asserted", "the routing change delivers at the new IVG");
}

void NestedHigherPriority() {
    constexpr u16 PUSH_RETI = 0x017B, POP_RETI = 0x013B; // [--SP] = RETI; RETI = [SP++]
    auto r = Scenario(IV7 | IV8, A | C, [](TestCpu& c, Runner& run) {
        // A nesting IVG8 ISR: save RETI (re-enables interrupts), two blocks, restore, RTI.
        auto& m = c.GetEmulator();
        m.MemoryWrite16(ISR8, PUSH_RETI);
        m.MemoryWrite16(ISR8 + 2, JUMP_NEXT);
        m.MemoryWrite16(ISR8 + 4, JUMP_NEXT);
        m.MemoryWrite16(ISR8 + 6, POP_RETI);
        m.MemoryWrite16(ISR8 + 8, RTI);
        c.SetRegister(RegIndex::SP, 0xFF900F00);
        c.Pin(PIN_C, 1);
        run.Steps(80, [](TestCpu& t) {
            // IVG7 arrives during IVG8's first block. Delivery takes one emulated
            // block (latched after a block, dispatched after the next), as before.
            if (t.PC() == ISR8) { t.Pin(PIN_C, 0); if (t.Sic().levelChanges < 3) t.Pin(PIN_A, 1); }
            if (t.PC() == ISR7) t.Pin(PIN_A, 0);
        });
    });
    Expect(r.count7 == 1 && r.count8 == 1, "IVG7 during IVG8 (nesting enabled)", "one entry each");
    Expect(r.entries.size() == 2 && r.entries[0].ivg == 8 && r.entries[1].ivg == 7 &&
           r.entries[1].reti > ISR8 && r.entries[1].reti <= ISR8 + 8,
           "IVG7 during IVG8 (nesting enabled)", "IVG7 nests and returns into the IVG8 ISR");
    Expect(r.entries.size() == 2 && r.entries[0].reti == 0x200, "IVG7 during IVG8 (nesting enabled)",
           "IVG8 itself returns to IVG15");
}

void LowerPriorityWaits() {
    auto r = Scenario(IV7 | IV8, A | C, [](TestCpu& c, Runner& run) {
        c.Pin(PIN_A, 1);
        run.Steps(80, [](TestCpu& t) {
            if (t.PC() == ISR7) t.Pin(PIN_A, 0);
            if (t.PC() == ISR7 + 2 && t.Sic().levelChanges < 3) t.Pin(PIN_C, 1); // IVG8 arrives mid-IVG7
            if (t.PC() == ISR8) t.Pin(PIN_C, 0);
        });
    });
    Expect(r.entries.size() == 2 && r.entries[0].ivg == 7 && r.entries[1].ivg == 8 && r.entries[1].reti == 0x200,
           "IVG8 during IVG7", "lower priority waits for IVG7's RTI, then returns to IVG15");
}

// Regression: a dispatch earlier in the same Run() (here the core timer's
// IVG6) must not swallow an SIC latch request made later in that Run().
void EdgeInSameRunAsAnotherDispatch() {
    constexpr u32 ISR6 = 0x500;
    unsigned entries6 = 0;
    auto r = Scenario(IV7 | (1u << 6), A, [&entries6](TestCpu& c, Runner& run) {
        auto& m = c.GetEmulator();
        m.MemoryWrite16(ISR6, JUMP_NEXT);
        m.MemoryWrite16(ISR6 + 2, JUMP_NEXT);
        m.MemoryWrite16(ISR6 + 4, RTI);
        evt_set(6, ISR6);
        bool fired = false;
        run.Steps(80, [&](TestCpu& t) {
            if (t.PC() == ISR6) ++entries6;
            if (!fired && t.PC() == 0x200) { fired = true; cec_raise(&t.State(), 6); t.Pin(PIN_A, 1); }
            if (t.PC() == ISR7) t.Pin(PIN_A, 0);
        });
    });
    Expect(entries6 == 1 && r.count7 == 1, "SIC edge in the Run() of another dispatch",
           "both delivered: IVG6, then IVG7 (the IVG7 request is not lost)");
}

void SteadyStateIsQuiet() {
    TestCpu cpu;
    Load(cpu, IV7, A);
    const auto before = cpu.RaiseStats();
    const auto sicBefore = cpu.Sic();
    for (int k = 0; k < 1000; ++k) cpu.Pin(PIN_A, 0);
    for (int i = 0; i < 100; ++i) cpu.Run();
    const auto after = cpu.RaiseStats();
    Expect(after.requested == before.requested && cpu.Sic().lineRises == sicBefore.lineRises,
           "steady state", "unchanged levels and other devices' writes forward nothing");
}
}

int main() {
    HeldHighReVectorsAfterEveryRti();
    ClearAtEntry();
    ClearAfterEntry();
    ClearJustBeforeRti();
    StillAssertedAtRtiThenCleared();
    RetriggerWhileInService();
    RetriggerWhileInServiceStillHigh();
    RepeatedHighWritesAreInvisible();
    CecMaskedLatchPersists();
    SicMaskUnmask();
    TwoSourcesOneIvg();
    TwoIvgsPriority();
    RoutingChange();
    NestedHigherPriority();
    LowerPriorityWaits();
    EdgeInSameRunAsAnotherDispatch();
    SteadyStateIsQuiet();
    if (failures) return 1;
    std::puts("sic level interrupt tests passed");
    return 0;
}
