// Functional-profile SPORT pacing on the guest clock (docs/sport-frame-format.md,
// docs/emulator-profiles.md): exact frame/word arithmetic at the ASSUMED
// 44.1 kHz frame rate, no host time, SPORT DMA deadlines in architectural IDLE
// with SIC_IWR wake rules, and regressions proving strict/unset never pace
// SPORT on the guest clock.
#include "cpu/cpu.h"
#include "cpu/dma.h"
#include "cpu/guest_coretimer.h"
#include "cpu/nand.h"
#include "cpu/sport.h"
#include "cec.h"
#include "cpu_state.h"
#include "evt.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void Check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "sport guest clock: %s\n", what);
        std::exit(1);
    }
}

constexpr uint64_t kClkin = 25000000, kFrameHz = 44100;
constexpr u32 TCR1 = 0x00, TCR2 = 0x04, RCR1 = 0x20, RCR2 = 0x24;

// --- SPORT alone, with a test-owned guest CLKIN counter ---------------------

struct Rig {
    uint64_t now = 0;
    SPORT sport{0xFFC00800, 0};
    uint64_t callbackFrames = 0;
    int callbackChannels = 0, callbackBits = 0;
    explicit Rig(u32 config2) {
        sport.UseGuestClock({[this] { return now; }, kClkin, kFrameHz});
        sport.SetAudioOutputCallback([this](const void*, size_t frames, int channels, int bits) {
            callbackFrames += frames;
            callbackChannels = channels;
            callbackBits = bits;
        });
        sport.Write32(TCR2, config2);
        sport.Write32(TCR1, 1);
        sport.Write32(RCR2, config2);
        sport.Write32(RCR1, 1);
    }
    // Drains everything due, as the DMA would with 4096-byte requests.
    uint64_t DrainTx() {
        static u8 buffer[4096];
        uint64_t bytes = 0;
        while (const u32 moved = sport.DMAWrite(0, 0, buffer, sizeof(buffer))) bytes += moved;
        return bytes;
    }
    uint64_t DrainRx() {
        static u8 buffer[4096];
        uint64_t bytes = 0;
        while (const u32 moved = sport.DMARead(0, 0, buffer, sizeof(buffer))) bytes += moved;
        return bytes;
    }
};

void Arithmetic() {
    Check(SportFramesDue(kClkin, kClkin, kFrameHz) == 44100, "one guest second of CLKIN is 44,100 frames");
    Check(SportFramesDue(3600 * kClkin, kClkin, kFrameHz) == 3600 * 44100, "one guest hour is exact");
    Check(SportFramesDue(566, kClkin, kFrameHz) == 0 && SportFramesDue(567, kClkin, kFrameHz) == 1,
          "the first frame completes at CLKIN 567 (25e6 / 44100 = 566.89)");
    // 2^40 CLKIN ticks (~12.2 guest hours): no intermediate overflow.
    Check(SportFramesDue(uint64_t{1} << 40, kClkin, kFrameHz) == ((uint64_t{1} << 40) * 441) / 250000,
          "long elapsed times stay exact");
    for (uint64_t frames : {uint64_t{1}, uint64_t{2}, uint64_t{128}, uint64_t{44099}, uint64_t{44100}, uint64_t{44101},
                            uint64_t{987654321}}) {
        const uint64_t tick = SportClkinForFrames(frames, kClkin, kFrameHz);
        Check(SportFramesDue(tick, kClkin, kFrameHz) >= frames && SportFramesDue(tick - 1, kClkin, kFrameHz) < frames,
              "SportClkinForFrames is the first tick at which the frames are due");
    }
    Check(SportClkinForFrames(0, kClkin, kFrameHz) == 0, "zero frames are due at once");
}

void StereoRates() {
    Rig rig(0x021F); // v241: 32-bit words, stereo frame sync, primary only.
    Check(rig.sport.GuestPaced() && rig.sport.TransmitConfig2().Channels() == 2, "stereo decode unchanged");
    Check(rig.DrainTx() == 0 && rig.DrainRx() == 0, "the first DMA access starts the clock; nothing is due yet");
    rig.now = kClkin;
    Check(rig.DrainTx() == 88200 * 4 && rig.sport.TransmitWords() == 88200, "88,200 TX words per guest second");
    Check(rig.callbackFrames == 44100 && rig.callbackChannels == 2 && rig.callbackBits == 32,
          "44,100 stereo 32-bit frames reach the audio callback");
    Check(rig.DrainRx() == 88200 * 4 && rig.sport.ReceiveWords() == 88200, "88,200 RX words per guest second");
}

void MonoRates() {
    for (u32 config : {0x001Fu, 0x000Fu}) {
        Rig rig(config);
        rig.DrainTx();
        rig.DrainRx();
        rig.now = kClkin;
        const uint64_t wordBytes = config == 0x001F ? 4 : 2;
        Check(rig.DrainTx() == 44100 * wordBytes && rig.sport.TransmitWords() == 44100 && rig.callbackChannels == 1,
              "mono: one word per frame, 44,100 words per guest second");
        Check(rig.DrainRx() == 44100 * wordBytes, "mono RX: 44,100 words per guest second");
    }
}

void NoDrift() {
    Rig rig(0x021F);
    rig.DrainTx();
    // Irregular steps (prime and 1-tick) over ten guest seconds.
    uint64_t step = 0;
    while (rig.now < 10 * kClkin) {
        rig.now = std::min(10 * kClkin, rig.now + ((step++ % 3 == 0) ? 1 : 7919));
        rig.DrainTx();
        Check(rig.sport.TransmitWords() == SportFramesDue(rig.now, kClkin, kFrameHz) * 2,
              "words moved always equal frames due x 2");
    }
    Check(rig.sport.TransmitWords() == 882000 && rig.callbackFrames == 441000, "ten guest seconds, no drift");
}

void NoHostTime() {
    Rig rig(0x021F);
    rig.DrainTx();
    rig.now = 1000;
    const uint64_t first = rig.DrainTx();
    // Host time passes; guest time does not.
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(20)) {}
    Check(first == SportFramesDue(1000, kClkin, kFrameHz) * 2 * 4 && rig.DrainTx() == 0 && rig.DrainRx() == 0,
          "with guest time frozen nothing more is due, however much host time passes");
}

void DueTicks() {
    Rig rig(0x021F);
    rig.DrainTx();
    Check(rig.sport.TransmitStartClkin() == 0u, "start tick captured at the first DMA access");
    // A 256-word stereo row is 128 frames.
    const auto due = rig.sport.TransmitDueClkin(1024);
    Check(due == SportClkinForFrames(128, kClkin, kFrameHz), "row deadline is the first tick with 128 frames due");
    u8 buffer[1024] = {};
    rig.now = *due - 1;
    Check(rig.sport.DMAWrite(0, 0, buffer, sizeof(buffer)) == 127 * 8, "one tick early: 127 frames");
    rig.now = *due;
    Check(rig.sport.DMAWrite(0, 0, buffer, sizeof(buffer)) == 8, "on the deadline the row is complete");
    Check(rig.sport.TransmitDueClkin(4) == std::nullopt, "less than one frame is never a deadline");
    Check(rig.sport.TransmitDueClkin(8) == SportClkinForFrames(129, kClkin, kFrameHz), "next frame");
    rig.sport.Write32(TCR1, 0);
    Check(!rig.sport.TransmitDueClkin(8) && !rig.sport.TransmitStartClkin() && rig.sport.TransmitWords() == 0,
          "disable clears the transmit timeline");
    rig.sport.Write32(TCR1, 1);
    rig.now = 5000;
    rig.DrainTx();
    Check(rig.sport.TransmitStartClkin() == 5000u, "re-enable restarts at the next first DMA access");
}

void Validation() {
    SPORT sport(0xFFC00800, 0);
    Check(!sport.GuestPaced() && !sport.TransmitDueClkin(8) && !sport.TransmitStartClkin(),
          "without a guest clock the legacy host-time pacing is kept and no deadline is offered");
    bool threw = false;
    try { sport.UseGuestClock({nullptr, kClkin, kFrameHz}); } catch (const std::invalid_argument&) { threw = true; }
    Check(threw && !sport.GuestPaced(), "a guest clock without a CLKIN source is rejected");
    threw = false;
    try { sport.UseGuestClock({[] { return uint64_t{0}; }, kClkin, 0}); } catch (const std::invalid_argument&) { threw = true; }
    Check(threw, "a zero frame rate is rejected");
}

// --- Whole CPU: functional profile, SPORT DMA in architectural IDLE ----------

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
    u32 Read(u32 address) { return GetEmulator().MemoryRead32(address); }
    void Write(u32 address, u32 value) { GetEmulator().MemoryWrite32(address, value); }
};

constexpr u32 SPORT0 = 0xFFC00800, SIC_IMASK0 = 0xFFC0010C, SIC_ISR0 = 0xFFC00120, SIC_IWR0 = 0xFFC00124;
constexpr u32 DMA3 = 0xFFC00CC0, DMA4 = 0xFFC00D00; // SPORT0 RX (SIC IRQ 16), SPORT0 TX.
constexpr u32 START_ADDR = 0x04, CONFIG = 0x08, X_COUNT = 0x10, X_MODIFY = 0x14, IRQ_STATUS = 0x28, CURR_X_COUNT = 0x30;
constexpr u32 IRQ_DMA3_BIT = 1u << 16;
constexpr u16 IDLE = 0x0020, RTI = 0x0011, JUMP_BACK = 0x2FFF;
constexpr u32 RX_WORDS = 256, TX_WORDS = 200; // 128 and 100 stereo frames per row.

// PLL relock IDLE at 0x110, RTI into IVG15 at 0x200 where IDLE / JUMP 0x200
// loops. Core timer off. SPORT0 stereo 32-bit in both directions, fed by
// autobuffer DMA with data interrupts (only DMA3 has a bound SIC source).
void LoadProgram(TestCpu& cpu, u32 iwr0) {
    auto& memory = cpu.GetEmulator();
    memory.MemoryWrite16(0x110, IDLE);
    memory.MemoryWrite16(0x112, RTI);
    memory.MemoryWrite16(0x200, IDLE);
    memory.MemoryWrite16(0x202, JUMP_BACK);
    memory.MemoryWrite16(0xFFC00004, 5);
    memory.MemoryWrite16(0xFFC00000, 0x2000);
    cpu.State().reti = 0x200;
    evt_set(15, 0x200);
    cec_mmr_write(&cpu.State(), CEC_MMR_BASE + 4, 1u << 15); // IVG9 (DMA3) stays masked at the core.
    cec_raise(&cpu.State(), 15);
    cpu.Write(SIC_IMASK0, IRQ_DMA3_BIT);
    cpu.Write(SIC_IWR0, iwr0 | 1u); // DPMC (PLL lock) always wakes.
    cpu.Write(SPORT0 + TCR2, 0x021F);
    cpu.Write(SPORT0 + RCR2, 0x021F);
    cpu.Write(SPORT0 + TCR1, 1);
    cpu.Write(SPORT0 + RCR1, 1);
    cpu.Write(DMA3 + START_ADDR, 0xFF800000);
    cpu.Write(DMA3 + X_COUNT, RX_WORDS);
    cpu.Write(DMA3 + X_MODIFY, 4);
    cpu.Write(DMA3 + CONFIG, 0x108B); // Autobuffer, DI_EN, 32-bit, WNR, DMAEN.
    cpu.Write(DMA4 + START_ADDR, 0xFF801000);
    cpu.Write(DMA4 + X_COUNT, TX_WORDS);
    cpu.Write(DMA4 + X_MODIFY, 4);
    cpu.Write(DMA4 + CONFIG, 0x1089); // Autobuffer, DI_EN, 32-bit, DMAEN.
    cpu.SetPC(0x110);
}

// Runs to the IVG15 IDLE at 0x200 (after the PLL lock wake and RTI).
void RunToSteadyIdle(TestCpu& cpu) {
    cpu.Run();
    Check(!cpu.IsIdle() && cpu.PC() == 0x112 && cpu.GuestTime().ClkinTicks() == 512, "PLL lock is the first deadline");
    cpu.Run(); // RTI
    cpu.Run(); // IDLE at 0x200
    Check(cpu.IsIdle() && cpu.PC() == 0x202 && !cpu.UnsupportedIdle(), "IVG15 idles with SPORT DMA running");
}

uint64_t RowTick(uint64_t start, uint64_t rows, uint64_t framesPerRow) {
    return start + SportClkinForFrames(rows * framesPerRow, kClkin, kFrameHz);
}

void IdleFollowsSportDeadlinesWithoutWaking() {
    ScopedEnv profile("OP1EMU_PROFILE", "functional");
    ScopedEnv divisor("OP1EMU_AUDIO_PACING_DIVISOR", "7"); // Legacy host-time knob: must have no effect here.
    TestCpu cpu;
    Check(cpu.Timing().sportFrameHz == 44100 && cpu.Sport(0).GuestPaced() && cpu.Sport(1).GuestPaced(),
          "functional paces both SPORTs on the guest clock");
    LoadProgram(cpu, 0); // No DMA wake source: SPORT progress must not end IDLE.
    RunToSteadyIdle(cpu);
    const auto rxStart = cpu.Sport(0).ReceiveStartClkin(), txStart = cpu.Sport(0).TransmitStartClkin();
    Check(rxStart && txStart, "both directions started by their first DMA access");
    const uint64_t packets = cpu.PacketEntryCount();
    uint64_t rxRows = 0, txRows = 0, rxCompletions = 0;
    // The Run() that entered IDLE already took the first deadline; each later
    // Run() takes exactly one more, in time order.
    const auto account = [&] {
        const uint64_t nextRx = RowTick(*rxStart, rxRows + 1, RX_WORDS / 2);
        const uint64_t nextTx = RowTick(*txStart, txRows + 1, TX_WORDS / 2);
        const uint64_t now = cpu.GuestTime().ClkinTicks();
        Check(now == std::min(nextRx, nextTx), "each step lands exactly on the earliest SPORT row deadline");
        if (now == nextRx) {
            ++rxRows;
            Check((cpu.Read(DMA3 + IRQ_STATUS) & 1u) && (cpu.Read(SIC_ISR0) & IRQ_DMA3_BIT) &&
                  cpu.Read(DMA3 + CURR_X_COUNT) == RX_WORDS, "RX row complete: DMA_DONE and SIC ISR latch, autobuffer reloads");
            cpu.Write(DMA3 + IRQ_STATUS, 1); // Clear as a handler would (outside guest code).
            ++rxCompletions;
        }
        if (now == nextTx) ++txRows;
        Check(cpu.Sport(0).ReceiveWords() == SportFramesDue(now - *rxStart, kClkin, kFrameHz) * 2 &&
              cpu.Sport(0).TransmitWords() == SportFramesDue(now - *txStart, kClkin, kFrameHz) * 2,
              "every word due at the deadline has moved, and no more");
        Check(cpu.IsIdle() && !cpu.UnsupportedIdle() && cpu.PacketEntryCount() == packets,
              "SPORT progress alone never wakes; no packets execute while idle");
    };
    account();
    // Ten guest seconds of TX (441,000 frames = 4,410 rows of 100) in IDLE.
    while (txRows < 4410) {
        cpu.Run();
        account();
    }
    // 10 s = 441,000 frames: 4,410 TX rows of 100 frames and 3,445 whole RX rows of 128 frames.
    Check(cpu.GuestTime().ClkinTicks() == *txStart + 10 * kClkin && rxRows == 3445 && rxCompletions == 3445,
          "completion cadence over exactly ten guest seconds");
}

void IwrDmaInterruptWakesAtItsDeadline() {
    ScopedEnv profile("OP1EMU_PROFILE", "functional");
    TestCpu cpu;
    LoadProgram(cpu, IRQ_DMA3_BIT);
    RunToSteadyIdle(cpu);
    const uint64_t rxStart = *cpu.Sport(0).ReceiveStartClkin();
    const uint64_t txStart = *cpu.Sport(0).TransmitStartClkin();
    for (uint64_t row = 1; row <= 3; ++row) {
        const uint64_t rxTick = RowTick(rxStart, row, RX_WORDS / 2);
        // TX rows (no bound interrupt) may complete first: they do not wake.
        while (cpu.IsIdle()) {
            cpu.Run();
            Check(cpu.GuestTime().ClkinTicks() <= rxTick, "never past the RX completion");
            if (cpu.GuestTime().ClkinTicks() < rxTick)
                Check(cpu.IsIdle(), "a TX row end without an IWR source keeps the core idle");
        }
        Check(cpu.GuestTime().ClkinTicks() == rxTick && cpu.PC() == 0x202 && !cpu.UnsupportedIdle(),
              "the IWR-enabled DMA3 interrupt wakes exactly at the RX row deadline");
        Check(cpu.Read(SIC_ISR0) & IRQ_DMA3_BIT, "the wake source is the DMA3 SIC interrupt");
        cpu.Run(); // JUMP back to 0x200; the existing SIC -> CEC routing delivers the latch by now.
        Check(cec_mmr_read(&cpu.State(), CEC_MMR_BASE + 12) & (1u << 9), "IVG9 latched through the SIC (IAR) route");
        cpu.Write(DMA3 + IRQ_STATUS, 1);
        cec_mmr_write(&cpu.State(), CEC_MMR_BASE + 12, 1u << 9); // Clear ILAT as the masked IVG9 never runs.
        cpu.Run(); // IDLE again
        Check(cpu.IsIdle() && !(cpu.Read(SIC_ISR0) & IRQ_DMA3_BIT), "back in IDLE after clearing the interrupt");
        Check(txStart <= rxStart + 1, "both directions started together");
    }
}

void CoreTimerAndSportShareTheTimeline() {
    ScopedEnv profile("OP1EMU_PROFILE", "functional");
    TestCpu cpu;
    LoadProgram(cpu, 0);
    RunToSteadyIdle(cpu);
    // Core timer with a period between SPORT rows; IVG6 masked, so it latches only.
    cpu.Write(0xFFE03004, 500000); // TPERIOD in CCLK (16 CCLK per CLKIN: 31,250 CLKIN).
    cpu.Write(0xFFE03000, 7);
    std::vector<uint64_t> visited;
    while (cpu.GuestTime().ClkinTicks() < 300000) {
        cpu.Run();
        visited.push_back(cpu.GuestTime().ClkinTicks());
    }
    Check(std::is_sorted(visited.begin(), visited.end()) &&
          std::adjacent_find(visited.begin(), visited.end()) == visited.end(),
          "core timer and SPORT deadlines interleave strictly in time order");
    // Armed at CCLK 899,361 (the first TX row, CLKIN 56,690); the loop stops at
    // the first deadline at or after CLKIN 300,000, the eighth expiry.
    Check(cpu.GuestTimer()->Expiries() == 8 && cpu.GuestTime().CclkCycles() == 899361 + 8 * 500000 && cpu.IsIdle(),
          "core timer expiries still taken, exactly, during SPORT IDLE");
    const auto rxStart = *cpu.Sport(0).ReceiveStartClkin();
    Check(cpu.Sport(0).ReceiveWords() == SportFramesDue(cpu.GuestTime().ClkinTicks() - rxStart, kClkin, kFrameHz) * 2,
          "SPORT words due at a core-timer timestamp are serviced before the wake decision");
}

void StrictAndUnsetNeverPaceOnGuestClock() {
    {
        ScopedEnv profile("OP1EMU_PROFILE", "strict");
        ScopedEnv guest("OP1EMU_EXPERIMENTAL_GUEST_CLOCK", "1");
        TestCpu cpu;
        Check(cpu.GuestClockMode() && !cpu.Timing().sportFrameHz && !cpu.Sport(0).GuestPaced() &&
              !cpu.Sport(1).GuestPaced(), "strict guest clock keeps SPORT off the ASSUMED frame rate");
    }
    {
        ScopedEnv profile("OP1EMU_PROFILE", nullptr);
        ScopedEnv clkin("OP1EMU_EXPERIMENTAL_CLKIN_HZ", "25000000");
        ScopedEnv guest("OP1EMU_EXPERIMENTAL_GUEST_CLOCK", "1");
        TestCpu cpu;
        Check(!cpu.Timing().sportFrameHz && !cpu.Sport(0).GuestPaced(), "unset profile keeps legacy SPORT pacing");
    }
    {
        ScopedEnv profile("OP1EMU_PROFILE", "functional");
        ScopedEnv guest("OP1EMU_EXPERIMENTAL_GUEST_CLOCK", "0");
        TestCpu cpu;
        Check(!cpu.Timing().sportFrameHz && !cpu.Sport(0).GuestPaced(), "functional without the guest clock keeps legacy pacing");
    }
}
}

int main() {
    Arithmetic();
    StereoRates();
    MonoRates();
    NoDrift();
    NoHostTime();
    DueTicks();
    Validation();
    IdleFollowsSportDeadlinesWithoutWaking();
    IwrDmaInterruptWakesAtItsDeadline();
    CoreTimerAndSportShareTheTimeline();
    StrictAndUnsetNeverPaceOnGuestClock();
    std::puts("sport guest clock tests passed");
    return 0;
}
