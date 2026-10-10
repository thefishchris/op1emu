// Multi-block execution, milestone M1 (docs/multiblock-execution.md).
// Every scenario runs twice from the same start: one device pass per block
// (batching off) and batched (on, with every incremental proof cross-checked
// in full). The complete captured state after every guest block, the MMIO
// sequence and guest memory must be identical.
//
// Determinism: host time is injected as a function of guest CLKIN time, so
// both runs see the same host time at the same guest block. Host inputs
// (register pokes, NAND state, SIC pins, USB host requests) are injected at
// the same block in both runs, through the paths real host inputs use.
#include "cpu/cpu.h"
#include "cpu/dma.h"
#include "cpu/guest_coretimer.h"
#include "cpu/nand.h"
#include "cpu/sport.h"
#include "cpu/usb.h"
#include "cec.h"
#include "cpu_state.h"
#include "evt.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
int failures = 0;

void Check(bool condition, const char* scenario, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "multiblock execution: %s: %s\n", scenario, what);
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
    // A host-side register write: delivered as an event, as frontend input is.
    void HostWr(u32 a, u32 v) { QueueEvent([this, a, v] { Wr(a, v); }, std::chrono::nanoseconds(0)); }
    void Host(std::function<void()> f) { QueueEvent(std::move(f), std::chrono::nanoseconds(0)); }
};

// Host time = fixed base + scaled guest CLKIN time.
const TestCpu* clockCpu = nullptr;
uint64_t hostNsPerClkin = 40;
std::chrono::system_clock::time_point TestClock() {
    const uint64_t clkin = clockCpu ? clockCpu->GuestTime().ClkinTicks() : 0;
    return std::chrono::system_clock::time_point(std::chrono::seconds(1700000000)) +
           std::chrono::nanoseconds(static_cast<int64_t>(clkin * hostNsPerClkin));
}

constexpr u16 IDLE = 0x0020, RTI = 0x0011, JUMP_SELF = 0x2000, JUMP_NEXT = 0x2001, NOP = 0x0000;
constexpr u16 STORE_P0_R0 = 0x9300, STORE_P0_INC_R0 = 0x9200, R0_ADD_1 = 0x6408, R1_ADD_1 = 0x6409, P0_EQ_P2 = 0x3242;
constexpr u16 PUSH_RETI = 0x017B, POP_RETI = 0x013B;
constexpr u32 LSETUP_NEXT2_LC0_P1 = 0xE0A21003; // lsetup(pc+4, pc+6) lc0 = P1
u16 JumpBack(u32 bytes) { return static_cast<u16>(0x3000u - bytes / 2); } // jump.s -bytes

void Put32(TestCpu& c, u32 at, u32 insn) {
    c.GetEmulator().MemoryWrite16(at, static_cast<u16>(insn >> 16));
    c.GetEmulator().MemoryWrite16(at + 2, static_cast<u16>(insn));
}

// Functional profile: PLL relock IDLE at 0x110, RTI into IVG15 at 0x200.
void Boot(TestCpu& c) {
    auto& m = c.GetEmulator();
    m.MemoryWrite16(0x110, IDLE);
    m.MemoryWrite16(0x112, RTI);
    m.MemoryWrite16(0xFFC00004, 5);
    m.MemoryWrite16(0xFFC00000, 0x2000);
    c.State().reti = 0x200;
    evt_set(15, 0x200);
    cec_mmr_write(&c.State(), CEC_MMR_BASE + 4, 1u << 15);
    cec_raise(&c.State(), 15);
    c.SetPC(0x110);
}

struct Scenario {
    const char* name;
    uint64_t blocks;
    uint64_t nsPerClkin = 40; // host ns per guest CLKIN tick (40: real time at 25 MHz)
    std::function<void(TestCpu&)> setup;
    std::function<void(TestCpu&, uint64_t block)> inject; // after each block, in both runs
    std::function<uint64_t(TestCpu&)> extraHash;          // optional per-block state (e.g. memory)
    std::function<void(const char*, const BlackFinCpu::MultiBlockStats&, TestCpu&)> expect;
};

uint64_t Mix(uint64_t h, uint64_t v) { for (int i = 0; i < 8; ++i) { h ^= (v >> (i * 8)) & 0xFF; h *= 1099511628211ull; } return h; }

uint64_t HashState(const BlackFinCpu::BlockState& s) {
    uint64_t h = 1469598103934665603ull;
    for (const auto& [name, value] : s) {
        for (char ch : name) h = Mix(h, static_cast<unsigned char>(ch));
        h = Mix(h, value);
    }
    return h;
}

uint64_t HashMemory(TestCpu& c, u32 from, u32 bytes) {
    uint64_t h = 1469598103934665603ull;
    for (u32 a = from; a < from + bytes; a += 4) h = Mix(h, c.Rd(a));
    return h;
}

struct Trace {
    std::vector<uint64_t> states;
    std::vector<MMIOAccess> mmio;
    uint64_t memory = 0;
    BlackFinCpu::MultiBlockStats stats;
    std::optional<BlackFinCpu::BlockState> captured; // full state at `captureAt`
    uint64_t runsAtEnd = 0;
    uint64_t maxBlocksPerRun = 0;
};

Trace RunScenario(const Scenario& s, bool batched, bool crossCheck, uint64_t captureAt = UINT64_MAX) {
    ScopedEnv profile("OP1EMU_PROFILE", "functional");
    TestCpu c;
    hostNsPerClkin = s.nsPerClkin;
    clockCpu = &c;
    c.SetHostClock(TestClock);
    c.Dma().SetWriteTracking(true);
    Boot(c);
    if (s.setup) s.setup(c);
    c.Run(); // PLL lock (IDLE)
    c.Run(); // RTI into IVG15
    cec_push_reti(&c.State()); // IVG15 runs with nesting enabled, as the firmware's main loop does
    if (batched) c.SetMultiBlock(true, crossCheck);
    Trace t;
    std::vector<MMIOAccess> log;
    c.SetMMIOLog(&log);
    uint64_t block = 0, inRun = 0;
    size_t mmioAtEnd = 0;
    c.SetBlockObserver([&] {
        ++inRun;
        if (block < s.blocks) {
            auto state = c.CaptureBlockState();
            if (block == captureAt) t.captured = state;
            uint64_t h = HashState(state);
            if (s.extraHash) h = Mix(h, s.extraHash(c));
            t.states.push_back(h);
            if (s.inject) s.inject(c, block);
            if (block + 1 == s.blocks) {
                mmioAtEnd = log.size();
                t.memory = HashMemory(c, 0xFF800000, 0x4000) ^ HashMemory(c, 0xFF900000, 0x4000) ^ HashMemory(c, 0, 0x10000);
            }
        }
        ++block;
    });
    uint64_t runs = 0;
    while (block < s.blocks) {
        inRun = 0;
        c.Run();
        ++runs;
        if (inRun > t.maxBlocksPerRun) t.maxBlocksPerRun = inRun;
        if (c.Failed() || c.UnsupportedIdle()) { Check(false, s.name, "workload keeps running"); break; }
    }
    c.SetBlockObserver(nullptr);
    c.SetMMIOLog(nullptr);
    log.resize(mmioAtEnd);
    t.mmio = std::move(log);
    t.stats = c.MultiBlockStatistics();
    t.runsAtEnd = runs;
    if (batched && s.expect) s.expect(s.name, t.stats, c);
    clockCpu = nullptr;
    return t;
}

// One batched run (cross-checked, or plain as in production) against one pass per block.
void CompareRuns(const Scenario& s, const Trace& off, const Trace& on, bool crossCheck) {
    const char* mode = crossCheck ? "cross-checked" : "plain";
    Check(off.states.size() == s.blocks && on.states.size() == s.blocks, s.name, "both runs reached the block count");
    size_t first = SIZE_MAX;
    for (size_t i = 0; i < off.states.size() && i < on.states.size(); ++i)
        if (off.states[i] != on.states[i]) { first = i; break; }
    if (first != SIZE_MAX) {
        const Trace a = RunScenario(s, false, false, first), b = RunScenario(s, true, crossCheck, first);
        std::string diff = a.captured && b.captured ? BlackFinCpu::DiffBlockState(*a.captured, *b.captured) : "";
        if (diff.empty()) diff = "(per-scenario extra state)";
        std::fprintf(stderr, "multiblock execution: %s (%s): first difference after block %zu: %s\n", s.name, mode, first,
                     diff.c_str());
        Check(false, s.name, "batched and unbatched state identical after every block");
    }
    Check(off.mmio == on.mmio, s.name, "identical MMIO sequence");
    Check(off.memory == on.memory, s.name, "identical guest memory");
    Check(on.stats.crossCheckFailures == 0, s.name, on.stats.firstCrossCheckFailure.empty()
          ? "no incremental proof disagreed with the full proof" : on.stats.firstCrossCheckFailure.c_str());
    Check(on.maxBlocksPerRun <= BlackFinCpu::kMaxBatchBlocks, s.name, "no Run() exceeds the batch budget");
}

void Compare(const Scenario& s) {
    const Trace off = RunScenario(s, false, false);
    const Trace on = RunScenario(s, true, true);
    const Trace plain = RunScenario(s, true, false);
    CompareRuns(s, off, on, true);
    CompareRuns(s, off, plain, false);
    Check(plain.stats.skippedPasses >= on.stats.skippedPasses, s.name, "cross-checking never skips more passes");
    std::printf("%-30s %-13s blocks %7llu  dispatches off %7llu on %6llu  skipped %6.1f%%  mmio %zu  max/run %llu\n", s.name, "(plain)",
                static_cast<unsigned long long>(s.blocks), static_cast<unsigned long long>(off.runsAtEnd),
                static_cast<unsigned long long>(plain.runsAtEnd),
                plain.stats.blocks ? 100.0 * static_cast<double>(plain.stats.skippedPasses) / static_cast<double>(plain.stats.blocks) : 0.0,
                plain.mmio.size(), static_cast<unsigned long long>(plain.maxBlocksPerRun));
    static const char* const names[BlackFinCpu::kBatchEndCount] = {"mmio", "irq-change", "irq-pending", "idle", "deadline",
        "pll", "device-work", "queued", "tracing", "budget", "stop-pc", "cross-check"};
    std::string ends;
    for (unsigned i = 0; i < BlackFinCpu::kBatchEndCount; ++i)
        if (plain.stats.ends[i]) ends += std::string(" ") + names[i] + "=" + std::to_string(plain.stats.ends[i]);
    std::printf("%-44s batch ends:%s\n", "", ends.c_str());
}

unsigned Bit(uint32_t barrier) { return static_cast<unsigned>(__builtin_ctz(barrier)); }
bool Ended(const BlackFinCpu::MultiBlockStats& s, uint32_t barrier) { return s.ends[Bit(barrier)] > 0; }
uint64_t SkippedPercent(const BlackFinCpu::MultiBlockStats& s) { return s.blocks ? s.skippedPasses * 100 / s.blocks : 0; }

void AckSportDma(TestCpu& t, uint64_t i) {
    if ((i & 2047) == 0) for (u32 ch : {0xFFC00CC0u, 0xFFC00D00u}) if (t.Rd(ch + 0x28) & 1u) t.HostWr(ch + 0x28, 1);
}

// SPORT0 TX on a two-descriptor list (firmware CONFIG 0x7489) and RX autobuffer.
void StartSport(TestCpu& c) {
    auto& m = c.GetEmulator();
    for (u32 at : {0xFF902000u, 0xFF902010u}) {
        const u32 next = at == 0xFF902000u ? 0xFF902010u : 0xFF902000u, start = at == 0xFF902000u ? 0xFF801000u : 0xFF801100u;
        m.MemoryWrite16(at, static_cast<u16>(next & 0xFFFF)); m.MemoryWrite16(at + 2, static_cast<u16>(next >> 16));
        m.MemoryWrite16(at + 4, static_cast<u16>(start & 0xFFFF)); m.MemoryWrite16(at + 6, static_cast<u16>(start >> 16));
    }
    for (u32 i = 0; i < 64; ++i) c.Wr(0xFF801000 + i * 4, 0xA5000000u | i);
    c.Wr(0xFFC0010C, 0xFFFFFFFF);
    c.Wr(0xFFC00804, 0x021F); c.Wr(0xFFC00824, 0x021F); c.Wr(0xFFC00800, 1); c.Wr(0xFFC00820, 1);
    c.Wr(0xFFC00D00, 0xFF902000); c.Wr(0xFFC00D10, 8); c.Wr(0xFFC00D14, 4); c.Wr(0xFFC00D08, 0x7489); // DMA4 TX
    c.Wr(0xFFC00CC4, 0xFF801200); c.Wr(0xFFC00CD0, 8); c.Wr(0xFFC00CD4, 4); c.Wr(0xFFC00CC8, 0x108B); // DMA3 RX
}

bool rtcMinuteSeen = false;

void JumpSelf(TestCpu& c) { c.GetEmulator().MemoryWrite16(0x200, JUMP_SELF); }

std::vector<Scenario> Scenarios() {
    std::vector<Scenario> list;
    list.push_back({"quiet loop", 5000, 40, JumpSelf, nullptr, nullptr,
        [](const char* n, const BlackFinCpu::MultiBlockStats& s, TestCpu&) {
            Check(SkippedPercent(s) >= 95 && Ended(s, BlackFinCpu::BarrierBudget), n, "quiet blocks batch up to the budget");
        }});
    list.push_back({"hardware loop with exit", 6000, 40,
        [](TestCpu& c) {
            // 0x200: lsetup(0x204, 0x206) lc0 = P1; body: R1 += 1; NOP; then jump back to 0x200.
            Put32(c, 0x200, LSETUP_NEXT2_LC0_P1);
            c.GetEmulator().MemoryWrite16(0x204, R1_ADD_1);
            c.GetEmulator().MemoryWrite16(0x206, NOP);
            c.GetEmulator().MemoryWrite16(0x208, JumpBack(8));
            c.State().dpregs[9] = 300; // P1: loop count
        }, nullptr, nullptr,
        [](const char* n, const BlackFinCpu::MultiBlockStats& s, TestCpu& c) {
            Check(SkippedPercent(s) >= 90 && c.State().dpregs[1] > 5000, n, "loop iterations batch and the loop re-arms");
        }});
    list.push_back({"memory stores in a loop", 6000, 40,
        [](TestCpu& c) {
            // lsetup over [P0++] = R0; R0 += 1; then P0 = P2 and repeat: 1 KiB of L1 rewritten per round.
            Put32(c, 0x200, LSETUP_NEXT2_LC0_P1);
            c.GetEmulator().MemoryWrite16(0x204, STORE_P0_INC_R0);
            c.GetEmulator().MemoryWrite16(0x206, R0_ADD_1);
            c.GetEmulator().MemoryWrite16(0x208, P0_EQ_P2);
            c.GetEmulator().MemoryWrite16(0x20A, JumpBack(10));
            c.State().dpregs[9] = 256;
            c.State().dpregs[10] = 0xFF800000; // P2
            c.State().dpregs[8] = 0xFF800000;  // P0
        }, nullptr,
        [](TestCpu& c) { return HashMemory(c, 0xFF800000, 0x400); },
        [](const char* n, const BlackFinCpu::MultiBlockStats& s, TestCpu&) {
            Check(SkippedPercent(s) >= 90, n, "plain memory stores do not end a batch");
        }});
    list.push_back({"MMIO every block", 1000, 40,
        [](TestCpu& c) {
            c.GetEmulator().MemoryWrite16(0x200, STORE_P0_R0);
            c.GetEmulator().MemoryWrite16(0x202, JumpBack(2));
            c.State().dpregs[8] = 0xFFC00110;       // P0 = SIC_IAR0
            c.State().dpregs[0] = c.Rd(0xFFC00110); // R0 = its value
        }, nullptr, nullptr,
        [](const char* n, const BlackFinCpu::MultiBlockStats& s, TestCpu&) {
            Check(s.skippedPasses == 0 && Ended(s, BlackFinCpu::BarrierMmio), n, "a block with MMIO always gets its pass");
        }});
    list.push_back({"MMIO between loops", 8000, 40,
        [](TestCpu& c) {
            // 0x200: [P0] = R0 (MMIO); 0x202: lsetup(0x206, 0x208) lc0 = P1; R1 += 1; NOP; jump 0x200.
            c.GetEmulator().MemoryWrite16(0x200, STORE_P0_R0);
            Put32(c, 0x202, LSETUP_NEXT2_LC0_P1);
            c.GetEmulator().MemoryWrite16(0x206, R1_ADD_1);
            c.GetEmulator().MemoryWrite16(0x208, NOP);
            c.GetEmulator().MemoryWrite16(0x20A, JumpBack(10));
            c.State().dpregs[8] = 0xFFC00110;
            c.State().dpregs[0] = c.Rd(0xFFC00110);
            c.State().dpregs[9] = 40;
        }, nullptr, nullptr,
        [](const char* n, const BlackFinCpu::MultiBlockStats& s, TestCpu&) {
            Check(s.skippedPasses > 0 && Ended(s, BlackFinCpu::BarrierMmio), n, "MMIO ends a batch, loops between batch");
        }});
    list.push_back({"core timer expiry and ISR", 8000, 40,
        [](TestCpu& c) {
            JumpSelf(c);
            c.GetEmulator().MemoryWrite16(0x500, NOP);
            c.GetEmulator().MemoryWrite16(0x502, RTI);
            evt_set(6, 0x500);
            cec_mmr_write(&c.State(), CEC_MMR_BASE + 4, (1u << 15) | (1u << 6));
            c.Wr(0xFFE03004, 700); // TPERIOD (CCLK)
            c.Wr(0xFFE03000, 7);   // TMPWR | TMREN | TAUTORLD
        }, nullptr, nullptr,
        [](const char* n, const BlackFinCpu::MultiBlockStats& s, TestCpu& c) {
            // An expiry dispatches IVG6 in the same block: the IPEND change is the first barrier.
            Check(Ended(s, BlackFinCpu::BarrierInterruptChange) && c.GuestTimer()->Expiries() >= 10 &&
                  SkippedPercent(s) >= 50, n, "expiries and ISRs end batches; blocks between expiries batch");
        }});
    list.push_back({"nested interrupts", 6000, 40,
        [](TestCpu& c) {
            auto& m = c.GetEmulator();
            JumpSelf(c);
            m.MemoryWrite16(0x300, NOP); m.MemoryWrite16(0x302, RTI);                    // IVG7
            m.MemoryWrite16(0x400, PUSH_RETI); m.MemoryWrite16(0x402, JUMP_NEXT);         // IVG8, nesting
            m.MemoryWrite16(0x404, JUMP_NEXT); m.MemoryWrite16(0x406, POP_RETI); m.MemoryWrite16(0x408, RTI);
            evt_set(7, 0x300); evt_set(8, 0x400);
            c.SetRegister(RegIndex::SP, 0xFF900F00);
            cec_mmr_write(&c.State(), CEC_MMR_BASE + 4, (1u << 15) | (1u << 8) | (1u << 7));
            c.Wr(0xFFC00110, 1u << 12);            // SIC_IAR0: pin 2 -> IVG7, pin 3 -> IVG8
            c.Wr(0xFFC0010C, (1u << 2) | (1u << 3));
        },
        [](TestCpu& t, uint64_t i) {
            if (i % 700 == 100) t.Pin(3, 1);                       // IVG8 request
            if (t.PC() == 0x400) { t.Pin(3, 0); t.Pin(2, 1); }    // IVG7 arrives inside IVG8
            if (t.PC() == 0x300) t.Pin(2, 0);
        }, nullptr,
        [](const char* n, const BlackFinCpu::MultiBlockStats& s, TestCpu&) {
            Check(Ended(s, BlackFinCpu::BarrierInterruptChange) && Ended(s, BlackFinCpu::BarrierQueuedEvents) &&
                  SkippedPercent(s) >= 80, n, "dispatch, RTI and latch requests end batches");
        }});
    list.push_back({"SPORT DMA words and rows", 150000, 40,
        [](TestCpu& c) { JumpSelf(c); StartSport(c); }, AckSportDma, nullptr,
        [](const char* n, const BlackFinCpu::MultiBlockStats& s, TestCpu& c) {
            Check(c.Sport(0).TransmitWords() >= 16 && c.Sport(0).ReceiveWords() >= 16, n, "SPORT moved words");
            Check(Ended(s, BlackFinCpu::BarrierDeviceWork) && SkippedPercent(s) >= 80, n, "due frames end batches");
        }});
    list.push_back({"core timer + SPORT deadlines", 80000, 40,
        [](TestCpu& c) { JumpSelf(c); StartSport(c); c.Wr(0xFFE03004, 9070); c.Wr(0xFFE03000, 7); }, AckSportDma, nullptr,
        [](const char* n, const BlackFinCpu::MultiBlockStats& s, TestCpu&) {
            Check(Ended(s, BlackFinCpu::BarrierGuestDeadline) && Ended(s, BlackFinCpu::BarrierDeviceWork), n,
                  "both deadline kinds end batches");
        }});
    list.push_back({"NAND/NFC completions", 3000, 40, JumpSelf,
        [](TestCpu& t, uint64_t i) {
            if (i == 500) t.Host([&t] { t.nand->busy = true; t.nand->pending = true; });
            if (i == 1500) t.Host([&t] { t.nand->busy = true; });
            if (i == 1502) t.Host([&t] { t.nand->busy = false; });
        }, nullptr,
        [](const char* n, const BlackFinCpu::MultiBlockStats& s, TestCpu&) {
            Check(Ended(s, BlackFinCpu::BarrierDeviceWork) && SkippedPercent(s) >= 80, n, "NFC work ends batches");
        }});
    list.push_back({"TWI master transfer", 3000, 40, JumpSelf,
        [](TestCpu& t, uint64_t i) {
            if (i == 500) { t.HostWr(0xFFC01404, 0x80); t.HostWr(0xFFC01414, 0x0041); }
            if (i == 1500) t.HostWr(0xFFC01404, 0x00);
        }, nullptr,
        [](const char* n, const BlackFinCpu::MultiBlockStats& s, TestCpu&) {
            Check(Ended(s, BlackFinCpu::BarrierDeviceWork), n, "an enabled TWI master ends batches");
        }});
    list.push_back({"GP timer host-time ticks", 20000, 400,
        [](TestCpu& c) {
            JumpSelf(c);
            c.Wr(0xFFC00600, 0x0009); c.Wr(0xFFC00608, 0x7FFFFFFF); c.Wr(0xFFC0060C, 1); c.Wr(0xFFC00680, 1);
        }, nullptr, nullptr,
        [](const char* n, const BlackFinCpu::MultiBlockStats& s, TestCpu& c) {
            Check(c.Rd(0xFFC00604) > 10 && Ended(s, BlackFinCpu::BarrierDeviceWork) && SkippedPercent(s) >= 50, n,
                  "due ticks end batches; the timer advances");
        }});
    list.push_back({"RTC second and minute rollover", 70000, 16000000,
        [](TestCpu& c) { JumpSelf(c); c.Wr(0xFFC00314, 1); c.Wr(0xFFC00304, 0x0C); }, // PREN; seconds + minutes events
        [](TestCpu& t, uint64_t i) {
            if (i == 0) rtcMinuteSeen = false;
            if (t.Rd(0xFFC00308) & 0x08) rtcMinuteSeen = true;
            if (i % 5000 == 4999) t.HostWr(0xFFC00308, 0x0C);
        }, nullptr,
        [](const char* n, const BlackFinCpu::MultiBlockStats& s, TestCpu&) {
            Check(Ended(s, BlackFinCpu::BarrierDeviceWork) && SkippedPercent(s) >= 80, n, "a new RTC second ends a batch");
            Check(rtcMinuteSeen, n, "the run crossed a host-time minute boundary");
        }});
    list.push_back({"USB host requests", 4500, 40,
        [](TestCpu& c) { JumpSelf(c); c.Wr(0xFFC03830, 0x0001 | (1u << 1) | (1u << 8)); },
        [](TestCpu& t, uint64_t i) {
            // Host (usbipd thread) requests arriving during quiet batches: a control SETUP that
            // the next pass delivers into EP0 (FIFO, CSR0, INTRTX), then a data write; the
            // guest services each later.
            if (i == 1000) { USBSetupBytes setup{}; setup.bRequest = 5; t.GetUSB().HandleSetupPacket(setup, nullptr, 0, [](const u8*, std::size_t) {}); }
            if (i == 1600) t.Host([&t] {
                for (int k = 0; k < 4; ++k) t.Rd(0xFFC03880);
                t.Wr(0xFFC03A04, 0x40); t.Wr(0xFFC03808, 1); t.Wr(0xFFC03A04, 0x08);
            });
            if (i == 2500) { const u8 data[2] = {1, 2}; t.GetUSB().HandleDataWrite(1, 0, data, sizeof(data), [] {}); }
            if (i == 3000) t.Host([&t] { t.Rd(0xFFC03888); t.Wr(0xFFC0384C + 0x200, 0); });
            // Guest queues an EP1 IN packet (FIFO + TXPKTRDY); the host's IN request arrives
            // later, during quiet batches, and the next pass moves the packet and completes it.
            if (i == 3200) t.Host([&t] { t.Wr(0xFFC03888, 0x11223344); t.Wr(0xFFC03A44, 1); });
            if (i == 3500) t.GetUSB().HandleDataRead(1, 0, 64, [](const u8*, std::size_t) {});
        }, nullptr,
        [](const char* n, const BlackFinCpu::MultiBlockStats& s, TestCpu&) {
            Check(Ended(s, BlackFinCpu::BarrierDeviceWork) && SkippedPercent(s) >= 50, n, "pending host work ends batches");
        }});
    list.push_back({"queued events (panel TE)", 6000, 40, JumpSelf,
        [](TestCpu& t, uint64_t i) {
            if (i % 2000 == 10) t.QueueEvent([] {}, std::chrono::nanoseconds(1000)); // like the TE high edge
        }, nullptr,
        [](const char* n, const BlackFinCpu::MultiBlockStats& s, TestCpu&) {
            Check(Ended(s, BlackFinCpu::BarrierQueuedEvents) && s.ends[Bit(BlackFinCpu::BarrierQueuedEvents)] >= 2000, n,
                  "a delayed event keeps every pass until it runs");
        }});
    list.push_back({"IDLE and core timer wake", 3000, 40,
        [](TestCpu& c) {
            c.GetEmulator().MemoryWrite16(0x200, NOP);
            c.GetEmulator().MemoryWrite16(0x202, IDLE);
            c.GetEmulator().MemoryWrite16(0x204, JumpBack(4));
            c.GetEmulator().MemoryWrite16(0x500, RTI);
            evt_set(6, 0x500);
            cec_mmr_write(&c.State(), CEC_MMR_BASE + 4, (1u << 15) | (1u << 6));
            c.Wr(0xFFE03004, 5000); c.Wr(0xFFE03000, 7);
        }, nullptr, nullptr, nullptr});
    list.push_back({"PLL same-value bypass countdown", 4000, 40,
        [](TestCpu& c) {
            // IDLE loop woken by the core timer; a same-value VR write before an IDLE starts an
            // awake bypass countdown (PLL_LOCKCNT CLKIN ticks at CCLK = CLKIN).
            c.GetEmulator().MemoryWrite16(0x200, NOP);
            c.GetEmulator().MemoryWrite16(0x202, IDLE);
            c.GetEmulator().MemoryWrite16(0x204, JumpBack(4));
            c.GetEmulator().MemoryWrite16(0x206, NOP);
            c.GetEmulator().MemoryWrite16(0x500, RTI);
            evt_set(6, 0x500);
            cec_mmr_write(&c.State(), CEC_MMR_BASE + 4, (1u << 15) | (1u << 6));
            c.Wr(0xFFE03004, 20000); c.Wr(0xFFE03000, 7);
        },
        [](TestCpu& t, uint64_t i) {
            if (i == 100) t.Host([&t] { t.GetEmulator().MemoryWrite16(0xFFC00008, 0x70B0); }); // same-value VR write
        }, nullptr,
        [](const char* n, const BlackFinCpu::MultiBlockStats& s, TestCpu&) {
            Check(Ended(s, BlackFinCpu::BarrierPllCountdown), n, "an outstanding PLL countdown ends every batch");
        }});
    return list;
}

// A PC the caller inspects between Run() calls must end every batch there.
void StopPC() {
    const char* name = "caller stop PC";
    ScopedEnv profile("OP1EMU_PROFILE", "functional");
    TestCpu c;
    clockCpu = &c; hostNsPerClkin = 40;
    c.SetHostClock(TestClock);
    Boot(c);
    c.GetEmulator().MemoryWrite16(0x200, NOP);
    c.GetEmulator().MemoryWrite16(0x202, JUMP_NEXT);
    c.GetEmulator().MemoryWrite16(0x204, JumpBack(4));
    c.Run(); c.Run();
    c.SetMultiBlock(true, true);
    c.AddBatchStopPC(0x204);
    uint64_t arrivals = 0, blocks = 0;
    bool atStop = false;
    c.SetBlockObserver([&] {
        Check(!atStop, name, "a batch never continues past the stop PC");
        ++blocks;
        if (c.PC() == 0x204) { atStop = true; ++arrivals; }
    });
    for (int i = 0; i < 2000; ++i) { c.Run(); atStop = false; }
    c.SetBlockObserver(nullptr);
    Check(arrivals >= 1000 && blocks > 2000, name, "the stop PC was reached repeatedly");
    clockCpu = nullptr;
    std::printf("%-30s %llu arrivals, all returned to the caller\n", name, static_cast<unsigned long long>(arrivals));
}

void ProfileGate() {
    const char* name = "profile gate";
    {
        ScopedEnv profile("OP1EMU_PROFILE", "strict");
        TestCpu c;
        bool threw = false;
        try { c.SetMultiBlock(true, false); } catch (const std::invalid_argument&) { threw = true; }
        Check(threw && !c.MultiBlock(), name, "strict profile refuses multi-block execution");
    }
    {
        ScopedEnv profile("OP1EMU_PROFILE", nullptr);
        TestCpu c;
        bool threw = false;
        try { c.SetMultiBlock(true, false); } catch (const std::invalid_argument&) { threw = true; }
        Check(threw && !c.MultiBlock(), name, "unset profile refuses multi-block execution");
    }
    {
        ScopedEnv profile("OP1EMU_PROFILE", "functional");
        TestCpu c;
        Check(!c.MultiBlock(), name, "off by default in the functional profile");
    }
    {
        ScopedEnv profile("OP1EMU_PROFILE", "functional");
        ScopedEnv multi("OP1EMU_EXPERIMENTAL_MULTIBLOCK", "1");
        TestCpu c;
        Check(c.MultiBlock(), name, "OP1EMU_EXPERIMENTAL_MULTIBLOCK=1 enables it");
    }
    {
        ScopedEnv profile("OP1EMU_PROFILE", "functional");
        ScopedEnv multi("OP1EMU_EXPERIMENTAL_MULTIBLOCK", "1");
        ScopedEnv diag("OP1EMU_BATCH_DIAG", "1");
        bool threw = false;
        try { TestCpu c; } catch (const std::invalid_argument&) { threw = true; }
        Check(threw, name, "multi-block execution and batch diagnostics are exclusive");
    }
    std::printf("%-30s ok\n", name);
}
}

int main(int argc, char** argv) {
    const char* only = argc > 1 ? argv[1] : nullptr;
    for (const auto& s : Scenarios())
        if (!only || std::strcmp(only, s.name) == 0) Compare(s);
    if (!only) { StopPC(); ProfileGate(); }
    if (failures) return 1;
    std::puts("multiblock execution tests passed");
    return 0;
}
