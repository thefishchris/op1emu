// MCP23017 register and interrupt behaviour against Microchip DS20001952C
// (MCP23017/MCP23S17 data sheet): IOCON (register 3-6), INTCAP (section
// 3.5.9), interrupt logic and mirroring (section 3.6, table 3-6), and byte
// and sequential modes (section 3.2.1). Register access goes through the I2C
// interface the firmware uses.
#include "peripheral/mcp230xx.h"
#include <cstdio>
#include <initializer_list>

namespace {
unsigned failures = 0;
void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "mcp23017: %s\n", message);
        ++failures;
    }
}

// Register addresses with IOCON.BANK = 0 (table 3-5).
constexpr u8 IODIRA = 0x00, GPINTENA = 0x04, GPINTENB = 0x05, DEFVALA = 0x06, INTCONA = 0x08, IOCON = 0x0A,
             IOCON_ALT = 0x0B, GPPUA = 0x0C, GPPUB = 0x0D, INTFA = 0x0E, INTCAPA = 0x10, INTCAPB = 0x11,
             GPIOA = 0x12, GPIOB = 0x13, OLATA = 0x14, OLATB = 0x15;
constexpr int INTA = 16, INTB = 17;

void write(MCP230XX& chip, u8 reg, std::initializer_list<u8> values) {
    chip.Stop();
    u8 bytes[8] = {reg};
    u32 n = 1;
    for (u8 v : values) bytes[n++] = v;
    check(chip.Write(bytes, n), "I2C write failed");
    chip.Stop();
}
void write(MCP230XX& chip, u8 reg, u8 value) { write(chip, reg, {value}); }

// Selects a register, then reads `count` bytes in one transfer.
void readMany(MCP230XX& chip, u8 reg, u8* out, u32 count) {
    chip.Stop();
    check(chip.Write(&reg, 1), "I2C register select failed");
    check(chip.Read(out, count), "I2C read failed");
    chip.Stop();
}
u8 read(MCP230XX& chip, u8 reg) {
    u8 value = 0;
    readMany(chip, reg, &value, 1);
    return value;
}

// Both ports are inputs at reset (IODIR = 0xFF). Pull-ups make the pins
// idle high, then interrupt-on-change is enabled on every pin.
void enableInterrupts(MCP230XX& chip) {
    write(chip, GPPUA, {0xFF, 0xFF});
    read(chip, GPIOA);
    read(chip, GPIOB);
    write(chip, GPINTENA, 0xFF);
    write(chip, GPINTENB, 0xFF);
}

void IoconBitZeroIsUnimplemented() {
    MCP230XX chip(0x20, MCP230XXModel::MCP23017);
    write(chip, IOCON, 0x01);
    check(read(chip, IOCON) == 0x00, "IOCON bit 0 is unimplemented and reads 0 (register 3-6)");
    write(chip, IOCON, 0x67);
    check(read(chip, IOCON) == 0x66, "IOCON keeps MIRROR, SEQOP, ODR and INTPOL but not bit 0");
    check(read(chip, IOCON_ALT) == 0x66, "IOCON is shared at 0x0A and 0x0B with BANK = 0");
}

void IntcapReadClearsInterrupt() {
    MCP230XX chip(0x20, MCP230XXModel::MCP23017);
    enableInterrupts(chip);
    chip.SetPinInput(0, GPIOPinLevel::Low);
    check(chip.GetPinOutput(INTA) == GPIOPinLevel::Low, "pin change asserts active-low INTA");
    check(read(chip, INTFA) == 0x01, "INTFA flags the pin that caused the interrupt");
    chip.SetPinInput(0, GPIOPinLevel::High);
    check((read(chip, INTCAPA) & 1) == 0, "INTCAP keeps the first captured value until cleared (3.6.4)");
    check(chip.GetPinOutput(INTA) == GPIOPinLevel::High, "an INTCAP read clears the interrupt (3.5.9, 3.6.4)");
    check(read(chip, INTFA) == 0x00, "clearing the interrupt clears INTF");
}

void GpioReadClearsInterrupt() {
    MCP230XX chip(0x20, MCP230XXModel::MCP23017);
    enableInterrupts(chip);
    chip.SetPinInput(8, GPIOPinLevel::Low);
    check(chip.GetPinOutput(INTB) == GPIOPinLevel::Low, "port B change asserts INTB");
    check(chip.GetPinOutput(INTA) == GPIOPinLevel::High, "INTA is independent without MIRROR");
    check((read(chip, GPIOB) & 1) == 0, "GPIOB reads the pin level");
    check(chip.GetPinOutput(INTB) == GPIOPinLevel::High, "a GPIO read clears the interrupt (3.6.4)");
}

void InterruptFromDefaultPersists() {
    MCP230XX chip(0x20, MCP230XXModel::MCP23017);
    write(chip, GPPUA, 0xFF);
    read(chip, GPIOA);
    write(chip, DEFVALA, 0xFF);
    write(chip, INTCONA, 0x01);
    write(chip, GPINTENA, 0x01);
    chip.SetPinInput(0, GPIOPinLevel::Low);
    check(chip.GetPinOutput(INTA) == GPIOPinLevel::Low, "a mismatch with DEFVAL asserts INTA");
    read(chip, INTCAPA);
    check(chip.GetPinOutput(INTA) == GPIOPinLevel::Low, "a DEFVAL interrupt remains while the condition exists (3.6.5)");
    chip.SetPinInput(0, GPIOPinLevel::High);
    read(chip, GPIOA);
    check(chip.GetPinOutput(INTA) == GPIOPinLevel::High, "the interrupt clears once the pin matches DEFVAL");
}

void InterruptPolarity() {
    MCP230XX chip(0x20, MCP230XXModel::MCP23017);
    enableInterrupts(chip);
    write(chip, IOCON, 0x02); // INTPOL: active-high push-pull
    check(chip.GetPinOutput(INTA) == GPIOPinLevel::Low, "inactive active-high INTA is low");
    chip.SetPinInput(0, GPIOPinLevel::Low);
    check(chip.GetPinOutput(INTA) == GPIOPinLevel::High, "active active-high INTA is high");
    write(chip, IOCON, 0x06); // ODR overrides INTPOL
    check(chip.GetPinOutput(INTA) == GPIOPinLevel::Low, "an active open-drain INTA sinks low, whatever INTPOL says");
    read(chip, INTCAPA);
    check(chip.GetPinOutput(INTA) == GPIOPinLevel::High, "a released open-drain INTA reads as pulled up");
    write(chip, IOCON, 0x04); // ODR with INTPOL = 0
    chip.SetPinInput(0, GPIOPinLevel::High);
    check(chip.GetPinOutput(INTA) == GPIOPinLevel::Low, "open-drain polarity does not depend on INTPOL");
    read(chip, GPIOA);
    check(chip.GetPinOutput(INTA) == GPIOPinLevel::High, "a GPIO read releases the open-drain INTA");
}

void MirroredInterruptsClearPerPort() {
    MCP230XX chip(0x21, MCP230XXModel::MCP23017);
    enableInterrupts(chip);
    write(chip, IOCON, 0x40); // MIRROR
    chip.SetPinInput(0, GPIOPinLevel::Low);
    check(chip.GetPinOutput(INTA) == GPIOPinLevel::Low && chip.GetPinOutput(INTB) == GPIOPinLevel::Low,
          "with MIRROR a port A interrupt drives both pins");
    chip.SetPinInput(8, GPIOPinLevel::Low);
    read(chip, INTCAPA);
    check(chip.GetPinOutput(INTA) == GPIOPinLevel::Low && chip.GetPinOutput(INTB) == GPIOPinLevel::Low,
          "reading port A leaves a pending port B interrupt active (table 3-6)");
    read(chip, INTCAPB);
    check(chip.GetPinOutput(INTA) == GPIOPinLevel::High && chip.GetPinOutput(INTB) == GPIOPinLevel::High,
          "both mirrored pins release once both ports are read");
}

void ByteModeTogglesRegisterPair() {
    MCP230XX chip(0x20, MCP230XXModel::MCP23017);
    write(chip, IODIRA, {0x00, 0x00}); // both ports output (sequential write)
    write(chip, GPIOA, {0x5A, 0xA5});  // sequential write: GPIOA then GPIOB
    write(chip, IOCON, 0x20);          // SEQOP = 1: byte mode, BANK = 0
    u8 bytes[4] = {};
    readMany(chip, GPIOA, bytes, 4);
    check(bytes[0] == 0x5A && bytes[1] == 0xA5 && bytes[2] == 0x5A && bytes[3] == 0xA5,
          "byte mode with BANK = 0 toggles between GPIOA and GPIOB (3.2.1)");
    readMany(chip, GPIOB, bytes, 2);
    check(bytes[0] == 0xA5 && bytes[1] == 0x5A, "the toggle may start at either register of the pair");
    write(chip, GPIOA, {0x11, 0x22, 0x33});
    check(read(chip, GPIOA) == 0x33 && read(chip, GPIOB) == 0x22, "byte-mode writes toggle the same way");
    check(read(chip, OLATA) == 0x33 && read(chip, OLATB) == 0x22, "GPIO writes set the output latches");
}

void SequentialModeIncrementsAndWraps() {
    MCP230XX chip(0x20, MCP230XXModel::MCP23017);
    write(chip, IODIRA, {0x0F, 0xF0});
    write(chip, OLATA, {0x33, 0x44}); // OLAT registers read back what was written
    u8 bytes[4] = {};
    readMany(chip, OLATA, bytes, 4);
    check(bytes[0] == 0x33 && bytes[1] == 0x44 && bytes[2] == 0x0F && bytes[3] == 0xF0,
          "sequential mode increments and rolls over from 0x15 to 0x00 (3.2.1)");
}

} // namespace

int main() {
    IoconBitZeroIsUnimplemented();
    IntcapReadClearsInterrupt();
    GpioReadClearsInterrupt();
    InterruptFromDefaultPersists();
    InterruptPolarity();
    MirroredInterruptsClearPerPort();
    ByteModeTogglesRegisterPair();
    SequentialModeIncrementsAndWraps();
    if (failures == 0) std::puts("mcp23017 tests passed");
    return failures == 0 ? 0 : 1;
}
