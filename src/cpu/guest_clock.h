#pragma once

#include <cstdint>
#include <optional>

// Deterministic guest time (docs/guest-clock-architecture.md, D1/D2).
//
// BF524 clock-tree ratios to CLKIN (HRM 18-3..18-5), expressed in a common
// sub-tick unit of 1/unitsPerClkin CLKIN tick so that one CCLK period and one
// SCLK period are both whole units. Ratios are register-determined; no
// absolute frequency is implied and the provisional 25 MHz CLKIN is not used.
struct ClockRatios {
    uint32_t unitsPerClkin = 1; // MSEL (64 for MSEL=0) with the PLL in use; 1 in bypass
    uint32_t cclkUnits = 1;     // (DF ? 2 : 1) * 2^CSEL; 1 in bypass
    uint32_t sclkUnits = 1;     // (DF ? 2 : 1) * max(SSEL, 2^CSEL); 1 in bypass
};

inline bool operator==(const ClockRatios& a, const ClockRatios& b) {
    return a.unitsPerClkin == b.unitsPerClkin && a.cclkUnits == b.cclkUnits && a.sclkUnits == b.sclkUnits;
}
inline bool operator!=(const ClockRatios& a, const ClockRatios& b) { return !(a == b); }

// How awake execution is charged in CCLK (D2). Both are estimates, not
// cycle-accurate timing.
enum class GuestCostModel {
    // ASSUMED minimum cost: one CCLK-equivalent per executed packet entry.
    // A deterministic lower-bound timeline; stalls and waits are omitted.
    MinCost,
    // Opt-in: the observational ShadowTiming estimate (OP1EMU_SHADOW_TIMING=1).
    ShadowEstimate,
};

const char* GuestCostModelName(GuestCostModel model);

// One CPU-owned guest clock. Canonical time is CLKIN ticks; CCLK and SCLK are
// derived domains with exact integer phase. Only the CPU loop advances it.
// Nothing here reads host time.
class GuestClock {
public:
    // Apply the PLL's current ratios; nullopt means unknown (no active PLL
    // state yet, HRM reset figures being inconsistent). A change that leaves
    // a sub-tick fraction aligns up to the next CLKIN (or SCLK) edge and is
    // counted in RatioAlignments(): ASSUMED clock-switch alignment.
    void SetRatios(const std::optional<ClockRatios>& ratios);
    const std::optional<ClockRatios>& Ratios() const { return ratios_; }

    // Awake execution charged at `cycles` CCLK. With unknown ratios CCLK
    // still counts, but CLKIN/SCLK cannot be derived and are not advanced.
    void AdvanceAwakeCclk(uint64_t cycles);
    // Architectural IDLE, CLKIN-domain time only (e.g. the existing PLL lock
    // countdown). CCLK is held: its plain-IDLE behavior is UNKNOWN (D6).
    // SCLK runs when the ratio is known (section 3).
    void AdvanceIdleClkin(uint64_t ticks);

    uint64_t ClkinTicks() const { return clkin_; }
    uint64_t CclkCycles() const { return cclk_; }
    uint64_t SclkCycles() const { return sclk_; }
    // CCLK charged while the CCLK:CLKIN ratio was unknown. While nonzero,
    // ClkinTicks() is a lower bound of elapsed guest time.
    uint64_t CclkWithoutRatio() const { return cclkWithoutRatio_; }
    uint64_t RatioAlignments() const { return alignments_; }

    // First CLKIN tick at or after which the CCLK count reaches `cclk`,
    // assuming continued awake execution at the current ratio. Rounds up.
    std::optional<uint64_t> ClkinTickForCclk(uint64_t cclk) const;

private:
    void AdvanceUnits(uint64_t units);

    std::optional<ClockRatios> ratios_;
    uint64_t clkin_ = 0;
    uint64_t subunits_ = 0;  // Fraction of the current CLKIN tick, < unitsPerClkin.
    uint64_t cclk_ = 0;
    uint64_t sclk_ = 0;
    uint64_t sclkPhase_ = 0; // Units into the current SCLK period, < sclkUnits.
    uint64_t cclkWithoutRatio_ = 0;
    uint64_t alignments_ = 0;
};
