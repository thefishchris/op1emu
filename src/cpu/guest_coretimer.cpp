#include "guest_coretimer.h"
#include "guest_clock.h"
#include "utils/log.h"

namespace {
constexpr u32 TMPWR = 1u << 0;
constexpr u32 TMREN = 1u << 1;
constexpr u32 TAUTORLD = 1u << 2;
constexpr u32 TINT = 1u << 3;
constexpr unsigned kMaxWarnings = 16;
}

GuestCoreTimer::GuestCoreTimer(u32 baseAddr, const GuestClock& clock)
    : RegisterDevice("CoreTimer", baseAddr, 0x10), clock_(clock) {
    REG32(TCNTL, 0x00);
    TCNTL.readCallback = [this]() -> u32 {
        return (power_ ? TMPWR : 0u) | (enabled_ ? TMREN : 0u) | (autoReload_ ? TAUTORLD : 0u) | (tint_ ? TINT : 0u);
    };
    TCNTL.writeCallback = [this](u32 v) { WriteControl(v); };

    REG32(TPERIOD, 0x04);
    TPERIOD.readCallback = [this]() -> u32 { return tperiod_; };
    TPERIOD.writeCallback = [this](u32 v) {
        if (Counting()) {
            if (warnings_++ < kMaxWarnings) LogWarn("guest-coretimer TPERIOD write %08X ignored while running (HRM 11-6)", v);
            return;
        }
        tperiod_ = v;
        count_ = v; // HRM 11-5: TPERIOD writes are copied to TCOUNT.
        stalled_ = false;
        StartIfNowCounting(false);
    };

    REG32(TSCALE, 0x08);
    TSCALE.readCallback = [this]() -> u32 { return tscale_; };
    TSCALE.writeCallback = [this](u32 v) {
        if (Counting()) {
            if (warnings_++ < kMaxWarnings)
                LogWarn("guest-coretimer TSCALE write %02X while running: count frozen and re-anchored (ASSUMED; HRM silent)", v & 0xFFu);
            Freeze();
            tscale_ = v & 0xFFu;
            anchor_ = Now();
            return;
        }
        tscale_ = v & 0xFFu;
    };

    REG32(TCOUNT, 0x0C);
    TCOUNT.readCallback = [this]() -> u32 { return CurrentCount(); };
    TCOUNT.writeCallback = [this](u32 v) {
        if (Counting()) {
            if (warnings_++ < kMaxWarnings) LogWarn("guest-coretimer TCOUNT write %08X ignored while running (HRM 11-5)", v);
            return;
        }
        count_ = v;
        stalled_ = false;
        StartIfNowCounting(false);
    };
}

uint64_t GuestCoreTimer::Now() const { return clock_.CclkCycles(); }

bool GuestCoreTimer::Counting() const { return power_ && enabled_ && !halted_ && !stalled_; }

u32 GuestCoreTimer::CountAt(uint64_t cclk) const {
    if (!Counting()) return count_;
    const uint64_t steps = (cclk - anchor_) / (uint64_t{tscale_} + 1);
    if (steps < count_) return static_cast<u32>(count_ - steps);
    // Past an expiry not yet synced (the CPU syncs after every advance).
    if (!autoReload_ || !tperiod_) return 0;
    return static_cast<u32>(tperiod_ - (steps - count_) % tperiod_);
}

u32 GuestCoreTimer::CurrentCount() const { return CountAt(Now()); }

std::optional<uint64_t> GuestCoreTimer::NextExpiryCclk() const {
    if (!Counting()) return std::nullopt;
    return anchor_ + uint64_t{count_} * (uint64_t{tscale_} + 1);
}

void GuestCoreTimer::Freeze() {
    count_ = CountAt(Now());
    anchor_ = Now();
}

void GuestCoreTimer::WriteControl(u32 v) {
    const bool wasCounting = Counting();
    const bool power = (v & TMPWR) != 0;
    const bool enabled = (v & TMREN) != 0;
    if (wasCounting && !(power && enabled)) Freeze();
    power_ = power;
    enabled_ = enabled;
    autoReload_ = (v & TAUTORLD) != 0;
    if (!(v & TINT)) tint_ = false; // HRM 11-4: not W1C; write 0 to clear.
    if (!enabled_) {
        halted_ = false;
        stalled_ = false;
    }
    if (enabled_ && !power_ && warnings_++ < kMaxWarnings)
        LogWarn("guest-coretimer TMREN set with TMPWR=0: undefined (HRM 11-3); not counting");
    StartIfNowCounting(wasCounting);
}

void GuestCoreTimer::StartIfNowCounting(bool wasCounting) {
    if (wasCounting || !Counting()) return;
    anchor_ = Now();
    if (!count_) {
        stalled_ = true;
        if (warnings_++ < kMaxWarnings) LogWarn("guest-coretimer started with TCOUNT=0: behavior UNKNOWN; not counting");
    }
}

uint64_t GuestCoreTimer::Sync() {
    if (!Counting()) return 0;
    const uint64_t scale = uint64_t{tscale_} + 1;
    const uint64_t steps = (Now() - anchor_) / scale;
    if (steps < count_) return 0;
    uint64_t expiries = 1;
    uint64_t consumed = count_;
    if (!autoReload_) {
        count_ = 0;
        halted_ = true;
    } else if (!tperiod_) {
        count_ = 0;
        stalled_ = true;
        if (warnings_++ < kMaxWarnings) LogWarn("guest-coretimer auto-reload with TPERIOD=0: behavior UNKNOWN; not counting");
    } else {
        expiries += (steps - count_) / tperiod_;
        consumed += (expiries - 1) * tperiod_;
        count_ = tperiod_;
    }
    // Rebase to the last expiry; the prescale phase is preserved, so periods do not drift.
    anchor_ += consumed * scale;
    tint_ = true;
    expiries_ += expiries;
    TriggerInterrupt(1);
    return expiries;
}
