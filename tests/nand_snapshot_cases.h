// Included within execution_accounting_tests.cpp's anonymous namespace.
void NandSnapshotStorage() {
    class SnapshotCpu : public BlackFinCpu {
    public: void Drain() { ProcessEvents(); ProcessEvents(); }
    };
    const char* path = "/tmp/op1emu-snapshot-test.img";
    std::array<u8, 2112> source{};
    source.fill(0xFF);
    source[0] = 0xF0;
    source[2048] = 0xCC;
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        std::vector<u8> data(65 * 2048, 0xFF), oob(65 * 64, 0xFF);
        data[5 * 2048] = source[0];
        oob[5 * 64] = source[2048];
        data[64 * 2048] = 0xAA;
        file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        file.seekp(536870912);
        file.write(reinterpret_cast<const char*>(oob.data()), static_cast<std::streamsize>(oob.size()));
        Check(file.good(), "create snapshot data/OOB fixture");
    }
    {
        SnapshotCpu cpu;
        auto flash = std::make_shared<MT29F4G08>(cpu, path, true);
        auto address = [&](u32 column, u32 page) {
            for (u8 byte : std::array<u8, 5>{static_cast<u8>(column), static_cast<u8>(column >> 8),
                    static_cast<u8>(page), static_cast<u8>(page >> 8), static_cast<u8>(page >> 16)})
                flash->SendAddress(byte);
        };
        auto read = [&](u32 page) {
            flash->SendCommand(0);
            address(0, page);
            flash->SendCommand(0x30);
            Check(flash->CompletePendingPageRead(), "explicit array-read completion in snapshot mode");
            std::array<u8, 2112> bytes{};
            Check(flash->PageRead(bytes.data(), bytes.size()) == bytes.size(), "read complete snapshot page/OOB");
            return bytes;
        };
        Check(read(5) == source, "snapshot initially reads backing data/OOB");
        flash->SendCommand(0x80);
        address(0, 5);
        const std::array<u8, 2> program{0x0F, 0x5A};
        Check(flash->PageWrite(program.data(), program.size()) == program.size(), "program through production bulk path");
        flash->SendCommand(0x85);
        flash->SendAddress(0x00);
        flash->SendAddress(0x08);
        flash->WriteData(0x0F);
        flash->SendCommand(0x10);
        cpu.Drain();
        auto expected = source;
        expected[0] = 0;
        expected[1] = 0x5A;
        expected[2048] = 0x0C;
        Check(read(5) == expected, "snapshot program ANDs data/OOB and reads its overlay");
        flash->SendCommand(0x80);
        address(0, 5);
        flash->WriteData(0xFF);
        flash->SendCommand(0x10);
        cpu.Drain();
        Check(read(5) == expected, "later snapshot program cannot restore cleared bits");
        flash->SendCommand(0x60);
        flash->SendAddress(0);
        flash->SendAddress(0);
        flash->SendAddress(0);
        flash->SendCommand(0xD0);
        cpu.Drain();
        std::array<u8, 2112> erased{}; erased.fill(0xFF);
        Check(read(0) == erased && read(5) == erased && read(63) == erased && read(64)[0] == 0xAA,
              "snapshot erase covers 64 data/OOB pages, replacing dirty pages but not another block");
    }
    {
        BlackFinCpu cpu;
        MT29F4G08 reopened(cpu, path, true);
        reopened.SendCommand(0);
        for (u8 byte : std::array<u8, 5>{0, 0, 5, 0, 0}) reopened.SendAddress(byte);
        reopened.SendCommand(0x30);
        Check(reopened.CompletePendingPageRead(), "reopen snapshot source");
        std::array<u8, 2112> bytes{};
        reopened.PageRead(bytes.data(), bytes.size());
        Check(bytes == source, "discarded snapshot writes and erases never changed source bytes");
        const char* missing = "/tmp/op1emu-snapshot-missing-test.img";
        std::remove(missing);
        bool rejected = false;
        try { MT29F4G08 invalid(cpu, missing, true); }
        catch (const std::exception&) { rejected = true; }
        Check(rejected && !std::ifstream(missing).good(), "missing snapshot fails without creating a source");
    }
    {
        SnapshotCpu cpu;
        MT29F4G08 persistent(cpu, path); // Default remains persistent.
        persistent.SendCommand(0x80);
        for (u8 byte : std::array<u8, 5>{0, 0, 5, 0, 0}) persistent.SendAddress(byte);
        persistent.WriteData(0x33);
        persistent.SendCommand(0x10);
        cpu.Drain();
    }
    {
        std::ifstream file(path, std::ios::binary);
        file.seekg(5 * 2048);
        char byte = 0;
        file.get(byte);
        Check(file.good() && static_cast<u8>(byte) == 0x30, "default persistent program writes ANDed bytes to source");
    }
    Check(std::remove(path) == 0, "remove snapshot fixture");
}
