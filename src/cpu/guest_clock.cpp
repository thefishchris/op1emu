#include "guest_clock.h"
#include <stdexcept>

namespace {
uint64_t CheckedAdd(uint64_t a, uint64_t b) {
    uint64_t sum = 0;
    if (__builtin_add_overflow(a, b, &sum)) throw std::overflow_error("guest clock overflow");
    return sum;
}

uint64_t CheckedMul(uint64_t a, uint64_t b) {
    uint64_t product = 0;
    if (__builtin_mul_overflow(a, b, &product)) throw std::overflow_error("guest clock overflow");
    return product;
}
}

const char* GuestCostModelName(GuestCostModel model) {
    return model == GuestCostModel::ShadowEstimate ? "shadow-estimate" : "min-cost";
}

void GuestClock::SetRatios(const std::optional<ClockRatios>& ratios) {
    if (ratios == ratios_) return;
    if (ratios_ && ratios) {
        if (ratios->unitsPerClkin != ratios_->unitsPerClkin && subunits_) {
            clkin_ = CheckedAdd(clkin_, 1);
            subunits_ = 0;
            ++alignments_;
        }
        if (ratios->sclkUnits != ratios_->sclkUnits && sclkPhase_) {
            sclkPhase_ = 0;
            ++alignments_;
        }
        if (ratios->cclkUnits != ratios_->cclkUnits && cclkPhase_) {
            cclkPhase_ = 0;
            ++alignments_;
        }
    } else if (subunits_ || sclkPhase_ || cclkPhase_) {
        // Leaving known ratios: the fraction cannot be carried into an unknown base.
        if (subunits_) clkin_ = CheckedAdd(clkin_, 1);
        subunits_ = 0;
        sclkPhase_ = 0;
        cclkPhase_ = 0;
        ++alignments_;
    }
    ratios_ = ratios;
}

void GuestClock::AdvanceUnits(uint64_t units) {
    const uint64_t sclkTotal = CheckedAdd(sclkPhase_, units);
    sclk_ = CheckedAdd(sclk_, sclkTotal / ratios_->sclkUnits);
    sclkPhase_ = sclkTotal % ratios_->sclkUnits;
    const uint64_t clkinTotal = CheckedAdd(subunits_, units);
    clkin_ = CheckedAdd(clkin_, clkinTotal / ratios_->unitsPerClkin);
    subunits_ = clkinTotal % ratios_->unitsPerClkin;
}

void GuestClock::AdvanceAwakeCclk(uint64_t cycles) {
    if (!cycles) return;
    cclk_ = CheckedAdd(cclk_, cycles);
    if (!ratios_) {
        cclkWithoutRatio_ = CheckedAdd(cclkWithoutRatio_, cycles);
        return;
    }
    AdvanceUnits(CheckedMul(cycles, ratios_->cclkUnits));
}

void GuestClock::AdvanceIdleClkin(uint64_t ticks) {
    if (!ticks) return;
    if (!ratios_) {
        clkin_ = CheckedAdd(clkin_, ticks);
        return;
    }
    // Whole CLKIN ticks keep the sub-tick phase; only SCLK accumulates.
    const uint64_t units = CheckedMul(ticks, ratios_->unitsPerClkin);
    const uint64_t sclkTotal = CheckedAdd(sclkPhase_, units);
    sclk_ = CheckedAdd(sclk_, sclkTotal / ratios_->sclkUnits);
    sclkPhase_ = sclkTotal % ratios_->sclkUnits;
    clkin_ = CheckedAdd(clkin_, ticks);
}

void GuestClock::AdvanceIdleUnits(uint64_t units, bool cclkRuns) {
    if (!units) return;
    if (!ratios_) throw std::logic_error("guest clock idle advance needs known ratios");
    if (cclkRuns) {
        const uint64_t total = CheckedAdd(cclkPhase_, units);
        cclk_ = CheckedAdd(cclk_, total / ratios_->cclkUnits);
        cclkPhase_ = total % ratios_->cclkUnits;
    }
    AdvanceUnits(units);
}

uint64_t GuestClock::UnitsToClkinEdges(uint64_t ticks) const {
    if (!ratios_) throw std::logic_error("guest clock conversion needs known ratios");
    if (!ticks) return 0;
    return CheckedMul(ticks, ratios_->unitsPerClkin) - subunits_;
}

uint64_t GuestClock::UnitsToCclk(uint64_t cclk) const {
    if (!ratios_) throw std::logic_error("guest clock conversion needs known ratios");
    if (cclk <= cclk_) return 0;
    return CheckedMul(cclk - cclk_, ratios_->cclkUnits) - cclkPhase_;
}

std::optional<uint64_t> GuestClock::ClkinTickForCclk(uint64_t cclk) const {
    if (!ratios_) return std::nullopt;
    if (cclk <= cclk_) return clkin_;
    const uint64_t units = CheckedAdd(subunits_, UnitsToCclk(cclk));
    const uint64_t whole = units / ratios_->unitsPerClkin;
    return CheckedAdd(clkin_, whole + (units % ratios_->unitsPerClkin ? 1 : 0));
}
