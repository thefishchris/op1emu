#pragma once

#include "io.h"
#include "dma.h"
#include <vector>
#include <queue>
#include <optional>
#include <functional>
#include <chrono>
#include <cstdint>

// SPORT_TCR2 / SPORT_RCR2 frame settings (BF52x HRM figures 24-26 and 24-28):
// SLEN[4:0] = word length - 1; bit 8 TXSE/RXSE (secondary side enable);
// bit 9 TSFSE/RSFSE (frame sync becomes the L/R clock: two words per frame);
// bit 10 TRFST/RRFST (right channel first).
struct SportConfig2 {
    int wordBits;
    bool secondary;
    bool stereo;
    bool rightFirst;
    int Channels() const { return stereo ? 2 : 1; }
};

SportConfig2 DecodeSportConfig2(u32 value);

// Guest-time SPORT pacing (functional profile only; docs/sport-frame-format.md).
// SPORT0's serial and frame clocks are external, so the word rate is the frame
// rate times the words per frame, timed on guest CLKIN with exact integer
// arithmetic. No host time is read.
struct SportGuestClock {
    std::function<uint64_t()> clkinTicks; // Current guest CLKIN tick.
    uint64_t clkinHz = 0;                 // Must not exceed 2^32.
    uint64_t frameHz = 0;                 // Must not exceed 2^31.
    explicit operator bool() const { return clkinTicks && clkinHz && frameHz; }
};

// Whole frames due `elapsedClkin` ticks after the start:
// floor(elapsedClkin * frameHz / clkinHz), without intermediate overflow.
uint64_t SportFramesDue(uint64_t elapsedClkin, uint64_t clkinHz, uint64_t frameHz);
// Smallest elapsed CLKIN count at which `frames` frames are due.
uint64_t SportClkinForFrames(uint64_t frames, uint64_t clkinHz, uint64_t frameHz);

class SPORT : public RegisterDevice, public DMABus {
public:
    using AudioOutputCallback = std::function<void(const void* data, size_t samples, int channels, int bitsPerSample)>;
    using AudioInputCallback = std::function<size_t(void* data, size_t samples, int channels, int bitsPerSample)>;

    SPORT(u32 baseAddr, int sportNum);

    // Decoded TCR2 / RCR2 frame settings.
    SportConfig2 TransmitConfig2() const;
    SportConfig2 ReceiveConfig2() const;

    // Pace both directions on guest CLKIN instead of host time. Throws
    // std::invalid_argument for an incomplete or out-of-range clock.
    void UseGuestClock(SportGuestClock clock);
    bool GuestPaced() const { return static_cast<bool>(guestClock_); }
    // Guest pacing only: the CLKIN tick at which a DMA request of `bytes`
    // can be satisfied in full, as whole frames; nullopt when the direction
    // is disabled, has not started (no DMA access since enable), or `bytes`
    // holds no whole frame.
    std::optional<uint64_t> TransmitDueClkin(u32 bytes) const;
    std::optional<uint64_t> ReceiveDueClkin(u32 bytes) const;
    // Words moved since the direction was enabled.
    uint64_t TransmitWords() const { return totalTxSamplesDelivered_; }
    uint64_t ReceiveWords() const { return totalRxSamplesDelivered_; }
    // Guest pacing: CLKIN tick of the first DMA access since enable.
    std::optional<uint64_t> TransmitStartClkin() const { return guestClock_ && dmaTxActive_ ? std::optional<uint64_t>(dmaTxStartClkin_) : std::nullopt; }
    std::optional<uint64_t> ReceiveStartClkin() const { return guestClock_ && dmaRxActive_ ? std::optional<uint64_t>(dmaRxStartClkin_) : std::nullopt; }

    // DMABus interface
    u32 DMARead(int x, int y, void* dest, u32 length) override;
    u32 DMAWrite(int x, int y, const void* source, u32 length) override;

    // Audio callbacks
    void SetAudioOutputCallback(AudioOutputCallback callback) { audioOutputCallback = callback; }
    void SetAudioInputCallback(AudioInputCallback callback) { audioInputCallback = callback; }

protected:
    void SetTransmitEnable();
    void SetReceiveEnable();
    uint64_t GuestWordsDue(uint64_t startClkin, int channels) const;
    std::optional<uint64_t> GuestDueClkin(bool enabled, bool active, uint64_t startClkin, uint64_t delivered,
                                          int wordBits, bool stereo, u32 bytes) const;

    int sportNumber;

    bool transmitEnabled = false;
    u8 transmitDataFormat = 0;
    bool transmitOrderLsbFirst = false;
    u8 transmitWordLength = 0;
    bool transmitSecondaryEnabled = false;
    bool transmitStereoFrameSync = false;
    bool transmitRightStereoOrderFirst = false;
    bool transmitOverflow = false;
    bool transmitUnderflow = false;

    bool receiveEnabled = false;
    u8 receiveDataFormat = 0;
    bool receiveOrderLsbFirst = false;
    u8 receiveWordLength = 0;
    bool receiveSecondaryEnabled = false;
    bool receiveStereoFrameSync = false;
    bool receiveRightStereoOrderFirst = false;
    bool receiveOverflow = false;
    bool receiveUnderflow = false;

    std::optional<u32> transmitHoldRegister;
    std::optional<u32> receiveHoldRegister;
    // FIFO buffers
    std::queue<u32> transmitFifo;
    std::queue<u32> receiveFifo;

    // Audio callbacks
    AudioOutputCallback audioOutputCallback;
    AudioInputCallback audioInputCallback;

    // DMA timing state — TX path
    std::chrono::steady_clock::time_point dmaTxStartTime_;
    uint64_t totalTxSamplesDelivered_ = 0;
    bool dmaTxActive_ = false;

    // DMA timing state — RX path
    std::chrono::steady_clock::time_point dmaRxStartTime_;
    uint64_t totalRxSamplesDelivered_ = 0;
    bool dmaRxActive_ = false;

    // Nominal sample rate in Hz (future: derive from TCLKDIV/TFSDIV registers)
    uint32_t sampleRateHz_ = 48000;

    // Guest pacing (UseGuestClock): CLKIN tick of each direction's first DMA
    // access after enable. Words due = frames due x words per frame.
    SportGuestClock guestClock_;
    uint64_t dmaTxStartClkin_ = 0;
    uint64_t dmaRxStartClkin_ = 0;
};
