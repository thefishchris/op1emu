#include "sport.h"
#include "emu.h"
#include "utils/log.h"
#include <stdexcept>

enum DataFormat {
    NORMAL = 0,
    COMPANDING_U_LAW = 2,
    COMPANDING_A_LAW = 3,
};

constexpr std::size_t FIFO_SIZE = 8; // 8x16-bit words or 4x32-bit words

SPORT::SPORT(u32 baseAddr, int sportNum)
    : RegisterDevice("SPORT" + std::to_string(sportNum), baseAddr, 0x60), sportNumber(sportNum) {

    REG32(SPORT_TCR1, 0x00);
    FIELD(SPORT_TCR1, TSPEN, 0, 1, R(transmitEnabled), W(transmitEnabled));
    FIELD(SPORT_TCR1, TDTYPE, 2, 2, R(transmitDataFormat), W(transmitDataFormat));
    FIELD(SPORT_TCR1, TLSBIT, 4, 1, R(transmitOrderLsbFirst), W(transmitOrderLsbFirst));
    SPORT_TCR1.writeCallback = [this](u32 value) {
        SetTransmitEnable();
    };

    REG32(SPORT_TCR2, 0x04);
    FIELD(SPORT_TCR2, SLEN, 0, 5, R(transmitWordLength - 1), [this](u32 v) {
        transmitWordLength = v + 1;
    });
    // BF52x HRM figure 24-26: TXSE bit 8, TSFSE bit 9, TRFST bit 10.
    FIELD(SPORT_TCR2, TXSE, 8, 1, R(transmitSecondaryEnabled), W(transmitSecondaryEnabled));
    FIELD(SPORT_TCR2, TSFSE, 9, 1, R(transmitStereoFrameSync), W(transmitStereoFrameSync));
    FIELD(SPORT_TCR2, TRFST, 10, 1, R(transmitRightStereoOrderFirst), W(transmitRightStereoOrderFirst));

    REG32(SPORT_TX, 0x10);
    SPORT_TX.writeCallback = [this](u32 v) {
        std::size_t fifoSize = transmitWordLength > 16 ? FIFO_SIZE/2 : FIFO_SIZE;
        if (transmitFifo.size() >= fifoSize) {
            transmitOverflow = true;
        } else {
            transmitFifo.push(v);
        }
    };

    REG32(RX, 0x18);
    FIELD(RX, VAL, 0, 32, [this]() {
        if (receiveFifo.empty()) {
            receiveUnderflow = true;
            return 0u;
        } else {
            u32 value = receiveFifo.front();
            receiveFifo.pop();
            return value;
        }
    }, N());

    REG32(SPORT_RCR1, 0x20);
    FIELD(SPORT_RCR1, RSPEN, 0, 1, R(receiveEnabled), W(receiveEnabled));
    FIELD(SPORT_RCR1, RDTYPE, 2, 2, R(receiveDataFormat), W(receiveDataFormat));
    FIELD(SPORT_RCR1, RLSBIT, 4, 1, R(receiveOrderLsbFirst), W(receiveOrderLsbFirst));
    SPORT_RCR1.writeCallback = [this](u32 value) {
        SetReceiveEnable();
    };

    REG32(SPORT_RCR2, 0x24);
    FIELD(SPORT_RCR2, SLEN, 0, 5, R(receiveWordLength - 1), [this](u32 v) {
        receiveWordLength = v + 1;
    });
    // BF52x HRM figure 24-28: RXSE bit 8, RSFSE bit 9, RRFST bit 10.
    FIELD(SPORT_RCR2, RXSE, 8, 1, R(receiveSecondaryEnabled), W(receiveSecondaryEnabled));
    FIELD(SPORT_RCR2, RSFSE, 9, 1, R(receiveStereoFrameSync), W(receiveStereoFrameSync));
    FIELD(SPORT_RCR2, RRFST, 10, 1, R(receiveRightStereoOrderFirst), W(receiveRightStereoOrderFirst));

    REG32(SPORT_STAT, 0x30);
    FIELD(SPORT_STAT, RXNE, 0, 1, R(!receiveFifo.empty()), N());
    FIELD(SPORT_STAT, RUVF, 1, 1, R(receiveUnderflow), W1C(receiveUnderflow));
    FIELD(SPORT_STAT, ROVF, 2, 1, R(receiveOverflow), W1C(receiveOverflow));
    FIELD(SPORT_STAT, TXF, 3, 1, R(transmitFifo.size() >= (transmitWordLength > 16 ? FIFO_SIZE/2 : FIFO_SIZE)), N());
    FIELD(SPORT_STAT, TUVF, 4, 1, R(transmitUnderflow), W1C(transmitUnderflow));
    FIELD(SPORT_STAT, TOVF, 5, 1, R(transmitOverflow), W1C(transmitOverflow));
    FIELD(SPORT_STAT, TXHRE, 6, 1, R(transmitHoldRegister.has_value()), N());
}

void SPORT::SetTransmitEnable() {
    if (!transmitEnabled) {
        transmitOverflow = false;
        transmitUnderflow = false;
        transmitHoldRegister.reset();
        dmaTxActive_ = false;
        totalTxSamplesDelivered_ = 0;
    }
}

void SPORT::SetReceiveEnable() {
    if (!receiveEnabled) {
        receiveOverflow = false;
        receiveUnderflow = false;
        receiveHoldRegister.reset();
        dmaRxActive_ = false;
        totalRxSamplesDelivered_ = 0;
    }
}

u32 SPORT::DMARead(int x, int y, void* dest, u32 length)
{
    if (!receiveEnabled) {
        return 0;
    }

    int wordSize = receiveWordLength > 16 ? 4 : 2;
    int channels = receiveStereoFrameSync ? 2 : 1;
    int bitsPerSample = receiveWordLength;
    int frameSize = wordSize * channels;
    // Pacing counts SPORT words, as before stereo frame sync was decoded, so the
    // DMA word and interrupt cadence is unchanged; only whole L/R frames move.
    // Frame-based pacing is deferred to the SPORT clock work.
    size_t requestedSamples = length / static_cast<size_t>(wordSize);
    if (requestedSamples < static_cast<size_t>(channels)) return 0;

    // Start timing on first DMA call after SPORT enable
    if (!dmaRxActive_) {
        if (guestClock_) dmaRxStartClkin_ = guestClock_.clkinTicks();
        else dmaRxStartTime_ = std::chrono::steady_clock::now();
        totalRxSamplesDelivered_ = 0;
        dmaRxActive_ = true;
    }

    // How many samples are due based on elapsed time?
    uint64_t totalDue = 0;
    if (guestClock_) {
        totalDue = GuestWordsDue(dmaRxStartClkin_, channels);
    } else {
        auto elapsed = std::chrono::steady_clock::now() - dmaRxStartTime_;
        uint64_t elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
        totalDue = (elapsedNs * sampleRateHz_) / 1'000'000'000ULL;
        // FIXME: too slow, / 10
        totalDue = totalDue / 10;
    }
    uint64_t available = (totalDue > totalRxSamplesDelivered_)
                         ? (totalDue - totalRxSamplesDelivered_) : 0;

    if (available > requestedSamples) available = requestedSamples;
    available -= available % static_cast<uint64_t>(channels); // Whole frames only.
    if (available == 0) return 0;  // Not time yet — DMA retries next cycle
    const uint64_t frames = available / static_cast<uint64_t>(channels);

    if (audioInputCallback) {
        size_t read = audioInputCallback(dest, frames, channels, bitsPerSample);
        totalRxSamplesDelivered_ += read * static_cast<size_t>(channels);
        return static_cast<u32>(read * frameSize);
    }

    // Fill with silence
    totalRxSamplesDelivered_ += available;
    return static_cast<u32>(frames * static_cast<uint64_t>(frameSize));
}

u32 SPORT::DMAWrite(int x, int y, const void* source, u32 length)
{
    if (!transmitEnabled) {
        return 0;
    }

    int wordSize = transmitWordLength > 16 ? 4 : 2;
    int channels = transmitStereoFrameSync ? 2 : 1;
    int bitsPerSample = transmitWordLength;
    int frameSize = wordSize * channels;
    // Pacing counts SPORT words, as before stereo frame sync was decoded, so the
    // DMA word and interrupt cadence is unchanged; only whole L/R frames move.
    // Frame-based pacing is deferred to the SPORT clock work.
    size_t requestedSamples = length / static_cast<size_t>(wordSize);
    if (requestedSamples < static_cast<size_t>(channels)) return 0;

    // Start timing on first DMA call after SPORT enable
    if (!dmaTxActive_) {
        if (guestClock_) dmaTxStartClkin_ = guestClock_.clkinTicks();
        else dmaTxStartTime_ = std::chrono::steady_clock::now();
        totalTxSamplesDelivered_ = 0;
        dmaTxActive_ = true;
    }

    // How many samples are due based on elapsed time?
    uint64_t totalDue = 0;
    if (guestClock_) {
        totalDue = GuestWordsDue(dmaTxStartClkin_, channels);
    } else {
        auto elapsed = std::chrono::steady_clock::now() - dmaTxStartTime_;
        uint64_t elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
        totalDue = (elapsedNs * sampleRateHz_) / 1'000'000'000ULL;
        // FIXME: too slow, / 10
        totalDue = totalDue / 10;
    }
    uint64_t available = (totalDue > totalTxSamplesDelivered_)
                         ? (totalDue - totalTxSamplesDelivered_) : 0;

    if (available > requestedSamples) available = requestedSamples;
    available -= available % static_cast<uint64_t>(channels); // Whole frames only.
    if (available == 0) return 0;  // Not time yet — DMA retries next cycle
    const uint64_t frames = available / static_cast<uint64_t>(channels);

    if (audioOutputCallback) {
        audioOutputCallback(source, frames, channels, bitsPerSample);
    }

    totalTxSamplesDelivered_ += available;
    return static_cast<u32>(frames * static_cast<uint64_t>(frameSize));
}

uint64_t SportFramesDue(uint64_t elapsedClkin, uint64_t clkinHz, uint64_t frameHz) {
    return (elapsedClkin / clkinHz) * frameHz + ((elapsedClkin % clkinHz) * frameHz) / clkinHz;
}

uint64_t SportClkinForFrames(uint64_t frames, uint64_t clkinHz, uint64_t frameHz) {
    const uint64_t rest = frames % frameHz;
    return (frames / frameHz) * clkinHz + (rest * clkinHz + frameHz - 1) / frameHz;
}

void SPORT::UseGuestClock(SportGuestClock clock) {
    if (!clock || clock.clkinHz > (uint64_t{1} << 32) || clock.frameHz > (uint64_t{1} << 31))
        throw std::invalid_argument("SPORT guest clock needs a CLKIN source, CLKIN <= 2^32 Hz and frame rate <= 2^31 Hz");
    guestClock_ = std::move(clock);
}

uint64_t SPORT::GuestWordsDue(uint64_t startClkin, int channels) const {
    const uint64_t now = guestClock_.clkinTicks();
    const uint64_t elapsed = now >= startClkin ? now - startClkin : 0;
    return SportFramesDue(elapsed, guestClock_.clkinHz, guestClock_.frameHz) * static_cast<uint64_t>(channels);
}

std::optional<uint64_t> SPORT::GuestDueClkin(bool enabled, bool active, uint64_t startClkin, uint64_t delivered,
                                             int wordBits, bool stereo, u32 bytes) const {
    if (!guestClock_ || !enabled || !active) return std::nullopt;
    // Mirrors DMARead/DMAWrite: whole words, then whole frames.
    const uint64_t wordBytes = wordBits > 16 ? 4 : 2;
    const uint64_t channels = stereo ? 2 : 1;
    const uint64_t frames = bytes / wordBytes / channels;
    if (frames == 0) return std::nullopt;
    const uint64_t deliveredFrames = (delivered + channels - 1) / channels;
    return startClkin + SportClkinForFrames(deliveredFrames + frames, guestClock_.clkinHz, guestClock_.frameHz);
}

std::optional<uint64_t> SPORT::TransmitDueClkin(u32 bytes) const {
    return GuestDueClkin(transmitEnabled, dmaTxActive_, dmaTxStartClkin_, totalTxSamplesDelivered_,
                         transmitWordLength, transmitStereoFrameSync, bytes);
}

std::optional<uint64_t> SPORT::ReceiveDueClkin(u32 bytes) const {
    return GuestDueClkin(receiveEnabled, dmaRxActive_, dmaRxStartClkin_, totalRxSamplesDelivered_,
                         receiveWordLength, receiveStereoFrameSync, bytes);
}

bool SPORT::TransferPossible(bool toMemory) const {
    if (!guestClock_ || !transferGating_) return true;
    if (!(toMemory ? receiveEnabled : transmitEnabled)) return false; // DMARead/DMAWrite return 0 at once.
    if (!(toMemory ? dmaRxActive_ : dmaTxActive_)) return true;       // This access starts the timeline.
    const uint64_t start = toMemory ? dmaRxStartClkin_ : dmaTxStartClkin_;
    const uint64_t delivered = toMemory ? totalRxSamplesDelivered_ : totalTxSamplesDelivered_;
    const uint64_t channels = (toMemory ? receiveStereoFrameSync : transmitStereoFrameSync) ? 2 : 1;
    TransferGate& gate = toMemory ? rxGate_ : txGate_;
    if (gate.start != start || gate.delivered != delivered || gate.channels != channels) {
        // DMARead/DMAWrite move data only when words due - delivered >= one
        // frame, i.e. frames due >= ceil(delivered / channels) + 1.
        const uint64_t frames = (delivered + channels - 1) / channels + 1;
        gate = {start, delivered, channels, start + SportClkinForFrames(frames, guestClock_.clkinHz, guestClock_.frameHz)};
    }
    return guestClock_.clkinTicks() >= gate.dueClkin;
}

SportConfig2 DecodeSportConfig2(u32 value) {
    return SportConfig2{static_cast<int>(value & 0x1Fu) + 1, (value & 0x100u) != 0, (value & 0x200u) != 0,
                        (value & 0x400u) != 0};
}

SportConfig2 SPORT::TransmitConfig2() const {
    return SportConfig2{transmitWordLength, transmitSecondaryEnabled, transmitStereoFrameSync, transmitRightStereoOrderFirst};
}

SportConfig2 SPORT::ReceiveConfig2() const {
    return SportConfig2{receiveWordLength, receiveSecondaryEnabled, receiveStereoFrameSync, receiveRightStereoOrderFirst};
}
