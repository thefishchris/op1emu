// SPORT_TCR2 / SPORT_RCR2 frame-setting decoding against the BF52x HRM
// register figures 24-26 and 24-28 (bit 8 TXSE/RXSE, bit 9 TSFSE/RSFSE,
// bit 10 TRFST/RRFST, SLEN[4:0] = word length - 1).
#include "cpu/sport.h"
#include <cstdio>
#include <cstdlib>

namespace {
void Check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "sport config: %s\n", what);
        std::exit(1);
    }
}

bool Same(const SportConfig2& a, const SportConfig2& b) {
    return a.wordBits == b.wordBits && a.secondary == b.secondary && a.stereo == b.stereo && a.rightFirst == b.rightFirst;
}

void DocumentedBits() {
    const auto secondary = DecodeSportConfig2(0x100 | 15);
    Check(secondary.secondary && !secondary.stereo && !secondary.rightFirst && secondary.Channels() == 1,
          "bit 8 alone is the secondary-side enable; the stream stays mono");
    const auto stereo = DecodeSportConfig2(0x200 | 15);
    Check(stereo.stereo && !stereo.secondary && !stereo.rightFirst && stereo.Channels() == 2,
          "bit 9 alone makes the frame sync the L/R clock: two words per frame");
    const auto right = DecodeSportConfig2(0x400 | 15);
    Check(right.rightFirst && !right.stereo && !right.secondary, "bit 10 alone selects right channel first");
    const auto mono = DecodeSportConfig2(15);
    Check(mono.wordBits == 16 && !mono.stereo && !mono.secondary && mono.Channels() == 1, "plain 16-bit mono");
    const auto op1 = DecodeSportConfig2(0x021F);
    Check(op1.wordBits == 32 && op1.stereo && !op1.secondary && !op1.rightFirst && op1.Channels() == 2,
          "0x021F: 32-bit words, stereo L/R frame sync, primary side only, left first");
}

void RegistersMatchDecoder() {
    SPORT sport(0xFFC00800, 0);
    for (u32 value : {0x000Fu, 0x010Fu, 0x020Fu, 0x040Fu, 0x021Fu, 0x0717u}) {
        sport.Write32(0x04, value);
        sport.Write32(0x24, value);
        Check(Same(sport.TransmitConfig2(), DecodeSportConfig2(value)), "TCR2 fields decode as documented");
        Check(Same(sport.ReceiveConfig2(), DecodeSportConfig2(value)), "RCR2 fields decode as documented");
        Check(sport.Read32(0x04) == value && sport.Read32(0x24) == value, "TCR2/RCR2 read back unchanged");
    }
}
}

int main() {
    DocumentedBits();
    RegistersMatchDecoder();
    std::puts("sport config tests passed");
    return 0;
}
