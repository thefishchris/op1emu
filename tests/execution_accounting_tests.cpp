#include "core.h"
#include "cpu/cpu.h"
#include "cpu/nand.h"
#include "peripheral/MT29F4G08.h"
#include "cec.h"
#include "evt.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
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
    Core core{&cpu, &memory};
    Fixture() {
        cpu_state_init(&cpu);
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
    MdmaIdleWake();
    CpuExposure();
    std::puts("execution accounting: all runtime packet-entry cases passed");
}
