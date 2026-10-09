// A backend-result seam exercises the production Run() failure path without
// relying on undefined instructions (which are guest exceptions, not JIT failures).
void CpuExecutionFailure() {
    class SpyDevice : public RegisterDevice {
    public:
        SpyDevice() : RegisterDevice("failure-spy", 0xFFC04000, 4) {}
        unsigned calls = 0;
        void ProcessWithInterrupt(int) override { ++calls; }
    };
    class FailureCpu : public BlackFinCpu {
    public:
        unsigned calls = 0;
        void AddSpy(const std::shared_ptr<SpyDevice>& spy) { devices.push_back(spy); }
    protected:
        bool ExecuteCoreBlock(u32) override { ++calls; return false; }
    };
    {
        FailureCpu cpu;
        auto spy = std::make_shared<SpyDevice>(); cpu.AddSpy(spy);
        cpu.AttachNandFlash(std::make_shared<IdleNand>());
        cpu.SetPC(0x100);
        cpu.Run();
        Check(cpu.Failed() && cpu.calls == 1 && spy->calls == 0 && cpu.PC() == 0x100 && cpu.PacketEntryCount() == 0,
              "genuine backend failure stops without pretending to execute or advance PC");
        cpu.Run();
        Check(cpu.calls == 1, "latched execution failure is not retried forever");
    }
    {
        BlackFinCpu cpu;
        cpu.AttachNandFlash(std::make_shared<IdleNand>());
        cpu.GetEmulator().MemoryWrite16(0x100, 0x0020);
        cpu.SetPC(0x100);
        cpu.Run();
        Check(cpu.IsIdle() && cpu.UnsupportedIdle() && !cpu.Failed() && cpu.PacketEntryCount() == 1,
              "architectural IDLE, including unsupported wake, is not a backend execution failure");
    }
}
