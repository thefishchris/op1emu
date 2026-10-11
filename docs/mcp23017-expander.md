# MCP23017 GPIO-expander model

`src/peripheral/mcp230xx.cpp` models the I²C GPIO expanders that carry the
OP-1 keys and controls. `BlackFinCpu` creates eight of them, at addresses
`0x20`–`0x27`, all as MCP23017.

**Sources:**
- Microchip DS20001952C, *MCP23017/MCP23S17*: register 3-6 (IOCON), section
  3.2.1 (byte and sequential modes), section 3.5.9 (INTCAP), section 3.6
  (interrupt logic, table 3-6 for MIRROR).
- Microchip DS21919E, *MCP23008/MCP23S08*. Its IOCON bit 0, ODR and
  interrupt-clearing rules are the same as the MCP23017's, so the corrections
  below hold for both models.

## Corrections (CONFIRMED by the data sheets)

| Behaviour | Before | Now | Source |
| --- | --- | --- | --- |
| IOCON bit 0 | Writable "INTCC" bit selecting which read clears an interrupt. That is an MCP23018 feature, not an MCP23017 one. | Unimplemented, reads 0 | DS20001952C register 3-6; DS21919E register 1-6 |
| Interrupt acknowledgement | Only GPIO reads cleared, unless bit 0 was set; then only INTCAP reads cleared | A read of INTCAP **or** GPIO clears that port's interrupt. INTCAP keeps the first capture until then. | DS20001952C sections 3.5.9 and 3.6.4 |
| Open-drain INT (`IOCON.ODR = 1`) | Active drove **high**, released drove low | Active sinks **low**; released reads as pulled up (high); INTPOL is ignored | DS20001952C register 3-6 ("overrides the INTPOL bit") |

Unchanged behaviour that `mcp23017_tests` also covers:
- push-pull INTPOL;
- MIRROR with per-port clearing (table 3-6);
- interrupt-on-change versus DEFVAL comparison (a DEFVAL interrupt stays
  active while the mismatch lasts, section 3.6.5);
- byte mode toggling GPIOA/GPIOB with `BANK = 0`;
- sequential mode rolling over from `0x15` to `0x00`.

## Effect on v241

v241 writes IOCON as `0x20`, `0x60` or `0x00`: it never sets bit 0 or ODR. It
acknowledges key interrupts by reading GPIO. A deterministic v241 replay
(boot, UI, play, stop, held note) gives identical execution, MMIO traffic,
interrupts and framebuffer with and without these corrections. They make the
model match the data sheet; they do not change current firmware behaviour.

## Open questions

| Question | Status | Notes |
| --- | --- | --- |
| Expander part fitted on the board | UNKNOWN | MCP23017 is assumed. Nothing is known to be an MCP23018. |
| Device `0x27` | LIKELY an MCP23008 | v241 accesses only registers `0x00`, `0x06`, `0x09` and `0x0A`, which are IODIR, GPPU, GPIO and OLAT in the MCP23008 map. It is modelled as an MCP23017. |
| Interrupt wiring between expanders and to PORTG | ASSUMED | The `FIXME` comments in `cpu.cpp` |
| `BANK = 1` register map | Model gap | The 16-bit address mapping (`REG_ADDR`) places port B at `bank << 1` instead of `0x10`. v241 never sets BANK. |
| OLAT writes | Model gap | They update only the latch value, not the output pins that GPIO reads. GPIO writes do both. |
