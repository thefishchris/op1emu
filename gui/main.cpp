#include <GLFW/glfw3.h>
#include "loader/ldr.h"
#include "cpu/cpu.h"
#include "peripheral/MT29F4G08.h"
#include "utils/log.h"
#include "glfw_display.h"
#include "audio_output_miniaudio.h"
#include "usbipd.h"
#include "headless.h"
#include <vector>
#include <iostream>
#include <memory>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>

std::atomic<bool> cpuShouldStop(false);

void LdrExecutionThread(BlackFinCpu& cpu, const LDRParser& parser) {
    const auto& dxes = parser.getDXEs();
    // PCs checked between Run() calls: a multi-block batch must stop there.
    for (u32 stop : {0x8000000u, 0xffa06e54u, 0xffa06eecu}) cpu.AddBatchStopPC(stop);
    for (const auto& dxe : dxes) {
        for (const auto& block : dxe.blocks) {
            if (!block.data.empty()) {
                cpu.GetEmulator().MemoryWrite(
                    block.header.target_address,
                    block.data.data(),
                    block.data.size()
                );
            }
            if (block.IsFirstBlock()) {
                cpu.SetRegister(RegIndex::RETS, 0x8000000);
                cpu.SetPC(block.header.target_address);
            }
        }
        LogInfo("Start executing DXE");

        while (!cpuShouldStop.load()) {
            cpu.Run();
            if (cpu.Failed()) { cpuShouldStop.store(true); break; }

            if (cpu.PC() == 0x8000000) {
                LogInfo("Finished executing DXE");
                break;
            }
            if (cpu.PC() == 0xffa06e54) {
                LogInfo("Hit delay(%d)", cpu.GetRegister(RegIndex::R0));
            }
            if (cpu.PC() == 0xffa06eec) {
                LogInfo("Hit delay end");
            }
        }

        if (cpuShouldStop.load()) {
            break;
        }
    }
    LogInfo("CPU thread exiting");
}

void BootExcutionThread(BlackFinCpu& cpu) {
    cpu.SetPC(0xEF000000); // Boot entry point
    while (!cpuShouldStop.load()) {
        cpu.Run();
        if (cpu.Failed()) { cpuShouldStop.store(true); break; }
    }
    LogInfo("CPU thread exiting");
}

int main(int argc, char* argv[]) {
    for (int i = 1; i < argc && std::string(argv[i]) != "--"; ++i)
        if (std::string(argv[i]) == "--headless") return RunHeadlessEmulator(argc, argv);
    bool nandSnapshot = false;
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--nand-snapshot") nandSnapshot = true;
        else if (argument == "--nand-rw") nandSnapshot = false;
        else positional.push_back(argument);
    }
    if (positional.empty() || positional.size() > 2) {
        std::cerr << "Usage: " << argv[0] << " <nand_flash_file> [ldr_file] [--nand-snapshot|--nand-rw]" << std::endl;
        return 1;
    }

    // Create BlackFin CPU. Construction rejects unusable configuration, such
    // as an invalid OP1EMU_OTP_FILE or OP1EMU_BOOTROM_FILE image, with an
    // explanatory exception. This happens before any window is opened.
    std::unique_ptr<BlackFinCpu> cpuStorage;
    try {
        cpuStorage = std::make_unique<BlackFinCpu>();
    } catch (const std::exception& error) {
        LogError("%s", error.what());
        return 1;
    }
    BlackFinCpu& cpu = *cpuStorage;
    // Without an LDR, execution starts at the boot ROM reset vector.
    if (positional.size() == 1 && !cpu.BootRomLoaded()) {
        LogError("No boot ROM image: booting from NAND starts in the boot ROM. Set OP1EMU_BOOTROM_FILE to a "
                 "32768-byte boot ROM image (docs/boot-rom.md)");
        return 1;
    }

    // Create GLFW Display
    auto display = std::make_shared<GLFWDisplay>();
    cpu.AttachDisplay(display);
    cpu.AttachKeyboard(display);
    auto audioOutput = std::make_shared<MiniaudioOutput>();
    cpu.AttachAudioOutput(audioOutput);
    cpu.SetBootMode(0x0D); // Set BMODE to 0b1101, boot from NAND flash with port H

    // Load LDR file
    LDRParser parser;
    if (positional.size() > 1 && !parser.loadFile(positional[1])) {
        std::cerr << "Failed to load LDR file: " << positional[1] << std::endl;
        return 1;
    }

    // Load NAND Flash underlying storage
    std::shared_ptr<MT29F4G08> nandFlash;
    try {
        nandFlash = std::make_shared<MT29F4G08>(cpu, positional[0], nandSnapshot);
    } catch (const std::exception& error) {
        LogError("%s", error.what());
        return 1;
    }
    cpu.AttachNandFlash(nandFlash);
    if (nandSnapshot) LogInfo("NAND snapshot: source read-only; guest changes discarded on exit");

    auto loop = uvw::loop::get_default();
    auto shutdown = loop->resource<uvw::async_handle>();
    shutdown->on<uvw::async_event>([loop](const auto&, auto&) { loop->stop(); });
    USBIPServer usbipd(*loop, cpu.GetUSB());
    usbipd.Start();
    std::thread uvloop([loop]() { while (!cpuShouldStop.load()) loop->run(); });

    // Start CPU execution thread
    std::thread cpuThread;
    if (positional.size() > 1) {
        cpuThread = std::thread(LdrExecutionThread, std::ref(cpu), std::ref(parser));
    } else {
        // If no LDR file is provided, just run the CPU without loading any code
        cpuThread = std::thread(BootExcutionThread, std::ref(cpu));
    }

    // Main thread handles GLFW display
    while (!cpuShouldStop.load() && !display->ShouldClose()) {
        display->PollEvents();
        int16_t ax = static_cast<int16_t>((std::rand() % (540 - 50 + 1)) + 50); // ax in [50, 540]
        int16_t ay = static_cast<int16_t>((std::rand() % (-50 - (-540) + 1)) + (-540)); // ay in [-540, -50]
        int16_t az = static_cast<int16_t>((std::rand() % (874 - 75 + 1)) + 75); // az in [75, 874]
        cpu.SetAcceleration(ax, ay, az); // Placeholder for random accelerometer data
        cpu.SetPotentiometerValue(0xFF - display->GetVolumeValue()); // Update potentiometer (volume) value

        // Small sleep to prevent busy-waiting
        std::this_thread::sleep_for(std::chrono::milliseconds(16)); // ~60 FPS
    }

    // Signal CPU thread to stop and wait for it
    LogInfo("Stopping CPU thread...");
    cpuShouldStop.store(true);
    cpuThread.join();
    shutdown->send();
    uvloop.join();

    return cpu.Failed() ? 1 : 0;
}
