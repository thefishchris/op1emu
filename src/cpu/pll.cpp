#include "pll.h"
#include <algorithm>

BF524PLL::BF524PLL(uint64_t provisionalClkinHz)
    : RegisterDevice("BF524 PLL", 0xFFC00000, 0x14), clkinHz_(provisionalClkinHz) {
    for (u32 offset : {0u, 4u, 8u, 12u, 16u}) {
        auto& reg = registers[offset];
        reg.addr = offset;
        reg.name = "PLL/VR";
        reg.readCallback = [this, offset]() -> u32 {
            switch (offset) {
            case 0: return ctl_;
            case 4: return div_;
            case 8: return vr_;
            case 12: return Status();
            default: return lockcnt_;
            }
        };
        reg.writeCallback = [this, offset](u32 value) {
            switch (offset) {
            case 0:
                ctlSame_ = ctl_ == (value & 0x7FEB);
                ctl_ = static_cast<u16>(value & 0x7FEB); ctlWritten_ = true; break;
            case 4:
                div_ = static_cast<u16>(value & 0x003F);
                if (!(div_ & 15)) unsupported_ = "reserved SSEL=0";
                break;
            case 8:
                vrSame_ = vr_ == (value & 0xF7F0);
                vr_ = static_cast<u16>(value & 0xF7F0); vrWritten_ = true; break;
            case 12: break; // Read-only.
            default: lockcnt_ = static_cast<u16>(value); break;
            }
        };
    }
}

bool BF524PLL::EnterIdle() {
    if (!ctlWritten_ && !vrWritten_) return false;
    if (remaining_) { unsupported_ = "programming during unfinished PLL transition"; return true; }
    if ((vr_ & 0x3000) != 0x3000) {
        unsupported_ = "BF524 hibernate/reserved VR FREQ request"; return true;
    }
    if (ctl_ & 0x0028) {
        unsupported_ = "PLL STOPCK/PDWN power mode"; return true;
    }
    if ((ctl_ & 2) && !(ctl_ & 0x100)) {
        unsupported_ = "PLL_OFF without BYPASS"; return true;
    }
    if (!(div_ & 15)) { unsupported_ = "reserved SSEL=0"; return true; }

    const bool same = (!ctlWritten_ || ctlSame_) && (!vrWritten_ || vrSame_);
    const bool relock = !same && ctlWritten_ && !(ctl_ & 2) &&
        (!activeCtl_ || ((*activeCtl_ ^ ctl_) & 0x7E01) || (*activeCtl_ & 2));
    if (ctlWritten_) activeCtl_ = ctl_;
    if (vrWritten_) activeVR_ = vr_; // VLEV is stored, not an internal regulator.
    ctlWritten_ = vrWritten_ = false;
    wake_ = false;
    if (relock || same) {
        remaining_ = lockcnt_;
        temporaryBypass_ = remaining_ != 0;
        locked_ = !relock;
        wakeOnLock_ = relock;
        // Same-value programming wakes immediately but stays bypassed.
        wake_ = same;
        if (!remaining_) { locked_ = true; wake_ = true; wakeOnLock_ = false; }
    } else {
        locked_ = !(ctl_ & 2);
        wake_ = true; // HR 18-15: no mode transition generates a wake signal.
    }
    return true;
}

void BF524PLL::AdvanceClkin(uint64_t ticks) {
    ticks_ += ticks;
    const auto elapsed = std::min(ticks, remaining_);
    remaining_ -= elapsed;
    if (elapsed && !remaining_) {
        temporaryBypass_ = false;
        locked_ = true;
        if (wakeOnLock_) wake_ = true;
        wakeOnLock_ = false;
    }
}

std::optional<uint64_t> BF524PLL::NextDeadlineTicks() const {
    if (unsupported_ || !remaining_) return std::nullopt;
    return remaining_;
}

bool BF524PLL::CanAdvanceAwakeBypass(bool experimentEnabled) const {
    return experimentEnabled && clkinHz_ && temporaryBypass_ && NextDeadlineTicks() && CoreHz() == clkinHz_;
}

BF524PLL::AwakeAdvance BF524PLL::AdvanceAwakeBypass(uint64_t cycleDelta, bool executedAwake, bool experimentEnabled) {
    if (!executedAwake || !cycleDelta || !CanAdvanceAwakeBypass(experimentEnabled)) return {};
    const uint64_t consumed = std::min(cycleDelta, remaining_);
    const uint64_t overshoot = cycleDelta - consumed;
    // CCLK == CLKIN in known bypass: one accounted CCLK cycle per input tick.
    // Never advance the input domain beyond expiry, even if this block crossed it.
    AdvanceClkin(consumed);
    return {consumed, overshoot, remaining_ == 0};
}

u32 BF524PLL::Status() const {
    if (!activeCtl_) return temporaryBypass_ ? 0x00A1 : 0x00A2;
    const u32 mode = (*activeCtl_ & 2) ? 8u :
        (temporaryBypass_ || (*activeCtl_ & 0x100) ? 1u : 2u);
    // VSTAT reset compatibility; BF524 has no modeled internal voltage regulator.
    return 0x80u | (locked_ ? 0x20u : 0u) | mode;
}

std::optional<uint64_t> BF524PLL::CoreHz() const {
    if (unsupported_ || !clkinHz_) return std::nullopt;
    if (temporaryBypass_) return clkinHz_;
    if (!activeCtl_) return std::nullopt;
    if (temporaryBypass_ || (*activeCtl_ & 0x100)) return clkinHz_;
    const u32 msel = (*activeCtl_ >> 9) & 63;
    const uint64_t vco = clkinHz_ * (msel ? msel : 64u) / ((*activeCtl_ & 1) ? 2u : 1u);
    return vco / (1u << ((div_ >> 4) & 3));
}

std::optional<uint64_t> BF524PLL::SystemHz() const {
    if (unsupported_ || !clkinHz_ || !(div_ & 15)) return std::nullopt;
    if (temporaryBypass_) return clkinHz_;
    if (!activeCtl_) return std::nullopt;
    if (temporaryBypass_ || (*activeCtl_ & 0x100)) return clkinHz_;
    const u32 msel = (*activeCtl_ >> 9) & 63;
    const uint64_t vco = clkinHz_ * (msel ? msel : 64u) / ((*activeCtl_ & 1) ? 2u : 1u);
    // HR 18-5: hardware raises effective SSEL to at least the CCLK divider.
    return vco / std::max<u32>(div_ & 15, 1u << ((div_ >> 4) & 3));
}
