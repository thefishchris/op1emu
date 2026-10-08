#include "cpu.h"
#include "bcore_memory.h"
#include "ebiu.h"
#include "bootrom.h"
#include "twi.h"
#include "sic.h"
#include "coretimer.h"
#include "pll.h"
#include "otp.h"
#include "gptimer.h"
#include "dma.h"
#include "ppi.h"
#include "gpio.h"
#include "nand.h"
#include "jtag.h"
#include "rtc.h"
#include "usb.h"
#include "sport.h"
#include "emu.h"
#include "peripheral/mcp230xx.h"
#include "peripheral/adxl345.h"
#include "peripheral/oled.h"
#include "peripheral/display.h"
#include "peripheral/keyboard.h"
#include "peripheral/potentiometer.h"
#include "peripheral/audio_output.h"
#include "utils/log.h"

#include "core.h"
#include "cpu_state.h"
#include "shadow_timing.h"
#include "mmr.h"
#include <cstring>
#include <cstdlib>
#include <stdexcept>

// bcore CEC functions (extern "C" in bcore's src/cec.h)
extern "C" void cec_raise(CpuState* cpu, uint32_t ivg);
extern "C" void cec_check_pending(CpuState* cpu);

// bcore EVT init (extern in bcore's src/evt.h)
void evt_init();

static constexpr int IVG_IVTMR = 6;

static constexpr int IRQ_TWI = 20;
static constexpr int IRQ_PORTF_A = 45;
static constexpr int IRQ_PORTF_B = 46;
static constexpr int IRQ_PORTG_A = 40;
static constexpr int IRQ_PORTG_B = 41;
static constexpr int IRQ_PORTH_A = 29;
static constexpr int IRQ_PORTH_B = 31;
static constexpr int IRQ_DMA0 = 15;
static constexpr int IRQ_DMA1 = 28;
static constexpr int IRQ_DMA2 = 30;
static constexpr int IRQ_DMA3 = 16;
static constexpr int IRQ_RTC = 14;
static constexpr int IRQ_NFC = 48;
static constexpr int IRQ_USB_INT0 = 52;
static constexpr int IRQ_USB_INT1 = 53;
static constexpr int IRQ_USB_INT2 = 54;
static constexpr int IRQ_USB_DMAINT = 55;
static constexpr int IRQ_GPTIMER_0 = 32;

// Thin CEC device shim so the Emulator device map covers the CEC MMR range.
// bcore intercepts these internally during JIT execution, but the Emulator
// needs a device registered to avoid "unmapped address" warnings.
// Note: CEC state is process-global (module-level statics in bcore's cec.cpp).
// Only one BlackFinCpu instance may exist per process.
class BcoreCECDevice : public Device {
public:
    BcoreCECDevice(CpuState* cpu)
        : Device("CEC", CEC_MMR_BASE, CEC_MMR_SIZE), cpu_(cpu) {}
    void Read(u32 offset, void* buffer, u32 length) override {
        u32 val = cec_mmr_read(cpu_, baseAddress + offset);
        memcpy(buffer, &val, std::min(length, (u32)sizeof(val)));
    }
    void Write(u32 offset, const void* buffer, u32 length) override {
        u32 val = 0;
        memcpy(&val, buffer, std::min(length, (u32)sizeof(val)));
        cec_mmr_write(cpu_, baseAddress + offset, val);
    }
    u32 Read32(u32 offset) override {
        return cec_mmr_read(cpu_, baseAddress + offset);
    }
    void Write32(u32 offset, u32 value) override {
        cec_mmr_write(cpu_, baseAddress + offset, value);
    }
private:
    CpuState* cpu_;
};

// Thin EVT device shim for the same reason.
class BcoreEVTDevice : public Device {
public:
    BcoreEVTDevice()
        : Device("EVT", EVT_BASE, EVT_SIZE) {}
    void Read(u32 offset, void* buffer, u32 length) override {
        u32 val = evt_read(baseAddress + offset);
        memcpy(buffer, &val, std::min(length, (u32)sizeof(val)));
    }
    void Write(u32 offset, const void* buffer, u32 length) override {
        u32 val = 0;
        memcpy(&val, buffer, std::min(length, (u32)sizeof(val)));
        evt_write(baseAddress + offset, val);
    }
    u32 Read32(u32 offset) override {
        return evt_read(baseAddress + offset);
    }
    void Write32(u32 offset, u32 value) override {
        evt_write(baseAddress + offset, value);
    }
};

BlackFinCpu::BlackFinCpu() : pc(0) {
    cpuState_ = std::make_unique<CpuState>();
    memset(cpuState_.get(), 0, sizeof(CpuState));
    if (const char* value = std::getenv("OP1EMU_SHADOW_TIMING")) {
        if (std::strcmp(value, "1") != 0 && std::strcmp(value, "0") != 0)
            throw std::invalid_argument("OP1EMU_SHADOW_TIMING must be 0 or 1");
        if (std::strcmp(value, "1") == 0) {
            shadowTiming_ = std::make_unique<ShadowTiming>();
            cpuState_->shadow_timing = shadowTiming_.get();
            LogInfo("Shadow timing enabled: observational issue baseline + documented penalties; no device time conversion");
        }
    }

    auto irqHandler = [this](int q, int level) { this->ProcessInterrupt(q, level); };
    uint64_t experimentalClkin = 0;
    if (const char* value = std::getenv("OP1EMU_EXPERIMENTAL_CLKIN_HZ")) {
        // Explicit opt-in only. This is the LIKELY photo-derived experiment,
        // not an unconditional original-OP-1 oscillator specification.
        if (std::strcmp(value, "25000000") != 0)
            throw std::invalid_argument("OP1EMU_EXPERIMENTAL_CLKIN_HZ currently supports only provisional 25000000");
        experimentalClkin = 25000000;
        LogInfo("PLL experimental CLKIN=25000000 Hz (PROVISIONAL/LIKELY board-photo inference; disconnected from devices)");
    }
    pll_ = std::make_shared<BF524PLL>(experimentalClkin);
    if (const char* value = std::getenv("OP1EMU_EXPERIMENTAL_PLL_AWAKE_BYPASS")) {
        if (std::strcmp(value, "1") != 0 && std::strcmp(value, "0") != 0)
            throw std::invalid_argument("OP1EMU_EXPERIMENTAL_PLL_AWAKE_BYPASS must be 0 or 1");
        awakePLLExperiment_ = std::strcmp(value, "1") == 0 && experimentalClkin && shadowTiming_;
        if (awakePLLExperiment_)
            LogInfo("PLL awake-bypass EXPERIMENTAL ideal/no-stall shadow deltas enabled ONLY while temporary CCLK=CLKIN; block-level expiry, no peripheral time conversion");
        else if (std::strcmp(value, "1") == 0)
            LogInfo("PLL awake-bypass experiment disabled: explicit provisional CLKIN and OP1EMU_SHADOW_TIMING=1 both required");
    }
    devices.emplace_back(pll_);
    devices.emplace_back(std::make_shared<MemoryDevice>("L1 SRAM", 0xFFB00000, 0x1000));
    devices.emplace_back(std::make_shared<MemoryDevice>("PORT_MUX", 0xFFC03200, 0x100));
    devices.emplace_back(std::make_shared<MemoryDevice>("Data A",   0xFF800000, 0x4000));
    devices.emplace_back(std::make_shared<MemoryDevice>("Data A Cache", 0xFF804000, 0x4000));
    devices.emplace_back(std::make_shared<MemoryDevice>("Data B",   0xFF900000, 0x4000));
    devices.emplace_back(std::make_shared<MemoryDevice>("Data B Cache", 0xFF904000, 0x4000));
    devices.emplace_back(std::make_shared<MemoryDevice>("Inst A",   0xFFA00000, 0x8000));
    devices.emplace_back(std::make_shared<MemoryDevice>("Inst B",   0xFFA08000, 0x4000));
    devices.emplace_back(std::make_shared<MemoryDevice>("Inst Cache", 0xFFA10000, 0x4000));
    devices.emplace_back(std::make_shared<MemoryDevice>("SDRAM", 0, 0x8000000));

    // CEC/EVT shim devices for emulator device map coverage
    devices.emplace_back(std::make_shared<BcoreEVTDevice>());
    devices.emplace_back(std::make_shared<BcoreCECDevice>(cpuState_.get()));
    // MMU region — register as plain memory (bcore does not use a separate MMU device)
    devices.emplace_back(std::make_shared<MemoryDevice>("MMU", 0xFFE00000, 0x2000));

    devices.emplace_back(std::make_shared<EBIU>(0xFFC00A00));
    devices.emplace_back(std::make_shared<OTP>(0xFFC03600, "otp.bin"));
    std::shared_ptr<USB> usb = std::make_shared<USB>(0xFFC03800);
    usb->BindInterrupt(IRQ_USB_INT0, IRQ_USB_INT1, IRQ_USB_INT2, IRQ_USB_DMAINT, irqHandler);
    devices.emplace_back(usb);
    this->usb = usb;
    sport0 = std::make_shared<SPORT>(0xFFC00800, 0);
    devices.emplace_back(sport0);
    sport1 = std::make_shared<SPORT>(0xFFC00900, 1);
    devices.emplace_back(sport1);
    // OP-1 seems only use last byte of DSPID, which is 0x02 for BF524 rev 02
    devices.emplace_back(std::make_shared<Jtag>(0xFFE05000, 0x02));
    devices.emplace_back(std::make_shared<BootROM>(0xEF000000));
    gptimer = std::make_shared<GPTimer>(0xFFC00600);
    for (int i = 0; i < 8; i++) {
        gptimer->BindInterrupt(i, IRQ_GPTIMER_0 + i, irqHandler);
    }
    devices.emplace_back(gptimer);
    nfc = std::make_shared<NFC>(0xFFC03700);
    nfc->BindInterrupt(IRQ_NFC, irqHandler);
    devices.emplace_back(nfc);

    std::shared_ptr<RTC> rtc = std::make_shared<RTC>(0xFFC00300);
    rtc->BindInterrupt(IRQ_RTC, irqHandler);
    devices.emplace_back(rtc);

    ppi = std::make_shared<PPI>(0xFFC01000);
    devices.emplace_back(ppi);
    std::shared_ptr<DMA> dma = std::make_shared<DMA>(0xFFC00C00, emulator);
    devices.emplace_back(dma);
    dma->AttachDMABus(DMAPeripheralType::DMAPeripheralPPI, ppi);
    dma->AttachDMABus(DMAPeripheralType::DMAPeripheralNFC, nfc);
    dma->AttachDMABus(DMAPeripheralType::DMAPeripheralSPORT0Rx, sport0);
    dma->AttachDMABus(DMAPeripheralType::DMAPeripheralSPORT0Tx, sport0);
    dma->AttachDMABus(DMAPeripheralType::DMAPeripheralSPORT1Rx, sport1);
    dma->AttachDMABus(DMAPeripheralType::DMAPeripheralSPORT1Tx, sport1);
    auto memorySrcBus0 = std::make_shared<MemorySrcDMABus>();
    auto memoryDestBus0 = std::make_shared<MemoryDestDMABus>(*memorySrcBus0);
    dma->AttachDMABus(DMAPeripheralType::DMAPeripheralMDMADest0, memoryDestBus0);
    dma->AttachDMABus(DMAPeripheralType::DMAPeripheralMDMASrc0, memorySrcBus0);
    auto memorySrcBus1 = std::make_shared<MemorySrcDMABus>();
    auto memoryDestBus1 = std::make_shared<MemoryDestDMABus>(*memorySrcBus1);
    dma->AttachDMABus(DMAPeripheralType::DMAPeripheralMDMADest1, memoryDestBus1);
    dma->AttachDMABus(DMAPeripheralType::DMAPeripheralMDMASrc1, memorySrcBus1);
    dma->BindInterrupt(0, IRQ_DMA0, irqHandler);
    dma->BindInterrupt(1, IRQ_DMA1, irqHandler);
    dma->BindInterrupt(2, IRQ_DMA2, irqHandler);
    dma->BindInterrupt(3, IRQ_DMA3, irqHandler);
    std::shared_ptr<GPIO> portF = std::make_shared<GPIO>("PORTF", 0xFFC00700);
    portF->BindInterruptA(IRQ_PORTF_A, irqHandler);
    portF->BindInterruptB(IRQ_PORTF_B, irqHandler);
    devices.emplace_back(portF);
    portG = std::make_shared<GPIO>("PORTG", 0xFFC01500);
    portG->BindInterruptA(IRQ_PORTG_A, irqHandler);
    portG->BindInterruptB(IRQ_PORTG_B, irqHandler);
    devices.emplace_back(portG);
    std::shared_ptr<GPIO> portH = std::make_shared<GPIO>("PORTH", 0xFFC01700);
    portH->BindInterruptA(IRQ_PORTH_A, irqHandler);
    portH->BindInterruptB(IRQ_PORTH_B, irqHandler);
    devices.emplace_back(portH);

    sic = std::make_shared<SIC>(0xFFC00100);
    sic->SetInterruptForwardCallback([this](int ivg, int level) {
        if (level) {
            QueueEvent([this, ivg]() {
                cec_raise(cpuState_.get(), ivg);
            });
        }
    });
    devices.push_back(sic);
    coreTimer = std::make_shared<CoreTimer>(0xFFE03000);
    coreTimer->BindInterrupt(IVG_IVTMR, [this](int ivg, int level) {
        if (level) {
            QueueEvent([this, ivg]() {
                cec_raise(cpuState_.get(), ivg);
            });
        }
    });
    devices.emplace_back(coreTimer);

    std::shared_ptr<TWI> twi = std::make_shared<TWI>(0xFFC01400);
    devices.push_back(twi);

    for (int i = 0; i < 8; i++) {
        u32 addr = 0x20 + i;
        gpioExpanders.push_back(std::make_shared<MCP230XX>(addr, MCP230XXModel::MCP23017));
        twi->AttachPeripheral(gpioExpanders.back());
    }
    // TODO: this bit controls DAT_01f007e0, figure out its function
    gpioExpanders[0]->SetPinInput(6, GPIOPinLevel::High); // Set MCP23017 #6 high
    // Connect gpio expanders
    gpioExpanders[3]->Connect(16, {*gpioExpanders[2], 0}); // Connect gpio3 INTA to gpio2 #0
    gpioExpanders[4]->Connect(16, {*gpioExpanders[2], 1}); // Connect gpio4 INTA to gpio2 #1
    gpioExpanders[6]->Connect(16, {*gpioExpanders[2], 2}); // Connect gpio6 INTA to gpio2 #2
    gpioExpanders[5]->Connect(16, {*gpioExpanders[2], 3}); // Connect gpio5 INTA to gpio2 #3

    gpioOrConnection = std::make_shared<GPIOOrGate>(true); // active low
    gpioOrConnection->Connect(2, {*portG, 0}); // Connect GPIOOr output to portG #0
    gpioExpanders[2]->Connect(16, {*gpioOrConnection, 0}); // Connect gpio2 INTA to GPIOOr #0
    // FIXME: figure out how gpio2 INTA and gpio0 INTA are connected to portG #0
    gpioExpanders[0]->Connect(16, {*gpioOrConnection, 1}); // Connect gpio0 INTA to GPIOOr #1

    adxl345 = std::make_shared<ADXL345>(0x53);
    adxl345->Connect(0, {*gpioExpanders[0], 1}); // Connect ADXL345 INT1 to gpio0 #1

    twi->AttachPeripheral(adxl345);
    // This one byte controls DAT_ff802894, which seams to be "is_not_display_flip"
    twi->AttachPeripheral(std::make_shared<DummyI2CPeripheral>(0x1a, 0x0)); // Dummy I2C device
    twi->AttachPeripheral(std::make_shared<DummyI2CPeripheral>(0x18, 0x0)); // Dummy I2C device
    twi->AttachPeripheral(std::make_shared<DummyI2CPeripheral>(0x58, 0x0)); // Dummy I2C device
    twi->AttachPeripheral(std::make_shared<DummyI2CPeripheral>(0x09, 0x0)); // Dummy I2C device
    twi->AttachPeripheral(std::make_shared<DummyI2CPeripheral>(0x4a, 0x0)); // probably ADC?
    twi->AttachPeripheral(std::make_shared<DummyI2CPeripheral>(0x64, 0x3C)); // LTC2941 battgauge
    twi->AttachPeripheral(std::make_shared<DummyI2CPeripheral>(0x11, 0x80)); // Si4713? FM radio
    // TODO: figure out the exact device of the potentiometer
    potentiometer = std::make_shared<Potentiometer>(0x54);
    twi->AttachPeripheral(potentiometer);
    twi->BindInterrupt(IRQ_TWI, irqHandler);

    for (const auto& device : devices) {
        emulator.BindDevice(device.get());
    }

    std::array<GPIOPeripheral::GPIOConnection, OLED::DATA_PINS> oledDatabus{
        std::tuple<GPIOPeripheral&, int>{*portF, 0},
        std::tuple<GPIOPeripheral&, int>{*portF, 1},
        std::tuple<GPIOPeripheral&, int>{*portF, 2},
        std::tuple<GPIOPeripheral&, int>{*portF, 3},
        std::tuple<GPIOPeripheral&, int>{*portF, 4},
        std::tuple<GPIOPeripheral&, int>{*portF, 5},
        std::tuple<GPIOPeripheral&, int>{*portF, 6},
        std::tuple<GPIOPeripheral&, int>{*portF, 7},
        std::tuple<GPIOPeripheral&, int>{*portF, 8},
        std::tuple<GPIOPeripheral&, int>{*portF, 9},
        std::tuple<GPIOPeripheral&, int>{*portF, 10},
        std::tuple<GPIOPeripheral&, int>{*portF, 11},
        std::tuple<GPIOPeripheral&, int>{*portF, 12},
        std::tuple<GPIOPeripheral&, int>{*portF, 13},
        std::tuple<GPIOPeripheral&, int>{*portF, 14},
        std::tuple<GPIOPeripheral&, int>{*portF, 15},
    };
    std::tuple<GPIOPeripheral&, int> oledWr {*portG, 11};
    std::tuple<GPIOPeripheral&, int> oledCs {*portG, 5};
    std::tuple<GPIOPeripheral&, int> oledRd {*portG, 4};
    std::tuple<GPIOPeripheral&, int> oledRs {*portG, 2};
    oled = std::make_shared<OLED>(oledDatabus, oledCs, oledRs, oledRd, oledWr);

    // Initialize bcore after all devices are bound
    bcoreMemory_ = std::make_unique<EmulatorMemory>(emulator, cpuState_.get());
    core_ = std::make_shared<Core>(cpuState_.get(), bcoreMemory_.get());
    core_->init(2);

    // Initialize bcore CEC and EVT
    cec_init();
    evt_init();

    SetRegister(RegIndex::SP, 0x7000000); // Set stack pointer to top of SDRAM
    SetRegister(RegIndex::FP, 0x7000000); // Set frame pointer to top of SDRAM
    cpuState_->ksp = 0x7000000;
    cpuState_->usp = 0x7000000;
    cpuState_->syscfg = 0x30;

    startTime = std::chrono::system_clock::now();
}

BlackFinCpu::~BlackFinCpu() {
}

void BlackFinCpu::ProcessInterrupt(int pin, int level) {
    sic->SetInterruptLevel(pin, level);
}

void BlackFinCpu::HaltExecution(HaltReason reason) {
    // TODO
}

void BlackFinCpu::SaveContext() {
    // TODO
}

void BlackFinCpu::RestoreContext() {
    // TODO
}

static void SetBfinCycles(CpuState& cpu_state, u64 cycles) {
    cpu_state.cycles[0] = (u32)(cycles & 0xffffffff);
    cpu_state.cycles[1] = (u32)(cycles >> 32);
    cpu_state.cycles[2] = cpu_state.cycles[1];
}

HaltReason BlackFinCpu::Run() {
    if (unsupportedIdle_) return HaltReason::Break;
    if (cpuState_->idle) {
        ServiceIdle();
        return HaltReason::Break; // A wake never issues a packet in this call.
    }
    // Permit only the explicit ideal/no-stall experiment with known CCLK=CLKIN.
    // A missing/unknown ratio is still an observable stop, never a guessed clock.
    const bool advanceAwakeBypass = pll_->CanAdvanceAwakeBypass(awakePLLExperiment_);
    if (pll_->NextDeadlineTicks() && !advanceAwakeBypass) {
        ReportUnsupportedIdle("awake PLL bypass countdown needs a running CLKIN time source");
        return HaltReason::Break;
    }
    auto microSecondsElapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now() - startTime).count();
    auto cyclesElapsed = microSecondsElapsed * 400; // assuming 400MHz CPU clock
    // Sync cycles with system time
    SetBfinCycles(*cpuState_, cyclesElapsed);
    coreTimer->UpdateCycles(cyclesElapsed);

    // Hit entry point, invalidating core to reset cached translations
    if (cpuState_->pc == 0xFFA00000) {
        core_->invalidate();
    }
    // Execute one basic block — bcore updates cpuState_->pc internally.
    // Hardware loops, PC advance, and hwloop counters are all handled by bcore.
    const uint64_t shadowBefore = advanceAwakeBypass ? shadowTiming_->EstimatedCycles() : 0;
    const uint64_t unknownBefore = advanceAwakeBypass ? shadowTiming_->unknown_packets : 0;
    const uint64_t fixedUnknownBefore = advanceAwakeBypass ? shadowTiming_->unknown_fixed_packets : 0;
    const u32 blockPC = cpuState_->pc;
    core_->run(cpuState_->pc);
    if (advanceAwakeBypass) {
        const uint64_t delta = shadowTiming_->EstimatedCycles() - shadowBefore;
        // This call started awake. Its newly issued cycles (including a terminal
        // IDLE's issue) precede any idle-domain advancement in ServiceIdle below.
        // No cycles from before activation, event dispatch, or idle duration enter.
        const auto result = pll_->AdvanceAwakeBypass(delta, true, awakePLLExperiment_);
        if (result.consumed) {
            awakePLLStats_.consumed += result.consumed;
            awakePLLStats_.overshoot += result.overshoot;
            ++awakePLLStats_.blocks;
            if (result.expired) ++awakePLLStats_.expiries;
            if (awakePLLStats_.blocks <= 16 || result.expired)
                LogInfo("pll-awake-bypass block-pc=%08X resume=%08X shadow-before=%llu delta=%llu consumed=%llu remaining=%llu overshoot=%llu total-consumed=%llu expiries=%llu unknown-packets=%llu unknown-fixed=%llu post-cclk-known=%u expired=%u (EXPERIMENTAL ideal/no-stall; intra-block expiry unknown)",
                        blockPC, cpuState_->pc, static_cast<unsigned long long>(shadowBefore),
                        static_cast<unsigned long long>(delta), static_cast<unsigned long long>(result.consumed),
                        static_cast<unsigned long long>(pll_->NextDeadlineTicks().value_or(0)),
                        static_cast<unsigned long long>(result.overshoot), static_cast<unsigned long long>(awakePLLStats_.consumed),
                        static_cast<unsigned long long>(awakePLLStats_.expiries),
                        static_cast<unsigned long long>(shadowTiming_->unknown_packets - unknownBefore),
                        static_cast<unsigned long long>(shadowTiming_->unknown_fixed_packets - fixedUnknownBefore),
                        pll_->CoreHz().has_value() ? 1u : 0u, result.expired ? 1u : 0u);
        }
    }
    cpuState_->did_jump = false; // Clear jump flag set by bcore, since we handle it in the emulator loop
    if (cpuState_->idle) {
        LogInfo("idle-enter pc=%08X resume=%08X packets=%llu entries=%llu host-us=%lld",
                cpuState_->idle_pc, cpuState_->pc,
                static_cast<unsigned long long>(cpuState_->packet_entries),
                static_cast<unsigned long long>(cpuState_->idle_entries),
                static_cast<long long>(microSecondsElapsed));
        pll_->EnterIdle();
        ServiceIdle();
        return HaltReason::Break;
    }
    cec_check_pending(cpuState_.get());

    // Get active IVG from CEC
    int ivg = cec_current_ivg();
    for (const auto& device : devices) {
        device->ProcessWithInterrupt(ivg);
    }
    // FIXME: use correct clock
    if (cyclesElapsed % 10000 == 0) {
        gptimer->Tick(GPTimerClockTypeSCLK);
        gptimer->Tick(GPTimerClockTypeTACLK);
        gptimer->Tick(GPTimerClockTypeTMRCLK);
    }

    ProcessEvents();
    return HaltReason::Break;
}

void BlackFinCpu::SetRegister(int index, u32 value) {
    switch (index)
    {
    case RegIndex::FP:
        cpuState_->dpregs[15] = value;
        break;
    case RegIndex::SP:
        cpuState_->dpregs[14] = value;
        break;
    case RegIndex::RETS:
        cpuState_->rets = value;
        break;
    case RegIndex::R0...RegIndex::R2:
        cpuState_->dpregs[index - RegIndex::R0] = value;
        break;
    case RegIndex::P1:
        cpuState_->dpregs[9] = value;
        break;
    default:
        break;
    }
}

u32 BlackFinCpu::GetRegister(int index) {
    switch (index)
    {
    case RegIndex::FP:
        return cpuState_->dpregs[15];
    case RegIndex::SP:
        return cpuState_->dpregs[14];
    case RegIndex::RETS:
        return cpuState_->rets;
    case RegIndex::R0...RegIndex::R2:
        return cpuState_->dpregs[index - RegIndex::R0];
    case RegIndex::P1:
        return cpuState_->dpregs[9];
    default:
        return 0;
    }
}

void BlackFinCpu::SetPC(u32 value) {
    cpuState_->pc = value;
}

u32 BlackFinCpu::PC() {
    return cpuState_->pc;
}

uint64_t BlackFinCpu::PacketEntryCount() const {
    return cpuState_->packet_entries;
}

bool BlackFinCpu::IsIdle() const { return cpuState_->idle; }
uint64_t BlackFinCpu::IdleEntryCount() const { return cpuState_->idle_entries; }

void BlackFinCpu::ServiceIdle() {
    if (pll_->Unsupported()) { ReportUnsupportedIdle(pll_->Unsupported()); return; }
    if (!pll_->WakeAsserted() && !sic->WakePending()) {
        if (const auto deadline = pll_->NextDeadlineTicks()) {
            pll_->AdvanceClkin(*deadline);
            LogInfo("pll-deadline elapsed-clkin=%llu total-clkin=%llu ctl=%04X active=%04X div=%04X stat=%04X provisional-cclk=%llu provisional-sclk=%llu",
                    static_cast<unsigned long long>(*deadline), static_cast<unsigned long long>(pll_->ClkinTicks()),
                    pll_->Read32(0), pll_->ActiveCtl().value_or(0), pll_->Divider(), pll_->Status(),
                    static_cast<unsigned long long>(pll_->CoreHz().value_or(0)),
                    static_cast<unsigned long long>(pll_->SystemHz().value_or(0)));
        }
    }
    if (pll_->WakeAsserted()) sic->SetInterruptLevel(0, 1); // DPMC SIC source 0.
    if (sic->WakePending()) {
        cpuState_->idle = false;
        LogInfo("idle-wake resume=%08X packets=%llu entries=%llu sic=%X/%X cec=%X/%X/%X",
                cpuState_->pc, static_cast<unsigned long long>(cpuState_->packet_entries),
                static_cast<unsigned long long>(cpuState_->idle_entries), sic->Read32(0x20), sic->Read32(0x60),
                cec_mmr_read(cpuState_.get(), CEC_MMR_BASE + 4),
                cec_mmr_read(cpuState_.get(), CEC_MMR_BASE + 8), cec_mmr_read(cpuState_.get(), CEC_MMR_BASE + 12));
        // Existing interrupt routing is independent of IWR/CEC wake eligibility.
        ProcessEvents();
        cec_check_pending(cpuState_.get());
        pll_->AcknowledgeWake();
        sic->SetInterruptLevel(0, 0);
        return;
    }
    ReportUnsupportedIdle(pll_->WakeAsserted() ? "PLL wake disabled in SIC_IWR0" :
                          "no known eligible wake completion or deterministic deadline");
}

void BlackFinCpu::ReportUnsupportedIdle(const char* reason) {
    unsupportedIdle_ = true;
    LogWarn("idle-unsupported reason=%s idle=%u pc=%08X resume=%08X packets=%llu entries=%llu rets=%08X reti=%08X seqstat=%08X ivg=%d",
            reason, cpuState_->idle ? 1u : 0u, cpuState_->idle_pc, cpuState_->pc,
            static_cast<unsigned long long>(cpuState_->packet_entries),
            static_cast<unsigned long long>(cpuState_->idle_entries), cpuState_->rets, cpuState_->reti,
            cpuState_->seqstat, cec_current_ivg());
    LogWarn("idle-state cec=%X/%X/%X sic-mask=%X/%X sic-isr=%X/%X sic-iwr=%X/%X pll=%X/%X/%X/%X/%X active-vr=%X",
            cec_mmr_read(cpuState_.get(), CEC_MMR_BASE + 4), cec_mmr_read(cpuState_.get(), CEC_MMR_BASE + 8),
            cec_mmr_read(cpuState_.get(), CEC_MMR_BASE + 12), sic->Read32(0x0C), sic->Read32(0x4C),
            sic->Read32(0x20), sic->Read32(0x60), sic->Read32(0x24), sic->Read32(0x64),
            pll_->Read32(0), pll_->Read32(4), pll_->Read32(8), pll_->Status(), pll_->Read32(16), pll_->ActiveVR());
    for (unsigned i = 0; i < 16; ++i)
        LogWarn("idle-register dpreg=%u value=%08X", i, cpuState_->dpregs[i]);
    // Non-destructive control/status readbacks only, never peripheral FIFOs.
    for (u32 address : {0xFFC03600u, 0xFFC03608u, 0xFFC0360Cu,
                        0xFFC03700u, 0xFFC03704u, 0xFFC03708u, 0xFFC0370Cu,
                        0xFFC01414u, 0xFFC01418u, 0xFFC01420u, 0xFFC01424u,
                        0xFFC00C48u, 0xFFC00C68u, 0xFFC00C70u,
                        0xFFC00C88u, 0xFFC00CA8u, 0xFFC00CB0u})
        LogWarn("idle-device address=%08X value=%08X", address, emulator.MemoryRead32(address));
    if (shadowTiming_)
        LogWarn("idle-shadow packets=%llu estimate=%llu idle-entries=%llu (observational only)",
                static_cast<unsigned long long>(shadowTiming_->packets),
                static_cast<unsigned long long>(shadowTiming_->EstimatedCycles()),
                static_cast<unsigned long long>(cpuState_->idle_entries));
    bcoreMemory_->DumpRecentMMIO();
    const u32 pc = cpuState_->idle_pc;
    const bool mappedCode = pc < 0x08000000 || (pc >= 0xEF000020 && pc < 0xEF007FC0) ||
                            (pc >= 0xFFA00020 && pc < 0xFFA0BFC0);
    if (mappedCode && pc >= 32) {
        u32 address = pc - 32;
        for (unsigned i = 0; i < 32 && address <= pc + 32; ++i) {
            auto [text, next] = core_->disassemble(address);
            LogWarn("idle-context address=%08X %s", address, text.c_str());
            if (next <= address || next - address > 8) break;
            address = next;
        }
    }
}

const ShadowTiming* BlackFinCpu::ShadowTimingStats() const {
    return shadowTiming_.get();
}

void BlackFinCpu::QueueEvent(const std::function<void()>& event, std::chrono::nanoseconds delay) {
    std::unique_lock<std::recursive_mutex> lock(eventQueueMutex);
    eventQueue.push_back({delay, event});
}

void BlackFinCpu::ProcessEvents() {
    std::unique_lock<std::recursive_mutex> lock(eventQueueMutex);
    constexpr std::chrono::nanoseconds instructionTime(1); // Approximate instruction execution time
    auto events = eventQueue;
    for (auto& [delay, event] : events) {
        if (delay <= std::chrono::nanoseconds(0)) {
            event();
        }
    }
    // Remove processed events
    auto it = eventQueue.begin();
    while (it != eventQueue.end()) {
        if (std::get<0>(*it) <= std::chrono::nanoseconds(0)) {
            it = eventQueue.erase(it);
        } else {
            std::get<0>(*it) -= instructionTime;
            ++it;
        }
    }
    elapsedTime += instructionTime;
}

void BlackFinCpu::AttachDisplay(const std::shared_ptr<Display>& display) {
    ppi->AttachDisplay(display);
    display->SetOnFrameStartCallback([this](Display& disp) {
        this->QueueEvent([this]() {
            this->portG->SetPinInput(3, GPIOPinLevel::Low);
        });
        this->QueueEvent([this]() {
            this->portG->SetPinInput(3, GPIOPinLevel::High);
        }, std::chrono::nanoseconds(1000));
    });
}

void BlackFinCpu::AttachNandFlash(const std::shared_ptr<NandFlash>& nandFlash) {
    nfc->AttachNandFlash(nandFlash);
}

void BlackFinCpu::AttachKeyboard(const std::shared_ptr<Keyboard>& keyboard) {
    keyboard->SetKeyEventCallback([this](int bank, int index, bool pressed) {
        this->QueueEvent([this, bank, index, pressed]() {
            // Map keyboard events to GPIO expander pins
            gpioExpanders[bank]->SetPinInput(index, pressed ? GPIOPinLevel::Low : GPIOPinLevel::High);
        });
    });
}

void BlackFinCpu::AttachAudioOutput(const std::shared_ptr<AudioOutput>& audioOutput) {
    auto cb = [audioOutput](const void* data, size_t samples, int channels, int bitsPerSample) {
        audioOutput->WriteSamples(data, samples, channels, bitsPerSample);
    };
    sport0->SetAudioOutputCallback(cb);
    sport1->SetAudioOutputCallback(cb);
}

void BlackFinCpu::SetAcceleration(int16_t x, int16_t y, int16_t z) {
    QueueEvent([this, x, y, z]() {
        this->adxl345->SetAcceleration(x, y, z);
    });
}

void BlackFinCpu::SetPotentiometerValue(u8 value) {
    QueueEvent([this, value]() {
        this->potentiometer->SetValue(value);
    });
}

void BlackFinCpu::SetBootMode(int mode) {
    sic->SetBootMode(mode);
}
