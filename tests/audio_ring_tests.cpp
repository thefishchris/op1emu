#define MA_NO_DEVICE_IO
#define MINIAUDIO_IMPLEMENTATION
#include "audio_ring.h"
#include <array>
#include <cstdio>

int main() {
    ma_pcm_rb ring;
    if (ma_pcm_rb_init(ma_format_f32, 2, 8, nullptr, nullptr, &ring) != MA_SUCCESS) return 1;
    const std::array<float, 12> first = {1,1,2,2,3,3,4,4,5,5,6,6};
    if (WriteAudioFrames(ring, first.data(), 6, 2) != 6) return 1;
    void* buffer = nullptr;
    ma_uint32 count = 5;
    if (ma_pcm_rb_acquire_read(&ring, &count, &buffer) != MA_SUCCESS || count != 5) return 1;
    ma_pcm_rb_commit_read(&ring, 5);
    const std::array<float, 12> second = {7,7,8,8,9,9,10,10,11,11,12,12};
    if (WriteAudioFrames(ring, second.data(), 6, 2) != 6) {
        std::fprintf(stderr, "available space after wraparound was incorrectly dropped\n");
        return 1;
    }
    const std::array<float, 4> more = {13,13,14,14};
    if (WriteAudioFrames(ring, more.data(), 2, 2) != 1) return 1;
    if (WriteAudioFrames(ring, more.data(), 1, 2) != 0) return 1;
    unsigned expected = 6;
    while (expected <= 13) {
        count = 8;
        if (ma_pcm_rb_acquire_read(&ring, &count, &buffer) != MA_SUCCESS || count == 0) return 1;
        auto* samples = static_cast<float*>(buffer);
        for (ma_uint32 i = 0; i < count; ++i) {
            if (samples[i * 2] != static_cast<float>(expected) || samples[i * 2 + 1] != static_cast<float>(expected)) return 1;
            ++expected;
        }
        ma_pcm_rb_commit_read(&ring, count);
    }
    ma_pcm_rb_uninit(&ring);
    return 0;
}
