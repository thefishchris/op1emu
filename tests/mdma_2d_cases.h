// Included in execution_accounting_tests.cpp's anonymous namespace.
void Finite2DMdmaCompletion() {
    for (bool prefilled : {false, true}) {
        BlackFinCpu cpu;
        cpu.AttachNandFlash(std::make_shared<IdleNand>());
        auto& memory = cpu.GetEmulator();
        DMA* dmaDevice = nullptr;
        SIC* sicDevice = nullptr;
        for (auto* device : memory.Devices()) {
            if (auto* dma = dynamic_cast<DMA*>(device)) dmaDevice = dma;
            if (auto* sic = dynamic_cast<SIC*>(device)) sicDevice = sic;
        }
        Check(dmaDevice && sicDevice, "production DMA/SIC registered for 2D test");
        unsigned assertions = 0;
        unsigned clears = 0;
        dmaDevice->BindInterrupt(12, 42, [&](int q, int level) {
            Check(q == 42, "MDMA0 destination interrupt uses SIC source 42");
            if (level) ++assertions;
            else ++clears;
            sicDevice->SetInterruptLevel(q, level);
        });
        memory.MemoryWrite32(0xFFC00124, 0);
        memory.MemoryWrite32(0xFFC00164, 0); // No fabricated wake required for completion.
        constexpr u32 source = 0xFF800100;
        constexpr u32 destination = 0x03000100;
        constexpr u32 columns = 198;
        constexpr u32 rows = 50;
        constexpr u32 pitch = 512;
        constexpr u32 span = (rows - 1) * pitch + columns * 2;
        const std::array<u16, 4> pattern{0x1234, 0x5678, 0x9ABC, 0xDEF0};
        for (u32 column = 0; column < pattern.size(); ++column)
            memory.MemoryWrite16(source + column * 2, pattern[column]);
        memory.MemoryWrite16(source + 8, 0xEEFF);
        std::vector<u8> guarded(span + 16, 0xA5);
        memory.MemoryWrite(destination, guarded.data(), static_cast<int>(guarded.size()));
        memory.MemoryWrite32(0xFFC00F44, source);
        memory.MemoryWrite32(0xFFC00F50, 4);
        memory.MemoryWrite32(0xFFC00F54, 2);
        memory.MemoryWrite32(0xFFC00F58, 2475);
        memory.MemoryWrite32(0xFFC00F5C, 0xFFFA); // -6 from the LAST element, not the next one.
        memory.MemoryWrite32(0xFFC00F48, 0x15);
        Check(memory.MemoryRead32(0xFFC00F68) == 8, "source enable alone cannot complete the paired work");
        if (prefilled) {
            dmaDevice->ProcessWithInterrupt(0);
            Check(memory.MemoryRead32(0xFFC00F64) == source &&
                  memory.MemoryRead32(0xFFC00F70) == 4 &&
                  memory.MemoryRead32(0xFFC00F78) == 2475 - 512,
                  "FIFO backpressure preserves source row start/count after 512 repeated rows");
        }
        memory.MemoryWrite32(0xFFC00F04, destination);
        memory.MemoryWrite32(0xFFC00F10, columns);
        memory.MemoryWrite32(0xFFC00F14, 2);
        memory.MemoryWrite32(0xFFC00F18, rows);
        memory.MemoryWrite32(0xFFC00F1C, 118);
        memory.MemoryWrite32(0xFFC00F08, 0x97);
        std::vector<u8> actual(guarded.size());
        memory.MemoryRead(destination, actual.data(), static_cast<int>(actual.size()));
        for (u32 row = 0; row < rows; ++row) {
            for (u32 column = 0; column < columns; ++column) {
                const u16 word = pattern[(row * columns + column) % 4];
                const u32 offset = row * pitch + column * 2;
                Check(actual[offset] == static_cast<u8>(word) &&
                      actual[offset + 1] == static_cast<u8>(word >> 8),
                      "2D copy follows continuous FIFO order across differing row widths");
            }
            const u32 end = row + 1 < rows ? (row + 1) * pitch : static_cast<u32>(actual.size());
            for (u32 offset = row * pitch + columns * 2; offset < end; ++offset)
                Check(actual[offset] == 0xA5, "Y_MODIFY preserves row gaps and final trailing guard");
        }
        Check(memory.MemoryRead32(0xFFC00F64) == source + 8 &&
              memory.MemoryRead32(0xFFC00F24) == destination + span &&
              memory.MemoryRead32(0xFFC00F70) == 0 && memory.MemoryRead32(0xFFC00F30) == 0 &&
              memory.MemoryRead32(0xFFC00F78) == 1 && memory.MemoryRead32(0xFFC00F38) == 1,
              "finite 2D final address is last element plus X_MODIFY; final counts X=0/Y=1");
        Check(memory.MemoryRead32(0xFFC00F50) == 4 && memory.MemoryRead32(0xFFC00F58) == 2475 &&
              memory.MemoryRead32(0xFFC00F10) == columns && memory.MemoryRead32(0xFFC00F18) == rows,
              "2D transfer preserves source and destination programmed geometry");
        Check(memory.MemoryRead32(0xFFC00F28) == 1 && assertions == 1 &&
              (memory.MemoryRead32(0xFFC00160) & (1u << 10)) != 0 && !sicDevice->WakePending(),
              "destination DONE/SIC asserts once even when its wake-enable bit is clear");
        memory.MemoryWrite32(0xFFC00F28, 1);
        Check(clears == 1 && memory.MemoryRead32(0xFFC00F28) == 0 &&
              (memory.MemoryRead32(0xFFC00160) & (1u << 10)) == 0,
              "destination W1C clears DONE and SIC source 42");
        memory.MemoryWrite16(destination, 0xBEEF);
        Check(!dmaDevice->ServiceIdleMDMACompletion(), "completed finite 2D work does not complete twice");
        dmaDevice->ProcessWithInterrupt(0);
        Check(assertions == 1 && memory.MemoryRead16(destination) == 0xBEEF,
              "ordinary DMA service does not repeat the stopped 2D copy");

        // Reject another 2D shape rather than silently completing any two enabled channels.
        memory.MemoryWrite32(0xFFC00F48, 0);
        memory.MemoryWrite32(0xFFC00F08, 0);
        memory.MemoryWrite32(0xFFC00F58, 2475);
        memory.MemoryWrite32(0xFFC00F48, 0x15);
        memory.MemoryWrite32(0xFFC00F18, rows - 1);
        memory.MemoryWrite32(0xFFC00F08, 0x97);
        Check(memory.MemoryRead32(0xFFC00F28) == 8 && !dmaDevice->ServiceIdleMDMACompletion(),
              "mismatched total source/destination counts are outside the supported shape");
        memory.MemoryWrite32(0xFFC00F08, 0);
        memory.MemoryWrite32(0xFFC00F18, 0);
        memory.MemoryWrite32(0xFFC00F08, 0x97);
        Check(!dmaDevice->ServiceIdleMDMACompletion() && memory.MemoryRead32(0xFFC00F28) == 8,
              "unobserved zero-Y shape is not auto-completed");
    }
}
