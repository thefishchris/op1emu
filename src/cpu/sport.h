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

class SPORT : public RegisterDevice, public DMABus {
public:
    using AudioOutputCallback = std::function<void(const void* data, size_t samples, int channels, int bitsPerSample)>;
    using AudioInputCallback = std::function<size_t(void* data, size_t samples, int channels, int bitsPerSample)>;

    SPORT(u32 baseAddr, int sportNum);

    // Decoded TCR2 / RCR2 frame settings.
    SportConfig2 TransmitConfig2() const;
    SportConfig2 ReceiveConfig2() const;

    // DMABus interface
    u32 DMARead(int x, int y, void* dest, u32 length) override;
    u32 DMAWrite(int x, int y, const void* source, u32 length) override;

    // Audio callbacks
    void SetAudioOutputCallback(AudioOutputCallback callback) { audioOutputCallback = callback; }
    void SetAudioInputCallback(AudioInputCallback callback) { audioInputCallback = callback; }

protected:
    void SetTransmitEnable();
    void SetReceiveEnable();

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
};
