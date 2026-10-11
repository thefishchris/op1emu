#include "core.h"
#include "cpu/cpu.h"
#include "cpu/nand.h"
#include "cpu/sic.h"
#include "cpu/pll.h"
#include "peripheral/MT29F4G08.h"
#include "cec.h"
#include "evt.h"
#include "shadow_timing.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace {
void Check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "execution accounting: %s\n", what);
        std::exit(1);
    }
}

class TestMemory final : public Memory {
public:
    uint32_t base() const override { return 0; }
    uint32_t size() const override { return static_cast<uint32_t>(data.size()); }
    uint8_t read8(uint32_t address) const override { return data.at(address); }
    uint16_t read16(uint32_t address) const override {
        return static_cast<uint16_t>(read8(address) | (uint16_t{read8(address + 1)} << 8));
    }
    uint32_t read32(uint32_t address) const override {
        return read16(address) | (uint32_t{read16(address + 2)} << 16);
    }
    void write8(uint32_t address, uint8_t value) override { data.at(address) = value; }
    void write16(uint32_t address, uint16_t value) override {
        write8(address, static_cast<uint8_t>(value));
        write8(address + 1, static_cast<uint8_t>(value >> 8));
    }
    void write32(uint32_t address, uint32_t value) override {
        write16(address, static_cast<uint16_t>(value));
        write16(address + 2, static_cast<uint16_t>(value >> 16));
    }
    const uint8_t* raw() const override { return data.data(); }
    uintptr_t fast_base() const override { return reinterpret_cast<uintptr_t>(data.data()); }
    uint32_t rawmem_limit() const override { return 0; }
    std::array<uint8_t, 4096> data{};
};

struct Fixture {
    CpuState cpu{};
    TestMemory memory;
    ShadowTiming timing;
    Core core{&cpu, &memory};
    explicit Fixture(bool shadow = false) {
        cpu_state_init(&cpu);
        if (shadow) cpu.shadow_timing = &timing;
        cpu.dpregs[8] = 0x200; // P0, default terminal branch target.
        cpu.dpregs[14] = 0xF00;
        cpu.ksp = 0xF00;
        cpu.usp = 0xE00;
        Check(core.init(2), "JIT init");
    }
    void Run(uint32_t pc) { Check(core.run(pc), "JIT run"); }
    void UserMode() {
        memory.write16(0x80, 0x0011); // RTI out of reset context.
        cpu.reti = 0x100;
        Run(0x80);
        Check(cec_current_ivg() == -1 && cpu.packet_entries == 1, "reset RTI entry");
        cpu.did_jump = false;
    }
};

void SequentialAndCache() {
    Fixture f;
    f.cpu.cycles[0] = 0x12345678;
    f.cpu.cycles[1] = 0x87654321;
    f.cpu.cycles[2] = 0xDEADBEEF;
    f.memory.write16(0x100, 0x0000); // NOP
    f.memory.write16(0x102, 0x0000); // NOP
    f.memory.write16(0x104, 0xE180); // R0 = 7 (Z), a 32-bit instruction.
    f.memory.write16(0x106, 7);
    f.memory.write16(0x108, 0x0050); // JUMP (P0)
    f.memory.write16(0x200, 0x0050);
    f.core.disassemble(0x104);
    Check(f.cpu.packet_entries == 0, "disassembly and code setup are not executed work");
    f.Run(0x100);
    Check(f.cpu.packet_entries == 4 && f.cpu.dpregs[0] == 7 && f.cpu.pc == 0x200,
          "sequential instructions of both widths, not block or byte count");
    for (unsigned i = 0; i < 5; ++i) f.Run(0x200);
    Check(f.cpu.packet_entries == 9, "cached translations count each runtime execution");
    f.core.invalidate();
    Check(f.cpu.packet_entries == 9, "invalidation is not executed work");
    f.Run(0x200);
    Check(f.cpu.packet_entries == 10, "retranslation adds only actual packet entry");
    Check(f.cpu.cycles[0] == 0x12345678 && f.cpu.cycles[1] == 0x87654321 &&
          f.cpu.cycles[2] == 0xDEADBEEF, "work accounting never advances architectural CYCLES");
}

void ConditionalExit() {
    for (uint32_t cc : {0u, 1u}) {
        Fixture f;
        f.cpu.cc = cc;
        f.memory.write16(0x100, 0x1804); // IF CC JUMP 0x108
        f.memory.write16(0x102, 0x0000);
        f.memory.write16(0x104, 0x0000);
        f.memory.write16(0x106, 0x0050);
        f.Run(0x100);
        Check(f.cpu.packet_entries == (cc ? 1u : 4u), "taken branch skips translated suffix");
        Check(f.cpu.pc == (cc ? 0x108u : 0x200u), "conditional branch semantics unchanged");
    }
}

void HardwareLoops() {
    Fixture f;
    f.cpu.lt[0] = 0x100;
    f.cpu.lb[0] = 0x102;
    f.cpu.lc[0] = 3;
    f.memory.write16(0x100, 0x0000);
    f.memory.write16(0x102, 0x0000);
    f.memory.write16(0x104, 0x0050);
    f.Run(0x100);
    Check(f.cpu.packet_entries == 2 && f.cpu.pc == 0x100 && f.cpu.lc[0] == 2,
          "first loopback excludes translated suffix");
    f.Run(0x100);
    Check(f.cpu.packet_entries == 4 && f.cpu.pc == 0x100 && f.cpu.lc[0] == 1,
          "cached hardware loop iteration");
    f.Run(0x100);
    Check(f.cpu.packet_entries == 7 && f.cpu.pc == 0x200 && f.cpu.lc[0] == 0,
          "final loop iteration includes fallthrough");
}

void ParallelPacket() {
    Fixture f;
    f.memory.write16(0x100, 0xC803); // MNOP || NOP || NOP, 64-bit packet.
    f.memory.write16(0x102, 0x1800);
    f.memory.write16(0x104, 0x0000);
    f.memory.write16(0x106, 0x0000);
    f.memory.write16(0x108, 0x0050);
    f.cpu.lt[0] = 0x100;
    f.cpu.lb[0] = 0x100;
    f.cpu.lc[0] = 2;
    f.Run(0x100);
    Check(f.cpu.packet_entries == 1 && f.cpu.pc == 0x100 && f.cpu.lc[0] == 1,
          "three parallel slots count one packet at loop bottom");
    f.Run(0x100);
    Check(f.cpu.packet_entries == 3 && f.cpu.pc == 0x200, "parallel loop fallthrough");
}

void BranchAtLoopBottom() {
    Fixture f;
    f.cpu.cc = 1;
    f.cpu.lt[0] = 0x100;
    f.cpu.lb[0] = 0x100;
    f.cpu.lc[0] = 3;
    f.memory.write16(0x100, 0x1804);
    f.memory.write16(0x102, 0x0050);
    f.Run(0x100);
    Check(f.cpu.packet_entries == 1 && f.cpu.pc == 0x108 && f.cpu.lc[0] == 2,
          "conditional taken-path epilog does not double-count loop-bottom packet");
}

void ParallelLoads() {
    Fixture f;
    f.cpu.iregs[0] = 0x400;
    f.cpu.iregs[1] = 0x500;
    f.memory.write32(0x400, 0x11223344);
    f.memory.write32(0x500, 0x55667788);
    f.memory.write16(0x100, 0xC803); // MNOP || R0 = [I0++] || R1 = [I1++]
    f.memory.write16(0x102, 0x1800);
    f.memory.write16(0x104, 0x9C00);
    f.memory.write16(0x106, 0x9C09);
    f.memory.write16(0x108, 0x0050);
    f.Run(0x100);
    Check(f.cpu.packet_entries == 2 && f.cpu.pc == 0x200 &&
          f.cpu.dpregs[0] == 0x11223344 && f.cpu.dpregs[1] == 0x55667788 &&
          f.cpu.iregs[0] == 0x404 && f.cpu.iregs[1] == 0x504,
          "parallel slot effects execute while packet count increments only once");
}

void PrivilegedEarlyExit() {
    Fixture f;
    f.UserMode();
    evt_set(3, 0x300);
    f.memory.write16(0x100, 0x0138); // USP = [SP++] in user mode: direct early exception return.
    f.memory.write16(0x102, 0x0050); // Translated but never entered.
    f.Run(0x100);
    Check(f.cpu.packet_entries == 2 && f.cpu.pc == 0x300 &&
          (f.cpu.seqstat & 0x3F) == VEC_ILGAL_I, "privileged fault skips suffix and epilog");
}

void EarlyFaultAndRetry() {
    Fixture f;
    f.UserMode();
    evt_set(3, 0x300);
    f.cpu.dpregs[8] = 0x201; // Odd indirect target: returns before hwloop epilog.
    f.memory.write16(0x100, 0x0050);
    f.memory.write16(0x300, 0x0012); // RTX
    f.Run(0x100);
    Check(f.cpu.packet_entries == 2 && f.cpu.pc == 0x300 && f.cpu.retx == 0x100 &&
          (f.cpu.seqstat & 0x3F) == VEC_MISALIGNI, "faulting attempt counts before early return");
    f.Run(0x300);
    Check(f.cpu.packet_entries == 3 && f.cpu.pc == 0x100, "exception return is one packet");
    f.cpu.dpregs[8] = 0x200;
    f.Run(0x100);
    Check(f.cpu.packet_entries == 4 && f.cpu.pc == 0x200, "retried packet counts a new attempt");
}

void ServiceException() {
    Fixture f;
    f.UserMode();
    evt_set(3, 0x300);
    f.memory.write16(0x100, 0x00A5); // EXCPT 5
    f.memory.write16(0x300, 0x0012);
    f.Run(0x100);
    Check(f.cpu.packet_entries == 2 && f.cpu.pc == 0x300 && f.cpu.retx == 0x102,
          "service exception counts instruction, not dispatch");
    f.Run(0x300);
    Check(f.cpu.packet_entries == 3 && f.cpu.pc == 0x102, "service exception resumes after packet");
}

void InterruptEntry() {
    Fixture f;
    f.memory.write16(0x80, 0x0011);
    f.cpu.reti = 0x100;
    evt_set(15, 0x100);
    evt_set(11, 0x300);
    cec_mmr_write(&f.cpu, CEC_MMR_BASE + 4, (1u << 15) | (1u << 11));
    cec_raise(&f.cpu, 15);
    f.Run(0x80); // Return out of reset directly into IVG15.
    Check(f.cpu.packet_entries == 1 && cec_current_ivg() == 15, "reset return and IRQ dispatch");
    f.cpu.did_jump = false;
    cec_push_reti(&f.cpu); // Permit higher-priority interrupt.
    cec_raise(&f.cpu, 11);
    cec_check_pending(&f.cpu);
    Check(f.cpu.packet_entries == 1 && f.cpu.pc == 0x300 && cec_current_ivg() == 11,
          "external interrupt entry is not a fabricated packet");
    f.memory.write16(0x300, 0x0011); // RTI
    f.Run(0x300);
    Check(f.cpu.packet_entries == 2 && f.cpu.pc == 0x100 && cec_current_ivg() == 15,
          "interrupt handler return counts actual handler instruction");
}

void CounterWrap() {
    Fixture f;
    f.memory.write16(0x100, 0x0050);
    f.cpu.packet_entries = std::numeric_limits<uint64_t>::max();
    f.Run(0x100);
    Check(f.cpu.packet_entries == 0 && f.cpu.pc == 0x200, "defined unsigned wrap");
}

// Storage-free platform fixture: tests must never perform NAND operations.
class IdleNand : public NandFlash {
    public:
        void SendCommand(u8) override { Check(false, "unexpected NAND command"); }
        void SendAddress(u8) override { Check(false, "unexpected NAND address"); }
        u8 ReadData() override { Check(false, "unexpected NAND read"); return 0; }
        void WriteData(u8) override { Check(false, "unexpected NAND write"); }
        bool IsDataReady() const override { return false; }
        bool IsBusy() const override { return false; }
        void StartPageRead() override { Check(false, "unexpected page read"); }
        void StartPageWrite() override { Check(false, "unexpected page write"); }
        u32 PageRead(u8*, u32) override { Check(false, "unexpected DMA read"); return 0; }
        u32 PageWrite(const u8*, u32) override { Check(false, "unexpected DMA write"); return 0; }
        void SetReadCallback(ReadCallback) override {}
};

class ResetNand final : public IdleNand {
    public:
        void SendCommand(u8 command) override {
            Check(command == 0xFF, "only NAND reset expected");
            busy = true;
            pending = true;
        }
        bool IsBusy() const override { return busy; }
        bool CompletePendingReset() override {
            if (!pending) return false;
            pending = false;
            busy = false;
            return true;
        }
    private:
        bool busy = false;
        bool pending = false;
};

void NandResetIdleWake() {
    BlackFinCpu cpu;
    cpu.AttachNandFlash(std::make_shared<ResetNand>());
    auto& memory = cpu.GetEmulator();
    memory.MemoryWrite32(0xFFC00124, 0);
    memory.MemoryWrite32(0xFFC00164, 1u << 16); // Only NFC can wake.
    memory.MemoryWrite32(0xFFC0014C, 0); // NFC cannot be delivered to CEC.
    memory.MemoryWrite32(0xFFC0370C, 0x16); // NBUSYIRQ enabled (active-low mask).
    memory.MemoryWrite32(0xFFC03744, 0xFF);
    Check((memory.MemoryRead32(0xFFC03704) & 1) == 0 &&
          (memory.MemoryRead32(0xFFC03708) & 1) == 0,
          "reset drives busy without prematurely latching NBUSYIRQ");

    memory.MemoryWrite16(0x100, 0x0020);
    cpu.SetPC(0x100);
    cpu.Run();
    Check(!cpu.IsIdle() && !cpu.UnsupportedIdle() && cpu.PC() == 0x102 &&
          cpu.PacketEntryCount() == 1 && cpu.IdleEntryCount() == 1,
          "NAND reset completion wakes IDLE without CEC delivery or resume issue");
    Check((memory.MemoryRead32(0xFFC03704) & 1) != 0 &&
          (memory.MemoryRead32(0xFFC03708) & 1) != 0 &&
          (memory.MemoryRead32(0xFFC00160) & (1u << 16)) != 0,
          "ready edge latches NBUSYIRQ and asserts SIC source 48");

    memory.MemoryWrite32(0xFFC03708, 1);
    Check((memory.MemoryRead32(0xFFC03708) & 1) == 0 &&
          (memory.MemoryRead32(0xFFC00160) & (1u << 16)) == 0,
          "NBUSYIRQ W1C deasserts SIC source 48");
}

void NandPageReadIdleWake() {
    const char* imagePath = "/tmp/op1emu-page-read-completion-test.img";
    std::remove(imagePath);
    {
        std::ofstream image(imagePath, std::ios::binary | std::ios::trunc);
        std::array<u8, 2048> page{};
        std::array<u8, 64> oob{};
        page.fill(0xA5);
        oob.fill(0x7C);
        oob[4] = 0xD6;
        oob[5] = 0x4D;
        oob[6] = 0xD1;
        image.write(reinterpret_cast<const char*>(page.data()), page.size());
        image.seekp(536870912);
        image.write(reinterpret_cast<const char*>(oob.data()), oob.size());
        Check(image.good(), "create sparse NAND page-read fixture");
    }

    {
        BlackFinCpu cpu;
        auto flash = std::make_shared<MT29F4G08>(cpu, imagePath);
        cpu.AttachNandFlash(flash);
        auto& memory = cpu.GetEmulator();
        memory.MemoryWrite32(0xFFC00124, 0);
        memory.MemoryWrite32(0xFFC00164, 1u << 16); // Only NFC can wake.
        memory.MemoryWrite32(0xFFC0014C, 0); // NFC cannot be delivered to CEC.
        memory.MemoryWrite32(0xFFC0370C, 0x1E); // Isolate active-low NBUSYIRQ.
        memory.MemoryWrite32(0xFFC03708, 0x1F);

        memory.MemoryWrite32(0xFFC03744, 0x00);
        for (u8 address : std::array<u8, 5>{0x04, 0x08, 0x00, 0x00, 0x00}) {
            memory.MemoryWrite32(0xFFC03740, address);
        }
        memory.MemoryWrite32(0xFFC03744, 0x30);
        Check(flash->IsBusy() && !flash->IsDataReady() &&
              (memory.MemoryRead32(0xFFC03704) & 1) == 0 &&
              (memory.MemoryRead32(0xFFC03708) & 1) == 0,
              "00/address/30 page read enters busy before data is available");

        memory.MemoryWrite16(0x100, 0x0020);
        cpu.SetPC(0x100);
        cpu.Run();
        Check(!cpu.IsIdle() && !cpu.UnsupportedIdle() && cpu.PC() == 0x102 &&
              !flash->IsBusy() && flash->IsDataReady(),
              "page-read completion wakes IDLE once without CEC delivery");
        Check((memory.MemoryRead32(0xFFC03704) & 1) != 0 &&
              (memory.MemoryRead32(0xFFC03708) & 1) != 0 &&
              (memory.MemoryRead32(0xFFC00160) & (1u << 16)) != 0,
              "page-read ready edge latches NBUSYIRQ and SIC source 48");

        memory.MemoryWrite32(0xFFC03724, 1);
        memory.MemoryWrite32(0xFFC03720, 0x155);
        Check(memory.MemoryRead32(0xFFC03720) == 0 &&
              (memory.MemoryRead32(0xFFC03708) & 8) == 0,
              "NFC_COUNT is read-only and page completion does not imply RD_RDY");
        memory.MemoryWrite32(0xFFC0374C, 0);
        constexpr std::array<u32, 3> expectedPrefix{0xD6, 0x4D, 0xD1};
        for (u32 i = 0; i < 62; ++i) {
            Check((memory.MemoryRead32(0xFFC03708) & 8) != 0,
                  "NFC_DATA_RD completion latches RD_RDY");
            if (i + 1 < 62) memory.MemoryWrite32(0xFFC0374C, 0);
            const u32 value = memory.MemoryRead32(0xFFC0372C) & 0xFF;
            const u32 expected = i < expectedPrefix.size() ? expectedPrefix[i] : i < 60 ? 0x7C : 0xFF;
            Check(value == expected && memory.MemoryRead32(0xFFC03720) == i + 1,
                  "PIO reads advance NFC_COUNT through and beyond the page buffer");
            memory.MemoryWrite32(0xFFC03708, 8);
            Check(((memory.MemoryRead32(0xFFC03708) & 8) != 0) == (i + 1 < 62),
                  "RD_RDY W1C exposes one pipelined read completion");
        }

        memory.MemoryWrite32(0xFFC03708, 1);
        memory.MemoryWrite16(0x102, 0x2000);
        cpu.Run();
        Check((memory.MemoryRead32(0xFFC03708) & 1) == 0 &&
              (memory.MemoryRead32(0xFFC00160) & (1u << 16)) == 0,
              "completed read does not relatch NBUSYIRQ without a new read");

        memory.MemoryWrite32(0xFFC03744, 0xFF);
        Check(flash->IsBusy(), "reset remains busy until its completion boundary");
        memory.MemoryWrite16(0x110, 0x0020);
        cpu.SetPC(0x110);
        cpu.Run();
        Check(!cpu.IsIdle() && !flash->IsBusy() &&
              (memory.MemoryRead32(0xFFC03708) & 1) != 0,
              "reset completion remains independent and latches NBUSYIRQ");
        memory.MemoryWrite32(0xFFC03708, 1);

        memory.MemoryWrite16(0x120, 0x0020);
        cpu.SetPC(0x120);
        cpu.Run();
        Check(cpu.IsIdle() && cpu.UnsupportedIdle() &&
              (memory.MemoryRead32(0xFFC03708) & 1) == 0,
              "no pending NAND operation produces no duplicate completion or wake");
    }
    Check(std::remove(imagePath) == 0, "remove NAND page-read fixture");
}

void NandPageReadDmaIdleWake() {
    const char* imagePath = "/tmp/op1emu-page-read-dma-test.img";
    std::remove(imagePath);
    std::array<u8, 2048> page{};
    for (size_t i = 0; i < page.size(); ++i) page[i] = static_cast<u8>(i ^ (i >> 8));
    {
        std::ofstream image(imagePath, std::ios::binary | std::ios::trunc);
        image.write(reinterpret_cast<const char*>(page.data()), page.size());
        image.seekp(536870912);
        const std::array<u8, 64> oob{};
        image.write(reinterpret_cast<const char*>(oob.data()), oob.size());
        Check(image.good(), "create sparse NAND DMA fixture");
    }

    {
        BlackFinCpu cpu;
        auto flash = std::make_shared<MT29F4G08>(cpu, imagePath);
        cpu.AttachNandFlash(flash);
        auto& memory = cpu.GetEmulator();

        memory.MemoryWrite32(0xFFC00124, 1u << 30); // Only DMA2 can wake.
        memory.MemoryWrite32(0xFFC00164, 0);
        memory.MemoryWrite32(0xFFC0010C, 0); // DMA2 cannot be delivered to CEC.
        memory.MemoryWrite32(0xFFC03744, 0x00);
        for (u8 address : std::array<u8, 5>{0, 0, 0, 0, 0}) {
            memory.MemoryWrite32(0xFFC03740, address);
        }
        memory.MemoryWrite32(0xFFC03744, 0x30);
        Check(flash->CompletePendingPageRead(), "complete NAND array read before NFC DMA");

        constexpr u32 destination = 0xFF900100;
        memory.MemoryWrite32(0xFFC00C84, destination);
        memory.MemoryWrite32(0xFFC00C90, 128);
        memory.MemoryWrite32(0xFFC00C94, 2);
        memory.MemoryWrite32(0xFFC00CA8, 1);
        memory.MemoryWrite32(0xFFC00C88, 0x87);
        memory.MemoryWrite8(destination, 0xEE);
        Check(memory.MemoryRead32(0xFFC00C84) == destination &&
              memory.MemoryRead32(0xFFC00C88) == 0x87 &&
              memory.MemoryRead32(0xFFC00C90) == 128 &&
              memory.MemoryRead32(0xFFC00C94) == 2 &&
              memory.MemoryRead32(0xFFC00C98) == 0 &&
              memory.MemoryRead32(0xFFC00C9C) == 0 &&
              memory.MemoryRead32(0xFFC00CAC) == 0x2000,
              "DMA2 is a linear 16-bit NFC-to-memory stop-mode transfer");

        memory.MemoryWrite16(0x120, 0x2000); // Awake packet services ordinary devices.
        cpu.SetPC(0x120);
        cpu.Run();
        Check(memory.MemoryRead8(destination) == 0xEE &&
              memory.MemoryRead32(0xFFC00CB0) == 128 &&
              memory.MemoryRead32(0xFFC00CA8) == 0x08,
              "DMA2 remains blocked before PG_RD_START");

        memory.MemoryWrite32(0xFFC03724, 1);
        memory.MemoryWrite32(0xFFC03728, 1);
        Check(memory.MemoryRead8(destination) == 0xEE &&
              memory.MemoryRead32(0xFFC00CA8) == 0x08,
              "PG_RD_START does not transfer before the device boundary");

        const u32 cecMask = memory.MemoryRead32(CEC_MMR_BASE + 4);
        const u32 cecPending = memory.MemoryRead32(CEC_MMR_BASE + 8);
        const u32 cecLatched = memory.MemoryRead32(CEC_MMR_BASE + 12);
        memory.MemoryWrite16(0x130, 0x0020);
        cpu.SetPC(0x130);
        cpu.Run();
        Check(!cpu.IsIdle() && !cpu.UnsupportedIdle() && cpu.PC() == 0x132,
              "DMA2 completion wakes IDLE without CEC delivery");
        Check(memory.MemoryRead32(CEC_MMR_BASE + 4) == cecMask &&
              memory.MemoryRead32(CEC_MMR_BASE + 8) == cecPending &&
              memory.MemoryRead32(CEC_MMR_BASE + 12) == cecLatched,
              "DMA2 IDLE wake does not require a CEC interrupt");
        std::array<u8, 256> transferred{};
        memory.MemoryRead(destination, transferred.data(), transferred.size());
        Check(std::equal(transferred.begin(), transferred.end(), page.begin()),
              "DMA2 writes the requested NAND bytes to contiguous guest memory");
        Check(memory.MemoryRead32(0xFFC00C90) == 128 &&
              memory.MemoryRead32(0xFFC00C94) == 2 &&
              memory.MemoryRead32(0xFFC00CA4) == destination + 256 &&
              memory.MemoryRead32(0xFFC00CB0) == 0,
              "DMA2 preserves parameters and advances current address/count");
        Check(memory.MemoryRead32(0xFFC00CA8) == 1 &&
              (memory.MemoryRead32(0xFFC00120) & (1u << 30)) != 0,
              "DMA2 DONE asserts SIC source 30 exactly at completion");
        Check(memory.MemoryRead32(0xFFC03720) == 256,
              "NFC counts exactly one 256-byte DMA page segment");

        memory.MemoryWrite32(0xFFC00CA8, 1);
        Check(memory.MemoryRead32(0xFFC00CA8) == 0 &&
              (memory.MemoryRead32(0xFFC00120) & (1u << 30)) == 0,
              "DMA_DONE W1C deasserts SIC source 30");
        memory.MemoryWrite8(destination, 0x5A);
        memory.MemoryWrite16(0x140, 0x2000);
        cpu.SetPC(0x140);
        cpu.Run();
        Check(memory.MemoryRead8(destination) == 0x5A &&
              memory.MemoryRead32(0xFFC03720) == 256 &&
              memory.MemoryRead32(0xFFC00CA8) == 0,
              "completed NFC DMA does not transfer or signal twice");
    }
    Check(std::remove(imagePath) == 0, "remove NAND DMA fixture");
}

#include "nand_random_read_cases.h"
#include "nand_snapshot_cases.h"
#include "mdma_2d_cases.h"
#include "cpu_failure_cases.h"

void MdmaIdleWake() {
    BlackFinCpu cpu;
    cpu.AttachNandFlash(std::make_shared<IdleNand>());
    auto& memory = cpu.GetEmulator();
    constexpr u32 source = 0xFF807FFC;
    constexpr u32 destination = 0x01000000;
    std::vector<u8> expected(65536 * 4);
    constexpr std::array<u8, 4> fill{0x5A, 0xC3, 0x17, 0xE8};
    for (size_t i = 0; i < expected.size(); ++i) expected[i] = fill[i % fill.size()];
    memory.MemoryWrite(source, fill.data(), fill.size());

    memory.MemoryWrite32(0xFFC00124, 0);
    memory.MemoryWrite32(0xFFC00164, 1u << 10); // Only MDMA0 can wake.
    memory.MemoryWrite32(0xFFC0014C, 0); // MDMA0 cannot be delivered to CEC.
    memory.MemoryWrite32(0xFFC00F44, source);
    memory.MemoryWrite32(0xFFC00F50, 0);
    memory.MemoryWrite32(0xFFC00F54, 0);
    memory.MemoryWrite32(0xFFC00F48, 0x09);
    memory.MemoryWrite16(0x80, 0x2000);
    cpu.SetPC(0x80);
    cpu.Run();
    Check((memory.MemoryRead32(0xFFC00F68) & 9) == 8,
          "zero X_COUNT represents 65536 elements and source stalls on its FIFO");
    memory.MemoryWrite32(0xFFC00F04, destination);
    memory.MemoryWrite32(0xFFC00F10, 0);
    memory.MemoryWrite32(0xFFC00F14, 4);
    memory.MemoryWrite32(0xFFC00F28, 3);
    memory.MemoryWrite32(0xFFC00F08, 0x8B);

    memory.MemoryWrite16(0x100, 0x0020);
    cpu.SetPC(0x100);
    cpu.Run();
    std::vector<u8> actual(expected.size());
    memory.MemoryRead(destination, actual.data(), static_cast<int>(actual.size()));
    Check(actual == expected, "finite stop-mode MDMA0 drains the source FIFO");
    Check(!cpu.IsIdle() && !cpu.UnsupportedIdle() && cpu.PC() == 0x102,
          "finite stop-mode MDMA0 completion wakes IDLE");
    Check((memory.MemoryRead32(0xFFC00F28) & 9) == 1 &&
          (memory.MemoryRead32(0xFFC00160) & (1u << 10)) != 0,
          "MDMA0 destination completion sets DONE and SIC source 42");
    memory.MemoryWrite32(0xFFC00F28, 1);
    Check((memory.MemoryRead32(0xFFC00160) & (1u << 10)) == 0,
          "MDMA0 DONE W1C deasserts SIC source 42");
}

void CpuExposure() {
    BlackFinCpu cpu;
    cpu.AttachNandFlash(std::make_shared<IdleNand>());
    Check(cpu.PacketEntryCount() == 0, "CPU exposes zero initial accounting");
    // NOP; JUMP +0. Both execute once; repeated runs execute only the jump.
    const std::array<uint8_t, 4> code{0x00, 0x00, 0x00, 0x20};
    cpu.GetEmulator().MemoryWrite(0x100, code.data(), static_cast<int>(code.size()));
    cpu.SetPC(0x100);
    cpu.Run();
    Check(cpu.PacketEntryCount() == 2 && cpu.PC() == 0x102, "BlackFinCpu sees actual Bcore work");
    cpu.Run();
    Check(cpu.PacketEntryCount() == 3 && cpu.PC() == 0x102, "BlackFinCpu cached-block work");
    cpu.GetEmulator().MemoryWrite16(0x110, 0x0020);
    cpu.GetEmulator().MemoryWrite16(0x112, 0x2000); // JUMP +0, never issued by wake.
    cpu.GetEmulator().MemoryWrite16(0xFFC00004, 5);
    cpu.GetEmulator().MemoryWrite16(0xFFC00000, 0x2000);
    cpu.SetPC(0x110);
    cpu.Run();
    Check(!cpu.IsIdle() && !cpu.UnsupportedIdle() && cpu.PC() == 0x112 &&
          cpu.PacketEntryCount() == 4 && cpu.IdleEntryCount() == 1 &&
          cpu.ClockState().ClkinTicks() == 512 && cpu.ClockState().ActiveCtl() == 0x2000,
          "PLL deadline wakes reset-context CPU with masked CEC and no extra issue");
    cpu.Run();
    Check(cpu.PacketEntryCount() == 5, "post-IDLE packet executes only in subsequent run");
    cpu.GetEmulator().MemoryWrite16(0x120, 0x0020);
    cpu.SetPC(0x120);
    cpu.Run();
    Check(cpu.IsIdle() && cpu.UnsupportedIdle() && cpu.PacketEntryCount() == 6 &&
          cpu.PC() == 0x122 && cpu.IdleEntryCount() == 2, "unknown wake is observable and CPU stays idle");
    for (unsigned i = 0; i < 5; ++i) cpu.Run();
    Check(cpu.PacketEntryCount() == 6 && cpu.IdleEntryCount() == 2, "unsupported idle never retries instruction");
}

void ShadowSequentialAndDisabled() {
    for (bool enabled : {false, true}) {
        Fixture f(enabled);
        f.memory.write16(0x100, 0x0000);
        f.memory.write16(0x102, 0xE180);
        f.memory.write16(0x104, 7);
        f.memory.write16(0x106, 0x0050);
        f.cpu.cycles[0] = 1234;
        f.Run(0x100);
        Check(f.cpu.packet_entries == 3 && f.cpu.pc == 0x200 && f.cpu.dpregs[0] == 7 && f.cpu.cycles[0] == 1234,
              "shadow enable does not change instruction results or CYCLES");
        Check(f.timing.packets == (enabled ? 3u : 0u) && f.timing.EstimatedCycles() == (enabled ? 7u : 0u),
              "one issue per packet plus four branch-loss cycles, or no shadow accounting");
        if (enabled) Check(f.timing.known_issue_packets == 3 && f.timing.unknown_packets == 0, "known issue coverage");
    }
}

void ShadowBranches() {
    for (uint32_t testCC : {0u, 1u}) for (uint32_t predict : {0u, 1u}) for (uint32_t cc : {0u, 1u}) {
        Fixture f(true);
        f.cpu.cc = cc;
        const uint16_t branch = static_cast<uint16_t>(0x1004u | (testCC << 11) | (predict << 10));
        f.memory.write16(0x100, branch);
        f.memory.write16(0x102, 0x0050);
        f.Run(0x100);
        const bool taken = cc == testCC;
        const uint64_t loss = taken == (predict != 0) ? (taken ? 4u : 0u) : 8u;
        Check(f.timing.EstimatedCycles() == 1 + loss + (taken ? 0u : 5u), "runtime branch prediction/outcome timing");
        Check(f.cpu.pc == (taken ? 0x108u : 0x200u) && f.cpu.packet_entries == (taken ? 1u : 2u),
              "branch hints affect shadow loss, never guest outcome");
    }
}

void ShadowMultiple() {
    for (uint32_t registers : {1u, 4u, 8u}) {
        Fixture f(true);
        const uint16_t tail = static_cast<uint16_t>((8u - registers) << 3);
        f.memory.write16(0x100, static_cast<uint16_t>(0x0540u | tail)); // PUSH R7:Rn
        f.memory.write16(0x102, static_cast<uint16_t>(0x0500u | tail)); // POP R7:Rn
        f.memory.write16(0x104, 0x0050);
        const auto sp = f.cpu.dpregs[14];
        f.Run(0x100);
        Check(f.cpu.packet_entries == 3 && f.cpu.dpregs[14] == sp && f.timing.EstimatedCycles() == 2u * registers + 5u,
              "multiple-register decode occupancy includes baseline, not baseline plus N");
        Check(f.timing.unknowns[static_cast<unsigned>(ShadowUnknown::MemoryWait)] == 2, "stack memory waits remain unknown");
    }
    Fixture f(true);
    f.memory.write16(0x100, 0x05C0); // PUSH R7:R0,P5:P0 (14 registers).
    f.memory.write16(0x102, 0x0580);
    f.memory.write16(0x104, 0x0050);
    f.Run(0x100);
    Check(f.timing.EstimatedCycles() == 33, "combined data/pointer register cost");
}

void ShadowParallelAndUnknown() {
    Fixture f(true);
    f.cpu.iregs[0] = 0x400;
    f.cpu.iregs[1] = 0x500;
    f.memory.write16(0x100, 0xC803);
    f.memory.write16(0x102, 0x1800);
    f.memory.write16(0x104, 0x9C00);
    f.memory.write16(0x106, 0x9C09);
    f.memory.write16(0x108, 0x0024); // SSYNC: cost of outstanding work unknown.
    f.memory.write16(0x10A, 0x0050);
    f.Run(0x100);
    Check(f.timing.packets == 3 && f.timing.EstimatedCycles() == 7, "parallel packet counts maximum slot issue, not three issues");
    Check(f.timing.known_issue_packets == 2 && f.timing.unknown_fixed_packets == 1 && f.timing.unknown_packets == 2,
          "known fixed issue, unknown fixed duration and overlapping unknowns are separate");
    Check(f.timing.unknowns[static_cast<unsigned>(ShadowUnknown::MemoryWait)] == 1 &&
          f.timing.unknowns[static_cast<unsigned>(ShadowUnknown::SyncCost)] == 1,
          "parallel memory and synchronization remain uncovered, without invented stall constants");
    Check(f.timing.unknown_opcodes.at("ProgCtrl_SSYNC") == 1, "unsupported opcode histogram is runtime, not translation count");
}

void ShadowHardwareLoops() {
    for (uint32_t top : {2u, 3u}) {
        Fixture f(true);
        f.cpu.dpregs[8] = 3;
        f.cpu.dpregs[9] = 0x200;
        f.memory.write16(0x100, static_cast<uint16_t>(0xE0A0u | top)); // LSETUP LC0=P0
        f.memory.write16(0x102, 4); // Bottom 0x108.
        f.memory.write16(0x10A, 0x0051);
        for (unsigned i = 0; i < 4; ++i) f.Run(i ? f.cpu.pc : 0x100);
        Check(f.cpu.pc == 0x200 && f.cpu.lc[0] == 0, "LSETUP and loop semantics unchanged");
        Check(f.timing.loop_first_penalties == (top == 2 ? 0u : 1u), "non-adjacent first loopback charged only once");
        Check(f.timing.EstimatedCycles() == (top == 2 ? 15u : 17u) && f.timing.loop_replays == 0,
              "LSETUP internal register writes do not incur restore replay penalties");
    }
    Fixture f(true);
    f.memory.write16(0x100, 0x3C00); // LC0=R0: unconditional replay.
    f.memory.write16(0x102, 0x3C08); // LT0=R0: replay only with active LC0.
    f.memory.write16(0x104, 0x0050);
    f.cpu.dpregs[0] = 2;
    f.Run(0x100);
    Check(f.timing.loop_replays == 2 && f.timing.EstimatedCycles() == 27, "LC and active LT restore add ten-cycle replay each");
}

void ShadowEventsAndFaults() {
    Fixture f(true);
    f.UserMode();
    evt_set(3, 0x300);
    f.cpu.dpregs[8] = 0x201;
    f.memory.write16(0x100, 0x0050);
    f.memory.write16(0x300, 0x0012);
    f.Run(0x100);
    Check(f.timing.event_entries == 1 && f.timing.unknowns[static_cast<unsigned>(ShadowUnknown::EventEntry)] == 1 &&
          f.timing.unknowns[static_cast<unsigned>(ShadowUnknown::FaultAttempt)] == 1,
          "fault and dispatched event coverage have no invented event-entry cost");
    const auto before = f.timing.EstimatedCycles();
    f.Run(0x300);
    Check(f.timing.EstimatedCycles() == before + 1 && f.timing.unknowns[static_cast<unsigned>(ShadowUnknown::ReturnCost)] == 2,
          "RTX has only issue baseline and explicit unknown fixed remainder");
}

void ShadowDagAndRtsComponents() {
    Fixture f(true);
    f.cpu.iregs[0] = 0x400;
    f.cpu.rets = 0x200;
    f.memory.write16(0x100, 0x9F60); // I0 += 2
    f.memory.write16(0x102, 0x0010); // RTS
    f.Run(0x100);
    Check(f.cpu.packet_entries == 2 && f.cpu.pc == 0x200 && f.cpu.iregs[0] == 0x402,
          "add-immediate DAG and RTS architectural results");
    Check(f.timing.EstimatedCycles() == 6 && f.timing.issue_cycles == 2 && f.timing.penalty_cycles == 4,
          "DAG issue baseline plus RTS generic redirect component, not summed total tables");
    Check(f.timing.known_issue_packets == 1 && f.timing.unknown_fixed_packets == 1 &&
          f.timing.unknowns[static_cast<unsigned>(ShadowUnknown::ReturnCost)] == 1 &&
          f.timing.unknowns[static_cast<unsigned>(ShadowUnknown::Dependency)] == 2,
          "DAG fixed gap closed but dependencies and complete RTS remainder stay explicit");
}

void ShadowIdleSites() {
    Fixture f(true);
    f.memory.write16(0x100, 0x0020);
    f.memory.write16(0x102, 0x0050);
    for (unsigned i = 0; i < 3; ++i) {
        f.Run(0x100);
        Check(f.cpu.idle && f.cpu.pc == 0x102 && f.cpu.idle_entries == i + 1,
              "IDLE counts once and terminates block before ordinary suffix");
        for (unsigned j = 0; j < 5; ++j) f.Run(0x102);
        Check(f.cpu.packet_entries == i + 1, "no packet attempts while idle");
        f.cpu.idle = false; // Synthetic external wake, not a firmware workaround.
    }
    Check(f.cpu.packet_entries == 3 && f.timing.idle_sites.at(0x100).count == 3,
          "shadow observer counts actual IDLE entries only");
    Check(f.timing.idle_sites.at(0x100).first_packet == 1 && f.timing.idle_sites.at(0x100).last_packet == 3 &&
          f.timing.unknowns[static_cast<unsigned>(ShadowUnknown::IdleTime)] == 3,
          "site counts and unmodelled idle duration stay distinct");
}

void IdlePrivilegeAndInterruptWake() {
    Fixture f;
    f.UserMode();
    evt_set(3, 0x300);
    f.memory.write16(0x100, 0x0020);
    f.Run(0x100);
    Check(!f.cpu.idle && f.cpu.idle_entries == 0 && f.cpu.pc == 0x300,
          "user-mode IDLE faults without entering idle");
    // Exception context is supervisor; a higher priority NMI wakes and vectors.
    f.memory.write16(0x300, 0x0020);
    evt_set(2, 0x400);
    f.Run(0x300);
    Check(f.cpu.idle && f.cpu.pc == 0x302, "supervisor IDLE saves resume PC");
    cec_raise(&f.cpu, 2);
    cec_check_pending(&f.cpu);
    Check(!f.cpu.idle && f.cpu.pc == 0x400 && f.cpu.retn == 0x302,
          "eligible core event wakes normally and preserves post-IDLE return PC");

    Fixture irq;
    irq.memory.write16(0x80, 0x0011);
    irq.memory.write16(0x100, 0x0020);
    irq.cpu.reti = 0x100;
    evt_set(15, 0x100);
    evt_set(11, 0x300);
    cec_mmr_write(&irq.cpu, CEC_MMR_BASE + 4, (1u << 15) | (1u << 11));
    cec_raise(&irq.cpu, 15);
    irq.Run(0x80);
    irq.cpu.did_jump = false;
    cec_push_reti(&irq.cpu);
    irq.Run(0x100);
    Check(irq.cpu.idle && irq.cpu.packet_entries == 2, "IRQ15-context IDLE counted once");
    cec_raise(&irq.cpu, 11);
    cec_check_pending(&irq.cpu);
    Check(!irq.cpu.idle && irq.cpu.pc == 0x300 && irq.cpu.reti == 0x102 &&
          irq.cpu.packet_entries == 2, "eligible maskable interrupt wakes and vectors without synthetic issue");
}

void SameValueVRIntegrationStop() {
    // No peripheral polling occurs: entry/wake and the unsupported-time guard
    // both return before the existing device-processing loop.
    BlackFinCpu cpu;
    cpu.GetEmulator().MemoryWrite16(0x100, 0x0020);
    cpu.GetEmulator().MemoryWrite16(0x102, 0x2000);
    cpu.GetEmulator().MemoryWrite16(0xFFC00008, 0x70B0);
    cpu.SetPC(0x100);
    cpu.Run();
    Check(!cpu.IsIdle() && !cpu.UnsupportedIdle() && cpu.PC() == 0x102 &&
          cpu.PacketEntryCount() == 1 && cpu.IdleEntryCount() == 1 &&
          cpu.ClockState().NextDeadlineTicks() == 512 && cpu.ClockState().Status() == 0xA1,
          "same-value VR wakes immediately but retains outstanding bypass countdown");
    cpu.Run();
    Check(cpu.UnsupportedIdle() && !cpu.IsIdle() && cpu.PacketEntryCount() == 1 &&
          cpu.ClockState().ClkinTicks() == 0 && cpu.ClockState().NextDeadlineTicks() == 512,
          "missing awake CLKIN source is explicit, not guessed from packets or host time");
}

class ScopedEnv {
public:
    ScopedEnv(const char* name, const char* value) : name_(name) {
        if (const char* old = std::getenv(name)) old_ = old;
        Check((value ? setenv(name, value, 1) : unsetenv(name)) == 0, "set test environment");
    }
    ~ScopedEnv() { if (old_) setenv(name_, old_->c_str(), 1); else unsetenv(name_); }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;
private:
    const char* name_;
    std::optional<std::string> old_;
};

void AwakeBypassGuestDeltas() {
    Fixture f(true);
    BF524PLL pll(25000000);
    pll.Write32(8, 0x70B0);
    Check(pll.EnterIdle(), "same VR request starts bypass");
    pll.AcknowledgeWake();
    f.timing.issue_cycles = 10000; // Old observational work must never be re-consumed.
    // Two maximum-length blocks of NOPs, each with 256 documented issue cycles.
    // Consumption uses measured shadow deltas, not their instruction count.
    for (uint32_t pc = 0x100; pc < 0x500; pc += 2) f.memory.write16(pc, 0);
    auto run = [&](uint32_t pc) {
        const bool awake = !f.cpu.idle;
        const auto before = f.timing.EstimatedCycles();
        f.Run(pc);
        return pll.AdvanceAwakeBypass(f.timing.EstimatedCycles() - before, awake, true);
    };
    auto first = run(0x100);
    Check(first.consumed == 256 && !first.expired && pll.ClkinTicks() == 256 &&
          pll.NextDeadlineTicks() == 256, "new actual guest shadow delta only, not old 10000 cycles");
    f.cpu.idle = true;
    const auto noIssue = run(f.cpu.pc);
    Check(noIssue.consumed == 0 && pll.ClkinTicks() == 256 && pll.NextDeadlineTicks() == 256,
          "idle core executes no cycles and awake path consumes no ticks");
    f.cpu.idle = false;
    const auto second = run(f.cpu.pc);
    Check(second.consumed == 256 && second.expired && second.overshoot == 0 &&
          pll.ClkinTicks() == 512 && !pll.NextDeadlineTicks() && !pll.CoreHz(),
          "exactly 512 modeled cycles expire 512 CLKIN ticks; reset active clock remains unknown");
    const auto afterExpiry = run(f.cpu.pc);
    Check(afterExpiry.consumed == 0 && !afterExpiry.expired && pll.ClkinTicks() == 512,
          "functional execution after expiry does not continue clock conversion");

    BF524PLL branchPLL(25000000);
    branchPLL.Write32(8, 0x70B0);
    branchPLL.EnterIdle(); branchPLL.AcknowledgeWake();
    f.memory.write16(0x800, 0x2000); // JUMP +0: issue 1 plus documented redirect component 4.
    uint64_t consumed = 0;
    BF524PLL::AwakeAdvance last;
    for (unsigned i = 0; i < 103; ++i) {
        const auto before = f.timing.EstimatedCycles();
        f.Run(0x800);
        const auto delta = f.timing.EstimatedCycles() - before;
        Check(delta == 5, "guest execution includes branch latency, not just packet count");
        last = branchPLL.AdvanceAwakeBypass(delta, true, true);
        consumed += last.consumed;
    }
    Check(consumed == 512 && branchPLL.ClkinTicks() == 512 && last.consumed == 2 &&
          last.overshoot == 3 && last.expired, "515 block-accounted cycles consume bounded 512 ticks with overshoot 3");
}

void AwakeBypassPlatformExperiment() {
    ScopedEnv clock("OP1EMU_EXPERIMENTAL_CLKIN_HZ", "25000000");
    ScopedEnv shadow("OP1EMU_SHADOW_TIMING", "1");
    ScopedEnv experiment("OP1EMU_EXPERIMENTAL_PLL_AWAKE_BYPASS", "1");
    BlackFinCpu cpu;
    cpu.AttachNandFlash(std::make_shared<IdleNand>());
    cpu.GetEmulator().MemoryWrite16(0x100, 0x0020);
    for (uint32_t pc = 0x102; pc < 0x502; pc += 2) cpu.GetEmulator().MemoryWrite16(pc, 0);
    cpu.GetEmulator().MemoryWrite16(0x502, 0x2000);
    cpu.GetEmulator().MemoryWrite16(0xFFC00008, 0x70B0);
    cpu.SetPC(0x100); cpu.Run();
    Check(cpu.ShadowTimingStats()->EstimatedCycles() == 1 && cpu.ClockState().ClkinTicks() == 0 &&
          cpu.ClockState().NextDeadlineTicks() == 512, "activation block/IDLE issue precedes countdown and is not charged");
    cpu.Run();
    Check(cpu.ClockState().ClkinTicks() == 256 && cpu.AwakePLLExperimentStats().consumed == 256 &&
          !cpu.UnsupportedIdle(), "platform consumes new first block delta");
    cpu.Run();
    const auto& s = cpu.AwakePLLExperimentStats();
    Check(cpu.ClockState().ClkinTicks() == 512 && !cpu.ClockState().CoreHz() &&
          s.consumed == 512 && s.overshoot == 0 && s.blocks == 2 && s.expiries == 1,
          "platform bounded countdown expires exactly once");
    cpu.Run();
    Check(!cpu.UnsupportedIdle() && cpu.ClockState().ClkinTicks() == 512 && s.expiries == 1 &&
          cpu.ShadowTimingStats()->EstimatedCycles() == 518, "post-bypass functional execution continues with unknown active reset clock");
    cpu.GetEmulator().MemoryWrite16(0x600, 0x0020);
    cpu.GetEmulator().MemoryWrite16(0xFFC00004, 5);
    cpu.GetEmulator().MemoryWrite16(0xFFC00000, 0x2000);
    cpu.SetPC(0x600); cpu.Run();
    Check(!cpu.IsIdle() && !cpu.UnsupportedIdle() && cpu.ClockState().ClkinTicks() == 1024 &&
          cpu.ClockState().CoreHz() == 400000000 && cpu.IdleEntryCount() == 2 &&
          s.consumed == 512 && s.expiries == 1, "idle PLL lock advances its separate 512 ticks without re-consuming any shadow cycles");
}

void AwakeBypassOptIns() {
    for (unsigned missing = 0; missing < 3; ++missing) {
        ScopedEnv clock("OP1EMU_EXPERIMENTAL_CLKIN_HZ", missing == 0 ? nullptr : "25000000");
        ScopedEnv shadow("OP1EMU_SHADOW_TIMING", missing == 1 ? "0" : "1");
        ScopedEnv experiment("OP1EMU_EXPERIMENTAL_PLL_AWAKE_BYPASS", missing == 2 ? "0" : "1");
        BlackFinCpu cpu;
        cpu.GetEmulator().MemoryWrite16(0x100, 0x0020);
        cpu.GetEmulator().MemoryWrite16(0xFFC00008, 0x70B0);
        cpu.SetPC(0x100); cpu.Run(); cpu.Run();
        Check(cpu.UnsupportedIdle() && cpu.PacketEntryCount() == 1 &&
              cpu.ClockState().ClkinTicks() == 0 && cpu.AwakePLLExperimentStats().consumed == 0,
              "all three explicit opt-ins required; missing gate preserves observable stop");
    }
}
}

int main() {
    SequentialAndCache();
    ConditionalExit();
    HardwareLoops();
    ParallelPacket();
    BranchAtLoopBottom();
    ParallelLoads();
    PrivilegedEarlyExit();
    EarlyFaultAndRetry();
    ServiceException();
    InterruptEntry();
    CounterWrap();
    NandResetIdleWake();
    NandPageReadIdleWake();
    NandPageReadDmaIdleWake();
    NandRandomReadAvailability();
    NandSnapshotStorage();
    Finite2DMdmaCompletion();
    CpuExecutionFailure();
    MdmaIdleWake();
    CpuExposure();
    ShadowSequentialAndDisabled();
    ShadowBranches();
    ShadowMultiple();
    ShadowParallelAndUnknown();
    ShadowHardwareLoops();
    ShadowEventsAndFaults();
    ShadowDagAndRtsComponents();
    ShadowIdleSites();
    IdlePrivilegeAndInterruptWake();
    SameValueVRIntegrationStop();
    AwakeBypassGuestDeltas();
    AwakeBypassPlatformExperiment();
    AwakeBypassOptIns();
    std::puts("execution accounting: all runtime packet-entry cases passed");
}
