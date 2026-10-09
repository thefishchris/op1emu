#pragma once

#include "guest_clock.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Execution profiles (docs/emulator-profiles.md). The one place where timing
// assumptions are selected, validated and listed.
//
//  - Unset: the pre-profile behaviour. Each OP1EMU_* timing variable is read
//    exactly as before; nothing is enabled implicitly.
//  - strict: evidence-oriented. Rejects provisional clock assumptions
//    (OP1EMU_EXPERIMENTAL_CLKIN_HZ, OP1EMU_EXPERIMENTAL_PLL_AWAKE_BYPASS=1).
//    Observational shadow timing and the opt-in guest clock stay available.
//  - functional: enables the assumptions the known-good v241 boot already
//    uses (provisional CLKIN, shadow timing, PLL awake bypass) so one switch
//    replaces three, plus the guest clock with the ASSUMED plain-IDLE core
//    timer (the CCLK-core domain keeps running in plain architectural IDLE and
//    the core timer may wake it through IVG6). An explicitly set individual
//    variable overrides the profile default for that setting.
enum class EmulatorProfile { Unset, Strict, Functional };

const char* EmulatorProfileName(EmulatorProfile profile);

// Where a resolved setting came from.
enum class SettingSource { Default, Profile, Environment };

const char* SettingSourceName(SettingSource source);

struct TimingConfig {
    EmulatorProfile profile = EmulatorProfile::Unset;
    uint64_t clkinHz = 0; // 0: no CLKIN frequency assumed.
    SettingSource clkinSource = SettingSource::Default;
    bool shadowTiming = false;
    SettingSource shadowSource = SettingSource::Default;
    bool awakeBypassRequested = false;
    SettingSource awakeBypassSource = SettingSource::Default;
    // Requested and its prerequisites (CLKIN and shadow timing) present.
    bool awakeBypass = false;
    bool guestClock = false;
    SettingSource guestClockSource = SettingSource::Default;
    GuestCostModel costModel = GuestCostModel::MinCost;
    SettingSource costSource = SettingSource::Default;
    // Functional profile with the guest clock only: the CCLK-core domain
    // (core timer) advances during plain architectural IDLE. ASSUMED, not a
    // CONFIRMED BF524 behaviour; never set by strict or unset profiles.
    bool coreTimerRunsInIdle = false;
    // Every enabled timing assumption, with its evidence label. Empty when
    // nothing beyond the evidence-backed model is in effect.
    std::vector<std::string> assumptions;
};

using EnvironmentLookup = std::function<const char*(const char* name)>;

// Resolves OP1EMU_PROFILE and the individual timing variables. Throws
// std::invalid_argument for invalid values or for assumptions the selected
// profile forbids.
TimingConfig ResolveTimingConfig(const EnvironmentLookup& environment);

// Process environment lookup (std::getenv).
TimingConfig ResolveTimingConfigFromEnvironment();
