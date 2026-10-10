// USB inactive fast path (USB::ProcessTransfer): equivalence with the
// always-walk path (every interrupt-line update, register value and host
// callback), host requests posted from another thread never lost, and no
// deadlock under repeated host/guest activity. Synchronisation is by atomics
// only; there are no sleeps.
#include "cpu/usb.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace {
void Check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "usb fast path: %s\n", what);
        std::exit(1);
    }
}

// Register offsets (BF52x USB, as modelled in src/cpu/usb.cpp).
constexpr u32 INTRTX = 0x08, INTRRX = 0x0C, INTRTXE = 0x10, INTRRXE = 0x14, GLOBAL_CTL = 0x30;
constexpr u32 EP0_FIFO = 0x80, EP1_FIFO = 0x88, CSR0 = 0x204;
constexpr u32 EP1_TXMAXP = 0x240, EP1_TXCSR = 0x244, EP1_RXCSR = 0x24C, EP1_TXCOUNT = 0x268;
constexpr int INT0 = 52, INT1 = 53, INT2 = 54, DMAINT = 55;

class TestUsb : public USB {
public:
    TestUsb() : USB(0xFFC03800) {}
};

// Records every interrupt-line update and every host callback.
struct Side {
    TestUsb usb;
    std::vector<std::tuple<int, int>> lines;
    std::vector<std::string> callbacks;
    explicit Side(bool fastPath) {
        usb.SetInactiveFastPath(fastPath);
        usb.BindInterrupt(INT0, INT1, INT2, DMAINT, [this](int pin, int level) { lines.emplace_back(pin, level); });
    }
    std::vector<u32> Snapshot() {
        std::vector<u32> values;
        // Every non-FIFO register (FIFO reads pop data).
        for (u32 offset : {0x00u, 0x04u, 0x08u, 0x0Cu, 0x10u, 0x14u, 0x18u, 0x1Cu, 0x20u, 0x24u, 0x2Cu, 0x30u, 0x400u})
            values.push_back(usb.Read32(offset));
        for (u32 ep = 0; ep < 8; ++ep)
            for (u32 reg : {0x00u, 0x04u, 0x08u, 0x0Cu, 0x10u, 0x14u, 0x18u, 0x1Cu, 0x20u, 0x28u})
                values.push_back(usb.Read32(0x200 + ep * 0x40 + reg));
        return values;
    }
};

// Applies one scripted step to both sides and compares everything observable.
struct Pair {
    Side fast{true}, slow{false};
    template <typename Step> void Do(Step step, const char* what) {
        step(fast);
        step(slow);
        Check(fast.lines == slow.lines, what);
        Check(fast.callbacks == slow.callbacks, what);
        Check(fast.Snapshot() == slow.Snapshot(), what);
    }
    void Poll(unsigned n, const char* what) {
        Do([n](Side& s) { for (unsigned i = 0; i < n; ++i) s.usb.ProcessWithInterrupt(0); }, what);
    }
};

void ScriptedEquivalence() {
    Pair p;
    p.Poll(5, "GLOBAL_ENA off: no line updates on either path");
    Check(p.fast.lines.empty() && p.fast.usb.Stats().calls == 0, "disabled USB returns before any work");

    p.Do([](Side& s) { s.usb.Write32(GLOBAL_CTL, 0x0001 | (1u << 1) | (1u << 8)); }, "enable USB, TX/RX EP1");
    p.Poll(1000, "idle polling after enable");
    {
        const auto stats = p.fast.usb.Stats();
        Check(stats.calls == 1000 && stats.walks == 1 && stats.fastPath == 999,
              "first poll after the enable walks; the rest take the fast path");
        const auto slow = p.slow.usb.Stats();
        Check(slow.walks == 1000 && slow.fastPath == 0, "reference path always walks");
        Check(p.fast.lines.size() == 3000, "every fast-path poll still drives the three lines");
    }

    // Host control-OUT SETUP (no data stage), serviced by the guest.
    p.Do([](Side& s) {
        USBSetupBytes setup{};
        setup.bRequest = 5;
        s.usb.HandleSetupPacket(setup, nullptr, 0, [&s](const u8*, std::size_t n) { s.callbacks.push_back("setup " + std::to_string(n)); });
    }, "host SETUP raises EP0 interrupt identically");
    p.Poll(10, "poll with SETUP pending");
    p.Do([](Side& s) {
        for (int i = 0; i < 4; ++i) s.usb.Read32(EP0_FIFO);
        s.usb.Write32(CSR0, 0x40); // SERVICED_RXPKTRDY
        s.usb.Write32(INTRTX, 1);  // W1C EP0
        s.usb.Write32(CSR0, 0x08); // DATAEND
    }, "guest services SETUP");
    p.Poll(10, "SETUP completes through DATAEND");
    Check(p.fast.callbacks.size() == 1, "SETUP callback delivered once");

    // Host OUT data on EP1, guest reads it and clears RXPKTRDY: rxCallback.
    p.Do([](Side& s) {
        const u8 data[6] = {1, 2, 3, 4, 5, 6};
        s.usb.HandleDataWrite(1, 0, data, sizeof(data), [&s] { s.callbacks.push_back("out done"); });
    }, "host OUT data");
    p.Poll(10, "RXPKTRDY pending");
    p.Do([](Side& s) {
        for (int i = 0; i < 3; ++i) s.usb.Read32(EP1_FIFO);
        s.usb.Write32(EP1_RXCSR, 0);
        s.usb.Write32(INTRRX, 2);
    }, "guest drains EP1 OUT");
    p.Poll(10, "OUT completion");
    Check(p.fast.callbacks.back() == "out done", "OUT callback delivered");

    // Guest prepares IN data first; the host request arrives later with no
    // further guest register write (the case a stale fast path would lose).
    p.Do([](Side& s) {
        s.usb.Write32(EP1_TXMAXP, 4);
        s.usb.Write32(EP1_TXCOUNT, 4);
        s.usb.Write32(EP1_FIFO, 0x2211);
        s.usb.Write32(EP1_FIFO, 0x4433);
        s.usb.Write32(EP1_TXCSR, 1); // TXPKTRDY
    }, "guest loads IN packet");
    p.Poll(50, "IN packet waiting for the host");
    p.Do([](Side& s) {
        s.usb.HandleDataRead(1, 0, 4, [&s](const u8* d, std::size_t n) {
            s.callbacks.push_back("in " + std::to_string(n) + " " + std::to_string(d[0]) + std::to_string(d[3]));
        });
    }, "host IN request");
    p.Poll(10, "IN request served");
    Check(p.fast.callbacks.back() == "in 4 1768", "IN callback delivered with the guest's data");

    // Interrupt masks, routing and W1C change the line levels.
    p.Do([](Side& s) { s.usb.Write32(INTRTXE, 0); s.usb.Write32(INTRRXE, 0); }, "mask endpoint interrupts");
    p.Poll(20, "masked");
    p.Do([](Side& s) { s.usb.Write32(INTRTXE, 0xFF); s.usb.Write32(INTRRXE, 0xFF); }, "unmask endpoint interrupts");
    p.Poll(20, "unmasked: pending flags reassert the lines");
    p.Do([](Side& s) { s.usb.Write32(INTRTX, 0xFF); s.usb.Write32(INTRRX, 0xFF); }, "W1C all endpoint flags");
    p.Poll(20, "lines deasserted");

    // Endpoint disable / global disable / re-enable.
    p.Do([](Side& s) { s.usb.Write32(GLOBAL_CTL, 0x0001); }, "disable EP1");
    p.Poll(20, "EP1 disabled");
    p.Do([](Side& s) { s.usb.Write32(GLOBAL_CTL, 0x0000); }, "global disable");
    p.Poll(20, "globally disabled: no line updates");
    p.Do([](Side& s) {
        const u8 data[2] = {9, 9};
        s.usb.HandleDataWrite(1, 0, data, sizeof(data), [&s] { s.callbacks.push_back("late out done"); });
    }, "host data while globally disabled");
    p.Do([](Side& s) { s.usb.Write32(GLOBAL_CTL, 0x0001 | (1u << 1) | (1u << 8)); }, "re-enable");
    p.Poll(200, "re-enabled with host data waiting");
    const auto stats = p.fast.usb.Stats();
    Check(stats.fastPath > 0 && stats.walks < stats.calls, "fast path used through the whole script");
    std::printf("usb scripted equivalence: %zu line updates identical; fast path %llu of %llu polls\n",
                p.fast.lines.size(), static_cast<unsigned long long>(stats.fastPath),
                static_cast<unsigned long long>(stats.calls));
}

// Guest thread polls continuously; host thread posts requests at arbitrary
// moments relative to the fast-path check. Each request needs no further guest
// register write to complete, so a lost wake-up would hang the round (bounded
// by a spin limit, then fail). Guest register accesses happen only while the
// host thread is waiting, mirroring the emulator, where host and guest share
// state only under the USB mutex.
void HostRequestsAreNeverLost() {
    TestUsb usb;
    std::atomic<int> lines{0};
    usb.BindInterrupt(INT0, INT1, INT2, DMAINT, [&](int, int) { lines.fetch_add(1, std::memory_order_relaxed); });
    usb.Write32(GLOBAL_CTL, 0x0001 | (1u << 1) | (1u << 8));
    usb.Write32(EP1_TXMAXP, 4);
    constexpr int kRounds = 20000;
    std::atomic<int> ready{0};   // Round the guest has prepared.
    std::atomic<int> served{0};  // Rounds the host saw completed.
    std::atomic<bool> failed{false};
    std::thread host([&] {
        for (int round = 1; round <= kRounds && !failed.load(); ++round) {
            for (unsigned long spin = 0; ready.load(std::memory_order_acquire) != round; ++spin)
                if (spin > 4000000000ul) { failed = true; return; }
            std::atomic<bool> done{false};
            usb.HandleDataRead(1, 0, 4, [&, round](const u8* d, std::size_t n) {
                if (n != 4 || d[0] != static_cast<u8>(round) || d[2] != static_cast<u8>(round >> 8)) failed = true;
                done.store(true, std::memory_order_release);
            });
            for (unsigned long spin = 0; !done.load(std::memory_order_acquire); ++spin)
                if (spin > 4000000000ul) { failed = true; return; }
            served.store(round, std::memory_order_release);
        }
    });
    for (int round = 1; round <= kRounds && !failed.load(); ++round) {
        // Host is waiting for `ready`: guest register access is exclusive.
        usb.Write32(EP1_TXCOUNT, 4);
        usb.Write32(EP1_FIFO, static_cast<u32>(round & 0xFF) | 0x5500u);
        usb.Write32(EP1_FIFO, 0x6600u | static_cast<u32>((round >> 8) & 0xFF));
        usb.Write32(EP1_TXCSR, 1);
        for (int i = 0; i < 3; ++i) usb.ProcessWithInterrupt(0); // Settle into the fast path.
        ready.store(round, std::memory_order_release);
        // Only polling from here on: the host request must be noticed by itself.
        for (unsigned long spin = 0; served.load(std::memory_order_acquire) != round; ++spin) {
            usb.ProcessWithInterrupt(0);
            if (spin > 400000000ul) { failed = true; break; }
        }
        usb.Write32(INTRTX, 0xFF);
    }
    host.join();
    Check(!failed.load() && served.load() == kRounds, "every host IN request completed without a guest register write");
    const auto stats = usb.Stats();
    Check(stats.fastPath > static_cast<uint64_t>(kRounds), "the guest was in the fast path while requests arrived");
    std::printf("usb host/guest stress: %d rounds, %llu polls, %llu fast-path, %llu walks, %d line updates\n", kRounds,
                static_cast<unsigned long long>(stats.calls), static_cast<unsigned long long>(stats.fastPath),
                static_cast<unsigned long long>(stats.walks), lines.load());
}

// Host OUT data posted from another thread while the guest is in the fast
// path: the guest must see RXPKTRDY and the interrupt line through polling.
void HostOutDataIsSeen() {
    TestUsb usb;
    std::atomic<int> int1High{0};
    const auto guest = std::this_thread::get_id();
    // Count only line updates made by guest polls, not by the host request itself.
    usb.BindInterrupt(INT0, INT1, INT2, DMAINT, [&](int pin, int level) {
        if (pin == INT1 && level && std::this_thread::get_id() == guest) int1High.fetch_add(1, std::memory_order_relaxed);
    });
    usb.Write32(GLOBAL_CTL, 0x0001 | (1u << 1) | (1u << 8));
    constexpr int kRounds = 5000;
    std::atomic<int> posted{0}, completed{0};
    std::atomic<bool> failed{false};
    std::thread host([&] {
        for (int round = 1; round <= kRounds && !failed.load(); ++round) {
            for (unsigned long spin = 0; completed.load(std::memory_order_acquire) != round - 1; ++spin)
                if (spin > 4000000000ul) { failed = true; return; }
            const u8 data[2] = {static_cast<u8>(round), 0x77};
            usb.HandleDataWrite(1, 0, data, sizeof(data), [&, round] { completed.store(round, std::memory_order_release); });
            posted.store(round, std::memory_order_release);
        }
    });
    for (int round = 1; round <= kRounds && !failed.load(); ++round) {
        // Poll until this round's RX interrupt line is seen high from a guest poll.
        unsigned long spin = 0;
        for (;; ++spin) {
            const int before = int1High.load(std::memory_order_relaxed);
            usb.ProcessWithInterrupt(0);
            if (posted.load(std::memory_order_acquire) == round && int1High.load(std::memory_order_relaxed) > before) break;
            if (spin > 400000000ul) { failed = true; break; }
        }
        if (failed) break;
        // Host is now waiting for `completed`: exclusive guest access.
        if (!(usb.Read32(EP1_RXCSR) & 1) || (usb.Read32(EP1_FIFO) & 0xFF) != static_cast<u32>(round & 0xFF)) failed = true;
        usb.Write32(EP1_RXCSR, 0);
        usb.Write32(INTRRX, 0xFF);
        for (unsigned long s = 0; completed.load(std::memory_order_acquire) != round; ++s) {
            usb.ProcessWithInterrupt(0);
            if (s > 400000000ul) { failed = true; break; }
        }
    }
    host.join();
    Check(!failed.load() && completed.load() == kRounds, "every host OUT packet raised RXPKTRDY and completed");
}
}

int main() {
    ScriptedEquivalence();
    HostRequestsAreNeverLost();
    HostOutDataIsSeen();
    std::puts("usb fast path tests passed");
    return 0;
}
