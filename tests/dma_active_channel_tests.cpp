// DMA active-channel tracking (DMA::ProcessWithInterrupt skips channels that
// are not enabled and running). The same deterministic workload is run with
// and without tracking; every DMA channel register, SIC/CEC state, SPORT word
// count and destination buffer must match after every block.
#include "cpu/cpu.h"
#include "cpu/dma.h"
#include "cpu/nand.h"
#include "cpu/sport.h"
#include "cec.h"
#include "cpu_state.h"
#include "evt.h"
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {
void Check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "dma active channels: %s\n", what);
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
    DMA& Dma() { return *dma; }
    u32 Rd(u32 a) { return GetEmulator().MemoryRead32(a); }
    void Wr(u32 a, u32 v) { GetEmulator().MemoryWrite32(a, v); }
};

constexpr u32 DMA_BASE = 0xFFC00C00, SPORT0 = 0xFFC00800, SIC_IMASK0 = 0xFFC0010C, SIC_ISR0 = 0xFFC00120;
constexpr u32 NEXT = 0x00, START = 0x04, CONFIG = 0x08, XCOUNT = 0x10, XMOD = 0x14, YCOUNT = 0x18, YMOD = 0x1C, IRQSTAT = 0x28;
constexpr u32 TX_BUF0 = 0xFF801000, TX_BUF1 = 0xFF801100, RX_BUF = 0xFF801200, DESC0 = 0xFF902000, DESC1 = 0xFF902010;
constexpr u32 MDMA0_SRC = 0xFF800000, MDMA0_DST = 0xFF800400, MDMA1_SRC = 0xFF800800, MDMA1_DST = 0xFF800C00;
u32 Ch(int n, u32 reg) { return DMA_BASE + static_cast<u32>(n) * 0x40 + reg; }

void WriteDescriptor(TestCpu& c, u32 at, u32 next, u32 start) {
    auto& m = c.GetEmulator();
    m.MemoryWrite16(at, static_cast<u16>(next & 0xFFFF));
    m.MemoryWrite16(at + 2, static_cast<u16>(next >> 16));
    m.MemoryWrite16(at + 4, static_cast<u16>(start & 0xFFFF));
    m.MemoryWrite16(at + 6, static_cast<u16>(start >> 16));
}

uint64_t Hash(uint64_t h, u32 v) { return (h ^ v) * 1099511628211ull; }

// One deterministic run; returns a hash of the observable state after every block.
std::vector<uint64_t> Workload(bool tracking, uint64_t& transfersSeen) {
    ScopedEnv profile("OP1EMU_PROFILE", "functional");
    TestCpu c;
    c.Dma().SetActiveChannelTracking(tracking);
    auto& m = c.GetEmulator();
    // PLL relock IDLE, then an awake IVG15 loop (as in the SPORT guest-clock tests).
    m.MemoryWrite16(0x110, 0x0020);
    m.MemoryWrite16(0x112, 0x0011);
    m.MemoryWrite16(0x200, 0x2000);
    m.MemoryWrite16(0xFFC00004, 5);
    m.MemoryWrite16(0xFFC00000, 0x2000);
    c.State().reti = 0x200;
    evt_set(15, 0x200);
    cec_mmr_write(&c.State(), CEC_MMR_BASE + 4, 1u << 15);
    cec_raise(&c.State(), 15);
    c.Wr(SIC_IMASK0, 0xFFFFFFFF);
    for (u32 i = 0; i < 64; ++i) {
        c.Wr(MDMA0_SRC + i * 4, 0x01010101u * (i + 1));
        c.Wr(MDMA1_SRC + i * 4, 0x10203040u ^ i);
        c.Wr(TX_BUF0 + i * 4, 0xA0000000u | i);
    }
    // SPORT0 stereo 32-bit both directions.
    c.Wr(SPORT0 + 0x04, 0x021F);
    c.Wr(SPORT0 + 0x24, 0x021F);
    c.Wr(SPORT0 + 0x00, 1);
    c.Wr(SPORT0 + 0x20, 1);
    // SPORT0 TX on DMA4: two-descriptor large-model list, firmware CONFIG 0x7489.
    WriteDescriptor(c, DESC0, DESC1, TX_BUF0);
    WriteDescriptor(c, DESC1, DESC0, TX_BUF1);
    c.Wr(Ch(4, NEXT), DESC0);
    c.Wr(Ch(4, XCOUNT), 8);
    c.Wr(Ch(4, XMOD), 4);
    c.Wr(Ch(4, CONFIG), 0x7489);
    // SPORT0 RX on DMA3: autobuffer with data interrupts.
    c.Wr(Ch(3, START), RX_BUF);
    c.Wr(Ch(3, XCOUNT), 8);
    c.Wr(Ch(3, XMOD), 4);
    c.Wr(Ch(3, CONFIG), 0x108B);
    c.SetPC(0x110);

    std::vector<uint64_t> trace;
    for (uint64_t run = 0; run < 400000; ++run) {
        switch (run) {
        case 1000: // MDMA0 1D, 16-bit, destination first.
            c.Wr(Ch(12, START), MDMA0_DST); c.Wr(Ch(12, XCOUNT), 64); c.Wr(Ch(12, XMOD), 2); c.Wr(Ch(12, CONFIG), 0x0087);
            c.Wr(Ch(13, START), MDMA0_SRC); c.Wr(Ch(13, XCOUNT), 64); c.Wr(Ch(13, XMOD), 2); c.Wr(Ch(13, CONFIG), 0x0005);
            break;
        case 2000: // MDMA1 2D 8x4, 16-bit, contiguous rows.
            c.Wr(Ch(14, START), MDMA1_DST); c.Wr(Ch(14, XCOUNT), 8); c.Wr(Ch(14, XMOD), 2); c.Wr(Ch(14, YCOUNT), 4); c.Wr(Ch(14, YMOD), 2); c.Wr(Ch(14, CONFIG), 0x0097);
            c.Wr(Ch(15, START), MDMA1_SRC); c.Wr(Ch(15, XCOUNT), 8); c.Wr(Ch(15, XMOD), 2); c.Wr(Ch(15, YCOUNT), 4); c.Wr(Ch(15, YMOD), 2); c.Wr(Ch(15, CONFIG), 0x0015);
            break;
        case 150000: c.Wr(Ch(4, CONFIG), 0x7488); break; // SPORT TX DMA disabled...
        case 170000: c.Wr(Ch(4, NEXT), DESC1); c.Wr(Ch(4, CONFIG), 0x7489); break; // ...and re-enabled.
        case 200000: // MDMA0 again, now 32-bit.
            c.Wr(Ch(12, IRQSTAT), 1); c.Wr(Ch(12, START), MDMA0_DST + 0x100); c.Wr(Ch(12, XCOUNT), 16); c.Wr(Ch(12, XMOD), 4); c.Wr(Ch(12, CONFIG), 0x008B);
            c.Wr(Ch(13, START), MDMA0_SRC); c.Wr(Ch(13, XCOUNT), 16); c.Wr(Ch(13, XMOD), 4); c.Wr(Ch(13, CONFIG), 0x0009);
            break;
        default: break;
        }
        // Acknowledge completions as a handler would (shared by both runs).
        if ((run & 4095) == 0)
            for (int ch : {3, 4, 12, 14}) if (c.Rd(Ch(ch, IRQSTAT)) & 1u) c.Wr(Ch(ch, IRQSTAT), 1);
        c.Run();
        Check(!c.UnsupportedIdle() && !c.Failed(), "workload keeps running");
        uint64_t h = 1469598103934665603ull;
        for (int ch = 0; ch < 16; ++ch)
            for (u32 reg : {NEXT, START, CONFIG, XCOUNT, XMOD, YCOUNT, YMOD, 0x20u, 0x24u, IRQSTAT, 0x2Cu, 0x30u, 0x38u})
                h = Hash(h, c.Rd(Ch(ch, reg)));
        h = Hash(h, c.Rd(SIC_ISR0));
        h = Hash(h, cec_mmr_read(&c.State(), CEC_MMR_BASE + 8));
        h = Hash(h, cec_mmr_read(&c.State(), CEC_MMR_BASE + 12));
        h = Hash(h, static_cast<u32>(c.Sport(0).TransmitWords()));
        h = Hash(h, static_cast<u32>(c.Sport(0).ReceiveWords()));
        h = Hash(h, c.PC());
        trace.push_back(h);
    }
    // Final memory: MDMA destinations and the RX buffer.
    uint64_t h = 1469598103934665603ull;
    for (u32 i = 0; i < 64; ++i) {
        h = Hash(h, c.Rd(MDMA0_DST + i * 4));
        h = Hash(h, c.Rd(MDMA1_DST + i * 4));
        h = Hash(h, c.Rd(RX_BUF + i * 4));
    }
    trace.push_back(h);
    for (u32 i = 0; i < 32; ++i) Check(c.Rd(MDMA0_DST + i * 4) == 0x01010101u * (i + 1), "MDMA0 copied 64 halfwords");
    for (u32 i = 0; i < 16; ++i) Check(c.Rd(MDMA0_DST + 0x100 + i * 4) == 0x01010101u * (i + 1), "MDMA0 re-run copied 16 words");
    for (u32 i = 0; i < 16; ++i) Check(c.Rd(MDMA1_DST + i * 4) == (0x10203040u ^ i), "MDMA1 2D copied its buffer");
    transfersSeen = c.Sport(0).TransmitWords() + c.Sport(0).ReceiveWords();
    if (tracking) Check(c.Dma().ActiveChannels() == ((1u << 3) | (1u << 4)), "only the two SPORT channels stay active");
    return trace;
}
}

int main() {
    uint64_t wordsOff = 0, wordsOn = 0;
    const auto off = Workload(false, wordsOff);
    const auto on = Workload(true, wordsOn);
    Check(off.size() == on.size(), "same number of blocks");
    for (size_t i = 0; i < off.size(); ++i)
        if (off[i] != on[i]) {
            std::fprintf(stderr, "first difference after block %zu\n", i);
            Check(false, "observable DMA/SIC/CEC/SPORT state identical with and without tracking");
        }
    Check(wordsOff == wordsOn && wordsOn > 0, "SPORT moved the same words");
    std::printf("dma active channel tests passed (%zu blocks compared, %llu SPORT words)\n", off.size(),
                static_cast<unsigned long long>(wordsOn));
    return 0;
}
