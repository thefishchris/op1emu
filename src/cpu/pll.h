#pragma once

#include "io.h"
#include <optional>

// BF52x HR 18-5, 18-14/15, figures 18-4,6,7,8,10. BF524 only.
// Disconnected input-clock domain: neither packets nor host time advance it.
class BF524PLL final : public RegisterDevice {
public:
    explicit BF524PLL(uint64_t provisionalClkinHz = 0);
    bool EnterIdle(); // Commit a pending programming request; false if none.
    void AdvanceClkin(uint64_t ticks);
    std::optional<uint64_t> NextDeadlineTicks() const;
    bool WakeAsserted() const { return wake_; }
    void AcknowledgeWake() { wake_ = false; }
    const char* Unsupported() const { return unsupported_; }
    std::optional<u16> ActiveCtl() const { return activeCtl_; }
    u16 ActiveVR() const { return activeVR_; }
    u16 Divider() const { return div_; }
    uint64_t ClkinTicks() const { return ticks_; }
    uint64_t ProvisionalClkinHz() const { return clkinHz_; }
    std::optional<uint64_t> CoreHz() const;
    std::optional<uint64_t> SystemHz() const;
    u32 Status() const;
    struct AwakeAdvance { uint64_t consumed = 0, overshoot = 0; bool expired = false; };
    bool CanAdvanceAwakeBypass(bool experimentEnabled) const;
    // EXPERIMENTAL ideal/no-stall path, bounded to the current bypass deadline.
    // Caller supplies only cycles newly accounted during awake guest execution.
    AwakeAdvance AdvanceAwakeBypass(uint64_t cycleDelta, bool executedAwake, bool experimentEnabled);

private:
    uint64_t clkinHz_;
    uint64_t ticks_ = 0;
    uint64_t remaining_ = 0;
    u16 ctl_ = 0x0B00, div_ = 0x0004, vr_ = 0x70B0, lockcnt_ = 0x0200;
    // HR reset CTL figure's binary, hex, and STAT are inconsistent. Preserve
    // documented MMR reset readback; do not infer an active reset frequency.
    std::optional<u16> activeCtl_;
    u16 activeVR_ = 0x70B0;
    bool ctlWritten_ = false, vrWritten_ = false;
    bool ctlSame_ = false, vrSame_ = false;
    bool locked_ = true, temporaryBypass_ = false, wake_ = false;
    bool wakeOnLock_ = false;
    const char* unsupported_ = nullptr;
};
