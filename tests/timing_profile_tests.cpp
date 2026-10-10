// Execution profiles (docs/emulator-profiles.md): resolution, precedence,
// rejection of forbidden assumptions, and the CPU-level effect on the
// same-value-VR awake PLL bypass that gates the v241 boot.
#include "cpu/cpu.h"
#include "cpu/nand.h"
#include "cpu/pll.h"
#include "cpu/timing_profile.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>

namespace {
void Check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "timing profile: %s\n", what);
        std::exit(1);
    }
}

using Env = std::map<std::string, std::string>;

TimingConfig Resolve(const Env& env) {
    return ResolveTimingConfig([&env](const char* name) -> const char* {
        const auto it = env.find(name);
        return it == env.end() ? nullptr : it->second.c_str();
    });
}

bool Rejects(const Env& env) {
    try { Resolve(env); } catch (const std::invalid_argument&) { return true; }
    return false;
}

void UnsetKeepsPreProfileBehaviour() {
    const auto none = Resolve({});
    Check(none.profile == EmulatorProfile::Unset && !none.clkinHz && !none.shadowTiming && !none.awakeBypass &&
          !none.guestClock && none.assumptions.empty(), "unset profile enables nothing");
    const auto legacy = Resolve({{"OP1EMU_EXPERIMENTAL_CLKIN_HZ", "25000000"}, {"OP1EMU_SHADOW_TIMING", "1"},
                                 {"OP1EMU_EXPERIMENTAL_PLL_AWAKE_BYPASS", "1"}});
    Check(legacy.profile == EmulatorProfile::Unset && legacy.clkinHz == 25000000 && legacy.shadowTiming &&
          legacy.awakeBypass && legacy.clkinSource == SettingSource::Environment &&
          legacy.awakeBypassSource == SettingSource::Environment && legacy.assumptions.size() == 3,
          "unset profile honours the three individual opt-ins exactly as before");
    const auto partial = Resolve({{"OP1EMU_EXPERIMENTAL_PLL_AWAKE_BYPASS", "1"}});
    Check(partial.awakeBypassRequested && !partial.awakeBypass, "bypass still needs CLKIN and shadow timing");
}

void StrictRejectsProvisionalAssumptions() {
    const auto strict = Resolve({{"OP1EMU_PROFILE", "strict"}});
    Check(strict.profile == EmulatorProfile::Strict && !strict.clkinHz && !strict.shadowTiming && !strict.awakeBypass &&
          !strict.guestClock && strict.assumptions.empty(), "strict selects the conservative settings");
    Check(Rejects({{"OP1EMU_PROFILE", "strict"}, {"OP1EMU_EXPERIMENTAL_CLKIN_HZ", "25000000"}}),
          "strict rejects the provisional CLKIN frequency");
    Check(Rejects({{"OP1EMU_PROFILE", "strict"}, {"OP1EMU_EXPERIMENTAL_PLL_AWAKE_BYPASS", "1"}}),
          "strict rejects the awake-bypass experiment");
    const auto allowed = Resolve({{"OP1EMU_PROFILE", "strict"}, {"OP1EMU_EXPERIMENTAL_PLL_AWAKE_BYPASS", "0"},
                                  {"OP1EMU_SHADOW_TIMING", "1"}, {"OP1EMU_EXPERIMENTAL_GUEST_CLOCK", "1"}});
    Check(allowed.shadowTiming && allowed.guestClock && !allowed.awakeBypass && !allowed.coreTimerRunsInIdle &&
          !allowed.sportFrameHz && allowed.assumptions.size() == 1, "strict keeps shadow timing and the opt-in guest clock, never the plain-IDLE core timer");
}

void FunctionalEnablesKnownGoodBootAssumptions() {
    const auto functional = Resolve({{"OP1EMU_PROFILE", "functional"}});
    Check(functional.profile == EmulatorProfile::Functional && functional.clkinHz == 25000000 &&
          functional.shadowTiming && functional.awakeBypass && functional.guestClock &&
          functional.coreTimerRunsInIdle && functional.guestClockSource == SettingSource::Profile &&
          functional.clkinSource == SettingSource::Profile && functional.shadowSource == SettingSource::Profile &&
          functional.awakeBypassSource == SettingSource::Profile && functional.sportFrameHz == 44100 &&
          functional.assumptions.size() == 6,
          "functional enables CLKIN, shadow timing, the awake bypass, the guest clock, the plain-IDLE core timer and "
          "the 44.1 kHz SPORT frame rate, and lists all six");
    Check(functional.assumptions[4].rfind("ASSUMED: CoreTimer continues during plain IDLE for functional profile", 0) == 0,
          "the plain-IDLE core timer assumption is labelled ASSUMED");
    Check(functional.assumptions[5].rfind("ASSUMED: external SPORT0 audio clock corresponds to 44.1 kHz native frame rate "
                                          "(LIKELY from firmware audio-engine evidence; codec MCLK inferred as 11.2896 MHz)", 0) == 0,
          "the SPORT frame rate assumption is labelled ASSUMED, with its LIKELY evidence");
    const auto noGuest = Resolve({{"OP1EMU_PROFILE", "functional"}, {"OP1EMU_EXPERIMENTAL_GUEST_CLOCK", "0"}});
    Check(!noGuest.guestClock && !noGuest.coreTimerRunsInIdle && !noGuest.sportFrameHz && noGuest.assumptions.size() == 3,
          "without the guest clock neither the plain-IDLE core timer nor the SPORT frame rate is applied");
    const auto unsetGuest = Resolve({{"OP1EMU_EXPERIMENTAL_CLKIN_HZ", "25000000"}, {"OP1EMU_EXPERIMENTAL_GUEST_CLOCK", "1"}});
    Check(unsetGuest.guestClock && unsetGuest.clkinHz && !unsetGuest.sportFrameHz,
          "the SPORT frame rate is a functional-profile assumption only");
    const auto noShadow = Resolve({{"OP1EMU_PROFILE", "functional"}, {"OP1EMU_SHADOW_TIMING", "0"}});
    Check(!noShadow.shadowTiming && noShadow.shadowSource == SettingSource::Environment &&
          noShadow.awakeBypassRequested && !noShadow.awakeBypass, "an explicit variable overrides the profile default");
    const auto noBypass = Resolve({{"OP1EMU_PROFILE", "functional"}, {"OP1EMU_EXPERIMENTAL_PLL_AWAKE_BYPASS", "0"}});
    Check(noBypass.clkinHz && noBypass.shadowTiming && !noBypass.awakeBypass && noBypass.assumptions.size() == 4,
          "the bypass can be switched off individually");
    const auto guest = Resolve({{"OP1EMU_PROFILE", "functional"}, {"OP1EMU_EXPERIMENTAL_GUEST_CLOCK", "1"},
                                {"OP1EMU_GUEST_CLOCK_COST", "shadow-estimate"}});
    Check(guest.guestClock && guest.guestClockSource == SettingSource::Environment &&
          guest.costModel == GuestCostModel::ShadowEstimate && guest.assumptions.size() == 6,
          "shadow-estimate satisfied by the profile's shadow timing");
}

void InvalidValuesRejected() {
    for (const char* bad : {"", "Functional", "lenient", "strict "})
        Check(Rejects({{"OP1EMU_PROFILE", bad}}), "invalid profile rejected");
    Check(Rejects({{"OP1EMU_SHADOW_TIMING", "2"}}), "invalid flag rejected");
    Check(Rejects({{"OP1EMU_PROFILE", "functional"}, {"OP1EMU_EXPERIMENTAL_CLKIN_HZ", "24000000"}}),
          "unsupported CLKIN rejected even in functional");
    Check(Rejects({{"OP1EMU_GUEST_CLOCK_COST", "shadow-estimate"}}), "shadow estimate still needs shadow timing");
    Check(Rejects({{"OP1EMU_GUEST_CLOCK_COST", "fast"}}), "unknown cost model rejected");
}

// Multi-block execution (docs/multiblock-execution.md): functional guest clock only, off by default.
void MultiBlockSwitch() {
    Check(!Resolve({{"OP1EMU_PROFILE", "functional"}}).multiBlock, "multi-block is off by default");
    const auto on = Resolve({{"OP1EMU_PROFILE", "functional"}, {"OP1EMU_EXPERIMENTAL_MULTIBLOCK", "1"}});
    Check(on.multiBlock && !on.multiBlockCrossCheck && on.assumptions.back().rfind("EXPERIMENTAL: multi-block", 0) == 0,
          "functional profile accepts multi-block and lists it");
    const auto cross = Resolve({{"OP1EMU_PROFILE", "functional"}, {"OP1EMU_EXPERIMENTAL_MULTIBLOCK", "crosscheck"}});
    Check(cross.multiBlock && cross.multiBlockCrossCheck, "crosscheck enables multi-block with full re-checks");
    Check(!Resolve({{"OP1EMU_PROFILE", "strict"}, {"OP1EMU_EXPERIMENTAL_MULTIBLOCK", "0"}}).multiBlock, "0 keeps it off");
    Check(Rejects({{"OP1EMU_PROFILE", "strict"}, {"OP1EMU_EXPERIMENTAL_MULTIBLOCK", "1"}}), "strict rejects multi-block");
    Check(Rejects({{"OP1EMU_EXPERIMENTAL_MULTIBLOCK", "1"}}), "unset profile rejects multi-block");
    Check(Rejects({{"OP1EMU_EXPERIMENTAL_GUEST_CLOCK", "1"}, {"OP1EMU_EXPERIMENTAL_MULTIBLOCK", "1"}}),
          "guest clock without the functional profile rejects multi-block");
    Check(Rejects({{"OP1EMU_PROFILE", "functional"}, {"OP1EMU_EXPERIMENTAL_GUEST_CLOCK", "0"},
                   {"OP1EMU_EXPERIMENTAL_MULTIBLOCK", "1"}}), "functional without the guest clock rejects multi-block");
    Check(Rejects({{"OP1EMU_PROFILE", "functional"}, {"OP1EMU_EXPERIMENTAL_MULTIBLOCK", "yes"}}), "invalid value rejected");
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

// Same-value VR write then IDLE: wakes immediately with a 512-tick bypass
// countdown outstanding, the condition that stops a plain v241 boot at EF000C3E.
void SameValueVR(BlackFinCpu& cpu) {
    cpu.AttachNandFlash(std::make_shared<IdleNand>());
    cpu.GetEmulator().MemoryWrite16(0x100, 0x0020); // IDLE
    cpu.GetEmulator().MemoryWrite16(0x102, 0x2000); // JUMP.S 0
    cpu.GetEmulator().MemoryWrite16(0xFFC00008, 0x70B0);
    cpu.SetPC(0x100);
    cpu.Run();
    cpu.Run();
}

void CpuDefaultUnchanged() {
    ScopedEnv profile("OP1EMU_PROFILE", nullptr);
    BlackFinCpu cpu;
    Check(cpu.Timing().profile == EmulatorProfile::Unset && cpu.ClockState().ProvisionalClkinHz() == 0 &&
          !cpu.ShadowTimingStats() && !cpu.GuestClockMode(), "default construction is unchanged");
    SameValueVR(cpu);
    Check(cpu.UnsupportedIdle() && std::strcmp(cpu.UnsupportedIdleReason(),
          "awake PLL bypass countdown needs a running CLKIN time source") == 0, "default still stops explicitly");
}

void CpuStrict() {
    ScopedEnv profile("OP1EMU_PROFILE", "strict");
    {
        BlackFinCpu cpu;
        SameValueVR(cpu);
        Check(cpu.Timing().profile == EmulatorProfile::Strict && cpu.UnsupportedIdle() &&
              cpu.ClockState().ClkinTicks() == 0, "strict stops at the bypass countdown instead of assuming a clock");
    }
    ScopedEnv clkin("OP1EMU_EXPERIMENTAL_CLKIN_HZ", "25000000");
    bool threw = false;
    try { BlackFinCpu cpu; } catch (const std::invalid_argument&) { threw = true; }
    Check(threw, "strict CPU construction refuses the provisional CLKIN");
}

void CpuFunctional() {
    ScopedEnv profile("OP1EMU_PROFILE", "functional");
    BlackFinCpu cpu;
    Check(cpu.Timing().profile == EmulatorProfile::Functional && cpu.ClockState().ProvisionalClkinHz() == 25000000 &&
          cpu.ShadowTimingStats() && cpu.Timing().awakeBypass && cpu.GuestClockMode() && cpu.Timing().coreTimerRunsInIdle,
          "functional configures the CPU with the known-good boot assumptions");
    SameValueVR(cpu);
    Check(!cpu.UnsupportedIdle() && cpu.AwakePLLExperimentStats().consumed > 0,
          "functional advances the bypass countdown from awake shadow deltas instead of stopping");
}
}

int main() {
    UnsetKeepsPreProfileBehaviour();
    StrictRejectsProvisionalAssumptions();
    FunctionalEnablesKnownGoodBootAssumptions();
    InvalidValuesRejected();
    MultiBlockSwitch();
    CpuDefaultUnchanged();
    CpuStrict();
    CpuFunctional();
    std::puts("timing profile tests passed");
    return 0;
}
