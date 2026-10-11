#include "headless.h"
#include "cpu/cpu.h"
#include "loader/ldr.h"
#include "peripheral/MT29F4G08.h"
#include "utils/log.h"
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <unistd.h>

namespace {
std::atomic<bool> stopRequested{false};
static_assert(std::atomic<bool>::is_always_lock_free);
void Stop(int) { stopRequested.store(true); }
}
int RunHeadlessEmulator(int argc, char** argv) {
    try {
        bool snapshot = true, stopOnIdle = false, options = true, bootRomGiven = false;
        std::string script, config = "gui/ui.json", screenshot, profile, bootRom;
        long limitMs = 0;
        std::vector<std::string> positional;
        auto value = [&](int& i) { if (++i == argc) throw std::runtime_error("Missing option argument"); return std::string(argv[i]); };
        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];
            if (options && argument == "--") options = false;
            else if (options && argument == "--headless") {}
            else if (options && argument == "--nand-snapshot") snapshot = true;
            else if (options && argument == "--nand-rw") snapshot = false;
            else if (options && argument == "--input-script") script = value(i);
            else if (options && argument == "--ui-config") config = value(i);
            else if (options && argument == "--profile") {
                profile = value(i);
                if (profile != "strict" && profile != "functional") throw std::runtime_error("--profile must be strict or functional");
            }
            else if (options && argument == "--boot-rom") { bootRom = value(i); bootRomGiven = true; }
            else if (options && argument == "--screenshot-on-stop") screenshot = value(i);
            else if (options && argument == "--stop-on-unsupported-idle") stopOnIdle = true;
            else if (options && argument == "--max-runtime") {
                const auto text = value(i);
                if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) throw std::runtime_error("Invalid runtime milliseconds");
                limitMs = std::stol(text); if (limitMs < 1 || limitMs > 86400000) throw std::runtime_error("Runtime out of range");
            } else if (options && (argument == "--help" || argument == "-h")) {
                std::cout << "Usage: op1emu-headless NAND [LDR] [--input-script PATH] [--ui-config PATH]\n"
                    "  --profile strict|functional (overrides OP1EMU_PROFILE; see docs/emulator-profiles.md)\n"
                    "  --boot-rom PATH (32768-byte boot ROM image; overrides OP1EMU_BOOTROM_FILE; see docs/boot-rom.md)\n"
                    "  --nand-snapshot (default) | --nand-rw\n  --max-runtime MS | --stop-on-unsupported-idle | --screenshot-on-stop NEW.ppm\n"
                    "Commands: keys; press KEY; release KEY; tap KEY [MS]; wait MS; wait-frames N [MS];\n"
                    "  screenshot NEW.ppm; accel X Y Z; accel random; volume N; quit\n"; return 0;
            } else if (options && argument.rfind("-", 0) == 0) throw std::runtime_error("Unknown option: " + argument);
            else positional.push_back(argument);
        }
        if (positional.empty() || positional.size() > 2) throw std::runtime_error("Expected NAND and optional LDR path");
        const int input = script.empty() ? STDIN_FILENO : open(script.c_str(), O_RDONLY | O_NONBLOCK);
        if (input < 0) throw std::runtime_error("Cannot open input script");
        std::shared_ptr<HeadlessFrontend> frontend;
        try { frontend = std::make_shared<HeadlessFrontend>(config, input); }
        catch (...) { if (!script.empty()) close(input); throw; }
        if (!script.empty()) close(input);
        stopRequested.store(false); std::signal(SIGINT, Stop); std::signal(SIGTERM, Stop);
        // The CLI selection wins over OP1EMU_PROFILE; the CPU resolves it once.
        if (!profile.empty() && setenv("OP1EMU_PROFILE", profile.c_str(), 1) != 0) throw std::runtime_error("Cannot select profile");
        // Likewise --boot-rom wins over OP1EMU_BOOTROM_FILE.
        if (bootRomGiven && setenv("OP1EMU_BOOTROM_FILE", bootRom.c_str(), 1) != 0) throw std::runtime_error("Cannot select boot ROM");
        BlackFinCpu cpu;
        if (positional.size() == 1 && !cpu.BootRomLoaded())
            throw std::runtime_error("No boot ROM image: booting from NAND starts in the boot ROM. Use --boot-rom PATH or "
                                     "OP1EMU_BOOTROM_FILE with a 32768-byte boot ROM image (docs/boot-rom.md)");
        cpu.AttachDisplay(frontend); cpu.AttachKeyboard(frontend); cpu.SetBootMode(0x0D);
        auto nand = std::make_shared<MT29F4G08>(cpu, positional[0], snapshot); cpu.AttachNandFlash(nand);
        LDRParser loader;
        size_t nextDxe = 0;
        if (positional.size() > 1 && !loader.loadFile(positional[1])) throw std::runtime_error("Cannot load LDR");
        auto loadDxe = [&]() {
            if (nextDxe == loader.getDXEs().size()) return false;
            for (const auto& block : loader.getDXEs()[nextDxe++].blocks) {
                if (block.data.size() > static_cast<size_t>(std::numeric_limits<int>::max()) ||
                    block.data.size() > UINT32_MAX - block.header.target_address)
                    throw std::runtime_error("LDR memory-write span exceeds address/length bounds");
                cpu.GetEmulator().MemoryWrite(block.header.target_address, block.data.data(), static_cast<int>(block.data.size()));
                if (block.IsFirstBlock()) { cpu.SetRegister(RETS, 0x08000000); cpu.SetPC(block.header.target_address); }
            }
            return true;
        };
        if (positional.size() > 1) {
            if (!loadDxe()) throw std::runtime_error("Empty LDR");
            cpu.AddBatchStopPC(0x08000000); // The DXE return address is checked between Run() calls.
        }
        else cpu.SetPC(0xEF000000);
        const auto start = HeadlessFrontend::Clock::now(); auto nextPoll = start;
        uint64_t sensorVersion = UINT64_MAX;
        int result = 0;
        while (!stopRequested.load() && !frontend->ShouldClose()) {
            const auto now = HeadlessFrontend::Clock::now();
            if (limitMs && now - start >= std::chrono::milliseconds(limitMs)) { LogError("Headless runtime bound reached"); result = 2; break; }
            if (now >= nextPoll) {
                frontend->PollEvents(now); nextPoll = now + std::chrono::milliseconds(16); // Existing host frontend cadence.
                const auto& sensors = frontend->GetSensors();
                if (!sensors.accelFixed) cpu.SetAcceleration(static_cast<int16_t>(std::rand() % 491 + 50), static_cast<int16_t>(std::rand() % 491 - 540), static_cast<int16_t>(std::rand() % 800 + 75));
                if (sensorVersion != frontend->SensorVersion()) {
                    if (sensors.accelFixed) cpu.SetAcceleration(sensors.ax, sensors.ay, sensors.az);
                    cpu.SetPotentiometerValue(static_cast<u8>(255 - sensors.volume)); sensorVersion = frontend->SensorVersion();
                }
            }
            if (frontend->ShouldClose()) break;
            cpu.Run();
            if (cpu.Failed()) { result = 1; break; }
            if (stopOnIdle && cpu.UnsupportedIdle()) { LogInfo("Headless stopped at unsupported IDLE, PC=%08X", cpu.PC()); break; }
            if (positional.size() > 1 && cpu.PC() == 0x08000000 && !loadDxe()) break;
        }
        if (!screenshot.empty()) frontend->WriteScreenshot(screenshot);
        LogInfo("Headless result=%d PC=%08X packets=%llu frames=%llu snapshot=%u", result, cpu.PC(),
            static_cast<unsigned long long>(cpu.PacketEntryCount()), static_cast<unsigned long long>(frontend->FrameCount()), snapshot);
        return result;
    } catch (const std::exception& error) { LogError("Headless: %s", error.what()); return 1; }
}
