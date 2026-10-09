#pragma once

#include "io.h"
#include <optional>

class GuestClock;

// BF52x core timer (HRM chapter 11) driven by the guest clock's CCLK domain.
// Used only in experimental guest-clock mode; the legacy CoreTimer is
// unchanged. Awake semantics only: the CCLK-core domain is held during plain
// IDLE (docs/guest-clock-architecture.md, D6), so this device never advances
// itself through IDLE.
//
// CONFIRMED (HRM 11-1..11-7): TCOUNT decrements every TSCALE+1 CCLK while
// TMPWR and TMREN are set; reaching 0 sets sticky TINT and requests IVTMR
// (edge, direct to the CEC); TAUTORLD reloads from TPERIOD, otherwise the
// timer stops; TPERIOD writes copy to TCOUNT; TPERIOD/TCOUNT writes are
// ignored while running; TINT is not W1C (writing 0 clears it); there is no
// interrupt enable bit in the timer (masking is in the CEC); TMREN with
// TMPWR=0 is undefined.
//
// ASSUMED where the HRM is silent, each logged when first relied on:
//  - auto-reload happens in the same step that reaches 0, so a period is
//    TPERIOD * (TSCALE+1) CCLK (Linux/Blackfin's "-1" periodic setup hints at
//    TPERIOD+1; UNKNOWN);
//  - one-shot expiry halts counting with TCOUNT=0 and leaves TMREN readback
//    set; clearing TMREN re-arms;
//  - stopping (TMPWR/TMREN cleared) freezes TCOUNT and drops the partial
//    prescale interval; a TSCALE write while running does the same;
//  - writing 1 to TINT leaves it unchanged;
//  - starting with TCOUNT=0, or reloading TPERIOD=0, does not count (UNKNOWN);
//  - reset values of TCOUNT/TPERIOD/TSCALE ("undefined") are modelled as 0;
//  - several expiries between two syncs raise IVTMR once (CEC ILAT latch).
class GuestCoreTimer : public RegisterDevice {
public:
    GuestCoreTimer(u32 baseAddr, const GuestClock& clock);

    // CPU loop only, after each guest-clock advance. Processes expiries up to
    // the clock's current CCLK, raising IVTMR once if any occurred. Returns
    // the number of expiries.
    uint64_t Sync();

    // Powered, enabled, and consuming CCLK.
    bool Counting() const;
    // Absolute CCLK count at which TCOUNT next reaches 0, if counting.
    std::optional<uint64_t> NextExpiryCclk() const;
    u32 CurrentCount() const;
    uint64_t Expiries() const { return expiries_; }

private:
    u32 CountAt(uint64_t cclk) const;
    uint64_t Now() const;
    void Freeze();
    void WriteControl(u32 value);
    void StartIfNowCounting(bool wasCounting);

    const GuestClock& clock_;
    bool power_ = false;
    bool enabled_ = false;
    bool autoReload_ = false;
    bool tint_ = false;
    bool halted_ = false;   // One-shot expiry stopped counting.
    bool stalled_ = false;  // Started or reloaded at 0: behavior UNKNOWN.
    u32 tscale_ = 0;
    u32 tperiod_ = 0;
    u32 count_ = 0;         // TCOUNT at anchor_ while counting, else current.
    uint64_t anchor_ = 0;   // CCLK at which the current prescale sequence began.
    uint64_t expiries_ = 0;
    unsigned warnings_ = 0;
};
