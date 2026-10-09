#include "nand.h"
#include "nand_ecc.h"
#include <cstring>

NFC::NFC(u32 baseAddr) : RegisterDevice("NFC", baseAddr, 0x50) {
    REG32(NFC_CTL, 0x00);
    FIELD(NFC_CTL, PG_SIZE, 9, 1, R(pageSize), W(pageSize));

    REG32(NFC_STAT, 0x04);
    FIELD(NFC_STAT, NBUSY, 0, 1, R(notBusy), N());
    FIELD(NFC_STAT, WB_FULL, 1, 1, R(writeBufferFull), N());
    FIELD(NFC_STAT, PG_WR_STAT, 2, 1, R(pageWritePending), N());
    FIELD(NFC_STAT, PG_RD_STAT, 3, 1, R(pageReadPending), N());
    FIELD(NFC_STAT, WB_EMPTY, 4, 1, R(writeBufferEmpty), N());

    REG32(NFC_IRQSTAT, 0x08);
    FIELD(NFC_IRQSTAT, NBUSYIRQ, 0, 1, R(notBusyRising), W1C(notBusyRising));
    FIELD(NFC_IRQSTAT, WB_OVF, 1, 1, R(writeBufferOverflow), W1C(writeBufferOverflow));
    FIELD(NFC_IRQSTAT, WB_EDGE, 2, 1, R(writeBufferEmptyRising), W1C(writeBufferEmptyRising));
    FIELD(NFC_IRQSTAT, RD_RDY, 3, 1, R(readDataReady), [this](u32 v) {
        if (v) {
            readDataReady = false;
            if (readDataConsumed && readRequestPending && !nandFlash->IsBusy()) {
                readRequestPending = false;
                readDataReady = true;
            }
            readDataConsumed = false;
        }
    });
    FIELD(NFC_IRQSTAT, WR_DONE, 4, 1, R(pageWriteDone), W1C(pageWriteDone));
    NFC_IRQSTAT.writeCallback = [this](u32 value) {
        UpdateInterrupts();
    };

    REG32(NFC_IRQMASK, 0x0C);
    FIELD(NFC_IRQMASK, irqmask, 0, 5, R(irqmask), W(irqmask));
    NFC_IRQMASK.writeCallback = [this](u32 value) {
        UpdateInterrupts();
    };

    for (int i = 0; i < 4; i++) {
        REG32(NFC_ECC, 0x10 + i * 4);
        auto r = [this, i]() -> u32 {
            return ecc[i];
        };
        auto w = [this, i](u32 v) {
            ecc[i] = static_cast<u16>(v);
        };
        FIELD(NFC_ECC, ECC, 0, 16, r, w);
    }

    REG32(NFC_COUNT, 0x20);
    FIELD(NFC_COUNT, ECCCNT, 0, 10, R(transferCount), N());

    REG32(NFC_RST, 0x24);
    FIELD(NFC_RST, ECC_RST, 0, 1, R(0), [this](u32 v) {
        if (v) {
            ResetECC();
        }
    });

    REG32(NFC_PGCTL, 0x28);
    FIELD(NFC_PGCTL, PG_RD_START, 0, 1, R(0), [this](u32 v) {
        pageReadStart = v != 0;
        if (pageReadStart) {
            nandFlash->StartPageRead();
            pageReadPending = true;
        }
    });
    FIELD(NFC_PGCTL, PG_WR_START, 1, 1, R(0), [this](u32 v) {
        pageWriteStart = v != 0;
        if (pageWriteStart) {
            nandFlash->StartPageWrite();
            pageWritePending = true;
        }
    });

    REG32(NFC_READ, 0x2C);
    FIELD(NFC_READ, READ_DATA, 0, 8, [this]() {
        if (readDataReady && !readDataConsumed) {
            readData = nandFlash->ReadData();
            ++transferCount;
            readDataConsumed = true;
        }
        return readData;
    }, N());

    REG32(NFC_ADDR, 0x40);
    FIELD(NFC_ADDR, ADDR, 0, 8, R(0), [this](u32 v) {
        address = v;
        nandFlash->SendAddress(address);
    });

    REG32(NFC_CMD, 0x44);
    NFC_CMD.writeCallback = [this](u32 v) {
        command = v;
        nandFlash->SendCommand(command);
        SetNotBusy(!nandFlash->IsBusy());
    };

    REG32(NFC_DATA_WR, 0x48);
    FIELD(NFC_DATA_WR, DATA_WR, 0, 8, R(0), [this](u32 v) {
        writeData = v;
        nandFlash->WriteData(writeData);
    });

    REG32(NFC_DATA_RD, 0x4C);
    NFC_DATA_RD.writeCallback = [this](u32 v) {
        if (readDataReady || nandFlash->IsBusy()) {
            readRequestPending = true;
        } else {
            readDataReady = true;
            readDataConsumed = false;
        }
        UpdateInterrupts();
    };
}

void NFC::ResetECC()
{
    ecc[0] = 0;
    ecc[1] = 0;
    ecc[2] = 0;
    ecc[3] = 0;
    transferCount = 0;
}

u32 NFC::DMARead(int x, int y, void* dest, u32 length)
{
    if (!PageReadDMAReady()) return 0;
    u32 len = nandFlash->PageRead(static_cast<u8*>(dest), length);
    CalculateECC(static_cast<const u8*>(dest), len);
    transferCount += len;
    if (transferCount >= PageSize()) {
        pageReadPending = false;
        pageReadStart = false;
    }
    return len;
}

u32 NFC::DMAWrite(int x, int y, const void* source, u32 length)
{
    u32 len = nandFlash->PageWrite(static_cast<const u8*>(source), length);
    CalculateECC(static_cast<const u8*>(source), len);
    transferCount += len;
    if (transferCount >= PageSize()) {
        pageWritePending = false;
        pageWriteDone = true;
        UpdateInterrupts();
    }
    return len;
}

void NFC::CalculateECC(const u8* data, u32 length) {
    u32 offset = 0;
    if (transferCount < 256) {
        u32 firstBlockLen = std::min(length, (u32)(256 - transferCount));
        auto [ecc1, ecc2] = ComputeEccPair(data, firstBlockLen, transferCount);
        ecc[0] ^= ecc1;
        ecc[1] ^= ecc2;
        offset = firstBlockLen;
    }
    if (transferCount + length >= 256) {
        auto [ecc1, ecc2] = ComputeEccPair(data + offset, length - offset, 0);
        ecc[2] ^= ecc1;
        ecc[3] ^= ecc2;
    }
}

void NFC::UpdateInterrupts() {
    u16 intStat = Read32(0x08);
    // NOTE that irqmask bits are active low
    if (intStat & (~irqmask)) {
        TriggerInterrupt(1);
    } else {
        TriggerInterrupt(0);
    }
}

void NFC::SetNotBusy(bool value) {
    bool oldNotBusy = notBusy;
    notBusy = value;
    if (notBusy && !oldNotBusy) {
        notBusyRising = true;
        UpdateInterrupts();
    }
}

void NFC::SetWriteBufferEmpty(bool value) {
    bool oldWriteBufferEmpty = writeBufferEmpty;
    writeBufferEmpty = value;
    if (writeBufferEmpty && !oldWriteBufferEmpty) {
        writeBufferEmptyRising = true;
        UpdateInterrupts();
    }
}

void NFC::ProcessWithInterrupt(int ivg) {
    nandFlash->CompletePendingReset();
    nandFlash->CompletePendingPageRead();
    SetNotBusy(!nandFlash->IsBusy());
    if (readRequestPending && !readDataReady && !nandFlash->IsBusy()) {
        readRequestPending = false;
        readDataReady = true;
        readDataConsumed = false;
    }
    UpdateInterrupts();
}

bool NFC::ServiceIdleCompletion() {
    if (!nandFlash) return false;
    const bool completed = nandFlash->CompletePendingReset() ||
                           nandFlash->CompletePendingPageRead();
    if (!completed) return false;
    SetNotBusy(!nandFlash->IsBusy());
    return true;
}

bool NFC::PageReadDMAReady() const {
    return nandFlash && pageReadStart && pageReadPending && nandFlash->IsDataReady();
}
