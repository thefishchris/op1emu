#pragma once

#include "miniaudio.h"
#include <cstring>

// Acquisition can stop at the physical end of the ring even when there is
// more free space after wraparound. Commit and reacquire until full or done.
inline ma_uint32 WriteAudioFrames(ma_pcm_rb& ring, const float* data, ma_uint32 frames, ma_uint32 channels) {
    ma_uint32 written = 0;
    while (written < frames) {
        void* buffer = nullptr;
        ma_uint32 count = frames - written;
        if (ma_pcm_rb_acquire_write(&ring, &count, &buffer) != MA_SUCCESS || count == 0) break;
        std::memcpy(buffer, data + static_cast<std::size_t>(written) * channels,
                    static_cast<std::size_t>(count) * channels * sizeof(float));
        ma_pcm_rb_commit_write(&ring, count);
        written += count;
    }
    return written;
}
