#include "core.h"
#include "cpu/cpu.h"
#include "cpu/nand.h"
#include "cec.h"
#include "evt.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>

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

void CpuExposure() {
    // The CPU polls NFC each block. Supply an idle, storage-free test device;
    // this fixture must never read or write firmware/provisioning images.
    class IdleNand final : public NandFlash {
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
    CpuExposure();
    std::puts("execution accounting: all runtime packet-entry cases passed");
}
