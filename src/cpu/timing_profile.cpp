#include "timing_profile.h"
#include <cstdlib>
#include <cstring>
#include <optional>
#include <stdexcept>

namespace {
constexpr uint64_t kProvisionalClkinHz = 25000000;

std::optional<bool> Flag(const EnvironmentLookup& environment, const char* name) {
    const char* value = environment(name);
    if (!value) return std::nullopt;
    if (std::strcmp(value, "1") != 0 && std::strcmp(value, "0") != 0)
        throw std::invalid_argument(std::string(name) + " must be 0 or 1");
    return std::strcmp(value, "1") == 0;
}
}

const char* EmulatorProfileName(EmulatorProfile profile) {
    switch (profile) {
    case EmulatorProfile::Strict: return "strict";
    case EmulatorProfile::Functional: return "functional";
    default: return "unset";
    }
}

const char* SettingSourceName(SettingSource source) {
    switch (source) {
    case SettingSource::Profile: return "profile";
    case SettingSource::Environment: return "environment";
    default: return "default";
    }
}

TimingConfig ResolveTimingConfig(const EnvironmentLookup& environment) {
    TimingConfig config;
    if (const char* value = environment("OP1EMU_PROFILE")) {
        if (std::strcmp(value, "strict") == 0) config.profile = EmulatorProfile::Strict;
        else if (std::strcmp(value, "functional") == 0) config.profile = EmulatorProfile::Functional;
        else throw std::invalid_argument("OP1EMU_PROFILE must be strict or functional");
    }
    const bool strict = config.profile == EmulatorProfile::Strict;
    const bool functional = config.profile == EmulatorProfile::Functional;

    if (const char* value = environment("OP1EMU_EXPERIMENTAL_CLKIN_HZ")) {
        if (strict)
            throw std::invalid_argument("strict profile rejects OP1EMU_EXPERIMENTAL_CLKIN_HZ: CLKIN frequency is PROVISIONAL, not CONFIRMED");
        // Explicit opt-in only. This is the LIKELY photo-derived experiment,
        // not an unconditional original-OP-1 oscillator specification.
        if (std::strcmp(value, "25000000") != 0)
            throw std::invalid_argument("OP1EMU_EXPERIMENTAL_CLKIN_HZ currently supports only provisional 25000000");
        config.clkinHz = kProvisionalClkinHz;
        config.clkinSource = SettingSource::Environment;
    } else if (functional) {
        config.clkinHz = kProvisionalClkinHz;
        config.clkinSource = SettingSource::Profile;
    }

    if (const auto shadow = Flag(environment, "OP1EMU_SHADOW_TIMING")) {
        config.shadowTiming = *shadow;
        config.shadowSource = SettingSource::Environment;
    } else if (functional) {
        config.shadowTiming = true;
        config.shadowSource = SettingSource::Profile;
    }

    if (const auto bypass = Flag(environment, "OP1EMU_EXPERIMENTAL_PLL_AWAKE_BYPASS")) {
        if (strict && *bypass)
            throw std::invalid_argument("strict profile rejects OP1EMU_EXPERIMENTAL_PLL_AWAKE_BYPASS=1: EXPERIMENTAL ideal/no-stall assumption");
        config.awakeBypassRequested = *bypass;
        config.awakeBypassSource = SettingSource::Environment;
    } else if (functional) {
        config.awakeBypassRequested = true;
        config.awakeBypassSource = SettingSource::Profile;
    }
    config.awakeBypass = config.awakeBypassRequested && config.clkinHz && config.shadowTiming;

    if (const auto guest = Flag(environment, "OP1EMU_EXPERIMENTAL_GUEST_CLOCK")) {
        config.guestClock = *guest;
        config.guestClockSource = SettingSource::Environment;
    }
    if (const char* value = environment("OP1EMU_GUEST_CLOCK_COST")) {
        if (std::strcmp(value, "shadow-estimate") == 0) config.costModel = GuestCostModel::ShadowEstimate;
        else if (std::strcmp(value, "min-cost") != 0)
            throw std::invalid_argument("OP1EMU_GUEST_CLOCK_COST must be min-cost or shadow-estimate");
        config.costSource = SettingSource::Environment;
        if (config.costModel == GuestCostModel::ShadowEstimate && !config.shadowTiming)
            throw std::invalid_argument("OP1EMU_GUEST_CLOCK_COST=shadow-estimate requires OP1EMU_SHADOW_TIMING=1");
    }

    if (config.clkinHz)
        config.assumptions.push_back("CLKIN=25000000 Hz: PROVISIONAL/LIKELY (board-photo inference), not CONFIRMED");
    if (config.awakeBypass)
        config.assumptions.push_back("PLL awake bypass: CCLK=CLKIN countdown advanced by the shadow ideal/no-stall "
                                     "issue estimate while awake: EXPERIMENTAL");
    if (config.shadowTiming && config.awakeBypass)
        config.assumptions.push_back("shadow timing: observational issue model, used here as the awake-bypass cycle source");
    if (config.guestClock)
        config.assumptions.push_back(std::string("guest clock cost model ") + GuestCostModelName(config.costModel) +
                                     ": ASSUMED, not cycle accurate");
    return config;
}

TimingConfig ResolveTimingConfigFromEnvironment() {
    return ResolveTimingConfig([](const char* name) { return static_cast<const char*>(std::getenv(name)); });
}
