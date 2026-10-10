#pragma once

#include "emu.h"
#include "mmio_access.h"
#include "guest_clock.h"
#include "timing_profile.h"
#include <atomic>
#include <memory>
#include <vector>
#include <chrono>
#include <string>
#include <utility>
#include <optional>

// Forward declarations for bcore types (headers included in cpu.cpp only)
struct CpuState;
struct ShadowTiming;
class Core;
class EmulatorMemory;

enum RegIndex {
    FP,
    SP,
    RETS,
    R0,
    R1,
    R2,
    P1,
};

class SIC;
class CoreTimer;
class GuestCoreTimer;
class RegisterDevice;
class BF524PLL;
class GPIO;
class PPI;
class Display;
class OLED;
class NFC;
class DMA;
class GPTimer;
class USBDevice;
class NandFlash;
class Keyboard;
class MCP230XX;
class ADXL345;
class Potentiometer;
class GPIOPeripheral;
class SPORT;
class AudioOutput;

class BlackFinCpu : public CpuInterface {
public:
    BlackFinCpu();
    ~BlackFinCpu() override;

    void HaltExecution(HaltReason reason) override;
    void SaveContext() override;
    void RestoreContext() override;
    HaltReason Run() override;

    void SetRegister(int index, u32 value) override;
    u32 GetRegister(int index) override;

    void SetPC(u32 value) override;
    u32 PC() override;

    // CPU-thread-only runtime packet entries, including faulting attempts.
    // Not retired instructions or cycles; unsigned count wraps modulo 2^64.
    uint64_t PacketEntryCount() const;
    const ShadowTiming* ShadowTimingStats() const;
    bool IsIdle() const;
    uint64_t IdleEntryCount() const;
    bool UnsupportedIdle() const { return unsupportedIdle_; }
    const char* UnsupportedIdleReason() const { return unsupportedIdleReason_; }
    // Experimental guest-clock mode (OP1EMU_EXPERIMENTAL_GUEST_CLOCK=1); see
    // docs/guest-clock-architecture.md. Fixed at construction.
    bool GuestClockMode() const { return guestClockMode_; }
    GuestCostModel GuestClockCostModel() const { return guestCostModel_; }
    const GuestClock& GuestTime() const { return guestClock_; }
    // Guest-clock mode only; null otherwise.
    GuestCoreTimer* GuestTimer() const { return guestCoreTimer_.get(); }
    // Resolved execution profile and timing assumptions (fixed at construction).
    const TimingConfig& Timing() const { return timing_; }
    SPORT& Sport(int index) { return index ? *sport1 : *sport0; }

    // Multi-block execution groundwork, milestone M0
    // (docs/multiblock-execution.md). Passive: nothing here changes when or
    // how devices are serviced. Off by default (OP1EMU_BATCH_DIAG=1 enables
    // barrier statistics, OP1EMU_BATCH_DIAG=verify also verifies them).
    enum BatchBarrier : uint32_t {
        BarrierMmio = 1u << 0,              // the block accessed a memory-mapped register
        BarrierInterruptChange = 1u << 1,   // IPEND or ILAT changed during the block
        BarrierInterruptPending = 1u << 2,  // ILAT & IMASK != 0: the dispatch check may act
        BarrierIdle = 1u << 3,              // the block entered architectural IDLE
        BarrierGuestDeadline = 1u << 4,     // the core timer expired during the block
        BarrierPllCountdown = 1u << 5,      // a PLL lock or awake-bypass countdown is outstanding
        BarrierDeviceWork = 1u << 6,        // some device's service pass is not proven a no-op
        BarrierQueuedEvents = 1u << 7,      // queued events or SIC latch requests
        BarrierTracing = 1u << 8,           // per-block tracing is enabled
    };
    static constexpr unsigned kBatchBarrierCount = 9;
    struct BatchDiagnostics {
        uint64_t blocks = 0;                     // awake blocks evaluated
        uint64_t eligible = 0;                   // blocks with no barrier
        uint64_t barriers[kBatchBarrierCount] = {};
        std::vector<uint64_t> deviceWork;        // per device (devices order), plus GP timer ticks last
        uint64_t verified = 0, mismatches = 0;   // eligible passes run and compared
        std::string firstMismatch;
    };
    void SetBatchDiagnostics(bool enabled, bool verify);
    const BatchDiagnostics& BatchStats() const { return batchStats_; }
    uint32_t LastBatchBarriers() const { return lastBatchBarriers_; }
    std::string DeviceName(size_t index) const;

    // Exact, named emulator state after a block (CPU, CEC, SIC, DMA, SPORT,
    // guest clock, core timer, PLL, NFC/RTC/TWI/USB registers without read
    // side effects, queued work, DMA memory writes). Host-time values
    // (RTC_STAT, CYCLES) and diagnostic counters are excluded.
    using BlockState = std::vector<std::pair<std::string, uint64_t>>;
    BlockState CaptureBlockState();
    // First differing field, or empty when equal.
    static std::string DiffBlockState(const BlockState& a, const BlockState& b);
    // Exact log of guest MMR accesses (nullptr stops logging).
    void SetMMIOLog(std::vector<MMIOAccess>* log);
    bool Failed() const { return failed_.load(std::memory_order_acquire); }
    BF524PLL& ClockState() { return *pll_; }
    struct AwakePLLStats { uint64_t consumed = 0, overshoot = 0, blocks = 0, expiries = 0; };
    const AwakePLLStats& AwakePLLExperimentStats() const { return awakePLLStats_; }

    Emulator& GetEmulator() { return emulator; }
    USBDevice& GetUSB() { return *usb; }

    void SetBootMode(int mode);

    void QueueEvent(const std::function<void()>& event, std::chrono::nanoseconds delay = std::chrono::nanoseconds(1));

    // SIC -> CEC delivery (docs/sic-interrupt-delivery.md):
    //  - requested: rising edges of SIC lines (latch requests);
    //  - latched: requests latched into ILAT;
    //  - inService: requests dropped because that IVG was being serviced
    //    (re-pended at its RTI instead if the line is still asserted);
    //  - repended: latches made at RTI for a still-asserted line.
    struct InterruptRaiseStats { uint64_t requested = 0, latched = 0, inService = 0, repended = 0; };
    InterruptRaiseStats RaiseStats() {
        std::lock_guard<std::recursive_mutex> lock(eventQueueMutex);
        return raiseStats_;
    }

    void AttachDisplay(const std::shared_ptr<Display>& display);
    void AttachKeyboard(const std::shared_ptr<Keyboard>& keyboard);
    void AttachNandFlash(const std::shared_ptr<NandFlash>& nandFlash);
    void AttachAudioOutput(const std::shared_ptr<AudioOutput>& audioOutput);
    void SetAcceleration(int16_t x, int16_t y, int16_t z);
    void SetPotentiometerValue(u8 value);

protected:
    virtual bool ExecuteCoreBlock(u32 address);
    void ProcessInterrupt(int pin, int level);
    void QueueInterruptRaise(int ivg);
    void LatchRequestedInterrupts();
    void RepeatAssertedAfterReturn(u32 ipendBefore);
    uint32_t EvaluateBatchBarriers(u32 ipendBefore, u32 ilatBefore, uint64_t mmioBefore, uint64_t expiriesBefore,
                                   bool gpTimerTickDue);
    // Out of line so that the disabled path costs only the flag tests in Run().
    [[gnu::cold, gnu::noinline]] bool DiagnoseBatchBlock(u32 ipendBefore, u32 ilatBefore, uint64_t mmioBefore,
                                                         uint64_t expiriesBefore, bool gpTimerTickDue);
    [[gnu::cold, gnu::noinline]] void VerifyBatchPass();
    void ProcessEvents();
    void ServiceIdle();
    void ReportUnsupportedIdle(const char* reason);
    void ReportHeldDomainIdle();
    enum class FunctionalIdleStep { None, Advanced, Stopped };
    // Functional profile only: advance architectural IDLE to the earliest
    // modelled deadline (PLL lock, core timer expiry, SPORT DMA row end).
    // One step per call.
    FunctionalIdleStep AdvanceFunctionalIdle();
    // Guest-paced SPORT only: the earliest future CLKIN tick at which a
    // running SPORT DMA channel can finish its current row.
    std::optional<uint64_t> NextSportDmaDeadline() const;
    // Guest-paced SPORT only: move every SPORT DMA word due at the current
    // guest time. Interrupts follow the normal DMA -> SIC routing.
    uint64_t ServiceSportDma();

    // Declared before the devices, which may keep a reference to it.
    GuestClock guestClock_;
    TimingConfig timing_;
    bool guestClockMode_ = false;
    GuestCostModel guestCostModel_ = GuestCostModel::MinCost;
    std::shared_ptr<SIC> sic;
    // MMR view of whichever core timer is active (diagnostics read it).
    std::shared_ptr<RegisterDevice> coreTimer;
    std::shared_ptr<CoreTimer> legacyCoreTimer_;      // Default mode.
    std::shared_ptr<GuestCoreTimer> guestCoreTimer_;  // Guest-clock mode.
    std::shared_ptr<BF524PLL> pll_;
    bool unsupportedIdle_ = false;
    const char* unsupportedIdleReason_ = nullptr;
    uint64_t functionalIdleAdvances_ = 0;
    uint64_t functionalIdleWakes_ = 0;
    std::atomic<bool> failed_{false};
    bool awakePLLExperiment_ = false;
    AwakePLLStats awakePLLStats_;
    std::shared_ptr<GPIO> portG;
    std::shared_ptr<PPI> ppi;
    std::shared_ptr<OLED> oled;
    std::shared_ptr<NFC> nfc;
    std::shared_ptr<DMA> dma;
    std::shared_ptr<GPTimer> gptimer;
    std::shared_ptr<USBDevice> usb;
    std::shared_ptr<SPORT> sport0;
    std::shared_ptr<SPORT> sport1;
    std::shared_ptr<ADXL345> adxl345;
    std::shared_ptr<Potentiometer> potentiometer;
    std::shared_ptr<GPIOPeripheral> gpioOrConnection;
    std::vector<std::shared_ptr<MCP230XX>> gpioExpanders;
    std::vector<std::shared_ptr<Device>> devices;
    std::vector<std::tuple<std::chrono::nanoseconds, std::function<void()>>> eventQueue;
    std::recursive_mutex eventQueueMutex;
    // IVGs with a SIC line rising edge not yet latched (bit n = IVGn), guarded
    // by eventQueueMutex: the SIC may be driven from a host thread (USB).
    u32 requestedRaises_ = 0;
    bool batchDiagnostics_ = false;
    bool batchVerify_ = false;
    bool perBlockTracing_ = false;
    uint32_t lastBatchBarriers_ = 0;
    BatchDiagnostics batchStats_;
    BlockState batchPassBefore_;
    InterruptRaiseStats raiseStats_;
    std::chrono::nanoseconds elapsedTime{0};
    std::chrono::system_clock::time_point startTime;
    Emulator emulator;
    std::unique_ptr<CpuState> cpuState_;
    std::unique_ptr<ShadowTiming> shadowTiming_;
    std::unique_ptr<EmulatorMemory> bcoreMemory_;
    std::shared_ptr<Core> core_;
    uint32_t pc;
};
