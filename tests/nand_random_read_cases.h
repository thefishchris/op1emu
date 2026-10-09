// Included inside execution_accounting_tests.cpp's anonymous namespace.
void NandRandomReadAvailability() {
    const char* path = "/tmp/op1emu-random-read-test.img";
    std::array<u8, 2112> expected{};
    for (size_t i = 0; i < expected.size(); ++i) expected[i] = static_cast<u8>(i ^ (i >> 8));
    for (size_t index = 0; index < 128; ++index) {
        const u16 next = index < 2 ? 0 : index == 2 ? 0xFFFF : static_cast<u16>(index + 1);
        expected[index * 2] = static_cast<u8>(next);
        expected[index * 2 + 1] = static_cast<u8>(next >> 8);
    }
    const std::array<u8, 4> ecc{0xE7, 0x03, 0x18, 0x04};
    std::copy(ecc.begin(), ecc.end(), expected.begin() + 2080);
    {
        std::ofstream image(path, std::ios::binary | std::ios::trunc);
        image.write(reinterpret_cast<const char*>(expected.data()), 2048);
        image.seekp(536870912);
        image.write(reinterpret_cast<const char*>(expected.data() + 2048), 64);
        Check(image.good(), "create random-read data/OOB fixture");
    }
    {
        BlackFinCpu cpu;
        auto flash = std::make_shared<MT29F4G08>(cpu, path);
        cpu.AttachNandFlash(flash);
        auto select = [&]() {
            flash->SendCommand(0);
            for (u8 byte : std::array<u8, 5>{0, 0, 0, 0, 0}) flash->SendAddress(byte);
            flash->SendCommand(0x30);
        };
        auto random = [&](u32 column, unsigned cycles) {
            flash->SendCommand(5);
            for (unsigned i = 0; i < cycles; ++i)
                flash->SendAddress(static_cast<u8>(column >> (8 * (i % 2))));
            Check(!flash->IsDataReady(), "05/address is not a completed output command");
            flash->SendCommand(0xE0);
        };
        std::array<u8, 256> actual{};
        random(0x740, 2);
        Check(!flash->IsDataReady() && flash->PageRead(actual.data(), actual.size()) == 0,
              "random read without a loaded page cannot expose erased cache");
        select();
        random(0x740, 2);
        Check(!flash->IsDataReady(), "random read while array read is busy is rejected");
        Check(flash->CompletePendingPageRead() && !flash->IsDataReady(),
              "later array completion does not validate an earlier rejected random command");
        select();
        Check(flash->IsBusy() && !flash->IsDataReady() && flash->CompletePendingPageRead() &&
              flash->IsDataReady() && !flash->IsBusy(), "ordinary page read remains explicit and deterministic");
        auto& memory = cpu.GetEmulator();
        NFC* controller = nullptr;
        for (auto* device : memory.Devices())
            if (auto* nfcDevice = dynamic_cast<NFC*>(device)) controller = nfcDevice;
        Check(controller != nullptr, "production NFC is registered");
        memory.MemoryWrite32(0xFFC03724, 1);
        memory.MemoryWrite32(0xFFC03728, 1);
        Check(controller->DMARead(0, 0, actual.data(), actual.size()) == actual.size() &&
              memory.MemoryRead32(0xFFC03710) == 0x03E7 &&
              memory.MemoryRead32(0xFFC03714) == 0x0418,
              "FAT-like source bytes calculate the same ECC pair as the stored OOB");
        for (unsigned count : {0u, 1u, 3u}) {
            random(0x740, count);
            Check(!flash->IsDataReady() && flash->PageRead(actual.data(), actual.size()) == 0,
                  "incomplete or excess column cycles cannot become ready");
        }
        flash->SendCommand(0xE0);
        Check(!flash->IsDataReady(), "E0 without preceding 05 cannot become ready");
        random(2112, 2);
        Check(!flash->IsDataReady(), "column beyond page/OOB is rejected");
        random(0x740, 2);
        Check(flash->IsDataReady() && !flash->IsBusy() && !flash->CompletePendingPageRead(),
              "valid random output reuses loaded page without an array operation or host time");
        Check(flash->PageRead(actual.data(), actual.size()) == actual.size() &&
              std::equal(actual.begin(), actual.end(), expected.begin() + 1856),
              "random-read cursor and full data-tail/OOB span match storage");
        Check(std::equal(ecc.begin(), ecc.end(), actual.begin() + 224) && !flash->IsDataReady() &&
              flash->PageRead(actual.data(), 1) == 0, "ECC bytes returned and exhausted cursor is not ready");
        random(2111, 2);
        Check(flash->ReadData() == expected.back() && !flash->IsDataReady(),
              "random output can select the final byte after exhaustion");

        random(0x740, 2);
        memory.MemoryWrite32(0xFFC03724, 1);
        memory.MemoryWrite32(0xFFC03728, 1);
        constexpr u32 destination = 0xFF900100;
        // DMA3 uses the ordinary awake NFC request path, not an IDLE special case.
        memory.MemoryWrite32(0xFFC00CE8, 1);
        memory.MemoryWrite32(0xFFC00CEC, 0x2000);
        memory.MemoryWrite32(0xFFC00CC4, destination);
        memory.MemoryWrite32(0xFFC00CD0, 128);
        memory.MemoryWrite32(0xFFC00CD4, 2);
        memory.MemoryWrite32(0xFFC00CC8, 0x87);
        memory.MemoryWrite32(destination + 224, 0x04F804F8); // Former stale comparison pair.
        memory.MemoryWrite16(0x100, 0x2000);
        cpu.SetPC(0x100);
        cpu.Run();
        memory.MemoryRead(destination, actual.data(), actual.size());
        Check(std::equal(actual.begin(), actual.end(), expected.begin() + 1856) &&
              memory.MemoryRead32(destination + 224) == 0x041803E7 &&
              memory.MemoryRead32(0xFFC00CE8) == 1 && memory.MemoryRead32(0xFFC00CF0) == 0,
              "production DMA3 random read replaces stale ECC scratch with correct bytes");
        random(226, 2);
        Check(flash->ReadData() == 0x72 && flash->ReadData() == 0,
              "source FAT-like entry remains 72 00 across random output operations");
        flash->SendCommand(0xFF);
        Check(flash->CompletePendingReset(), "complete reset independently");
        random(0x740, 2);
        Check(!flash->IsDataReady(), "reset invalidates the loaded page for random read");
    }
    Check(std::remove(path) == 0, "remove random-read fixture");
}
