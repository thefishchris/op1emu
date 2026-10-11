# OP-1 Emulator

An experimental emulator for the original Teenage Engineering OP-1 (Blackfin
BF524), developed against firmware v241. It is not a complete OP-1, and it is
not cycle accurate. It does not target the OP-1 Field.

![OP-1 Emulator Screenshot](screenshot.png)

This fork (`thefishchris/op1emu`) builds on the upstream
[op1emu](https://github.com/op1emu/op1emu) project. It adds evidence-backed
device fixes, explicit timing profiles, a headless runner and tools for
preparing the device-specific inputs.

## Status

Observed with firmware v241 under the `functional` profile, using a
locally prepared NAND image and OTP image (see [Inputs](#inputs)):

| Feature | Status |
| --- | --- |
| Boot to the normal tape screen | Works. It takes several minutes, because emulation runs well below real time. |
| Tape play and stop, record entry | Works, from the GUI (mouse or keys) and from headless scripts |
| Synth keys (held note) | Works: the note is shown and the engine produces samples |
| Encoders, other buttons | Input reaches the firmware; full behaviour is not verified |
| Live audio output | Not usable: the host audio device underruns heavily (see [Limitations](#limitations)) |
| USB (host connection, firmware updater) | Not supported |
| `strict` profile | Stops by design at the first unresolved hardware behaviour, in the boot ROM. A research and debugging mode. |
| Firmware other than v241, OP-1 Field | Not supported |

## Prerequisites

- **CMake 3.20 or newer**, **Ninja** (recommended), and a **C++20-capable
  compiler** (GCC or Clang).
- **LLVM 15 exactly.** The Bcore CPU core requires
  `find_package(LLVM 15)`, and newer LLVM releases are not accepted. Pass its
  CMake directory with `-DLLVM_DIR=…/lib/cmake/llvm` if it is not the system
  default.
- **OpenGL and the windowing development packages GLFW needs** (X11 or
  Wayland), for the GUI.
- **Python 3**, for some tests and the OTP import tool.
- **Network access during the first configure.** CMake fetches spdlog, GLFW,
  nlohmann/json, stb, cqueue, uvw/libuv and miniaudio.

## Getting the source

```sh
git clone --recursive https://github.com/thefishchris/op1emu.git
cd op1emu
# Or, in an existing clone:
git submodule update --init --recursive
```

The `ext/bcore` (CPU core) and `ext/bfin_sim` submodules are required. The
build expects them at exactly the commits this repository pins.

## Building and testing

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DLLVM_DIR=/path/to/llvm-15/lib/cmake/llvm
cmake --build build
ctest --test-dir build --output-on-failure
```

The build produces:
- `build/op1emu`: the GUI;
- `build/op1emu-headless`: the scriptable runner without a window or audio;
- `build/provision_yaffs`: the NAND provisioning tool.

The test suite needs no firmware.

## Inputs

The emulator needs device data that is **not distributed** with the source.
Never commit it.

| Input | What it is | Docs |
| --- | --- | --- |
| NAND image | A 553,648,128-byte image in the emulator layout (all 2048-byte page data, then all 64-byte OOB) holding the boot loader, v241 firmware and a provisioned content filesystem. Raw interleaved programmer dumps must be converted. | [NAND boot investigation](docs/nand-boot-investigation.md), [YAFFS provisioning](docs/yaffs-provisioning.md) |
| OTP image | An 8,192-byte image with the OP-1's OTP provisioning. Without it v241 takes its recovery path. `tools/import_otp_dump.py` builds one from a textual dump. | [OTP image](docs/otp-image.md) |
| Firmware | v241 (`op1_241.op1`), the source of the content filesystem | [Provenance](docs/v241-nand-provenance-and-traversal.md) |

## Running

`run-op1.sh` is the recommended launcher.
- **NAND:** opened as a snapshot, so the image is never modified.
- **OTP:** copied into a fresh private run directory. The directory and its
  OTP copy are kept for inspection, so delete them when done.

```sh
# GUI
./run-op1.sh --profile functional --nand PATH/TO/nand.img --otp PATH/TO/otp.bin

# Headless, scripted
./run-op1.sh --headless --profile functional --nand PATH/TO/nand.img --otp PATH/TO/otp.bin \
    -- --input-script script.txt --max-runtime 900000
```

**Headless script commands**, one per line:
- `keys`
- `press KEY`, `release KEY`, `tap KEY [MS]`
- `wait MS`, `wait-frames N [MS]`
- `screenshot NEW.ppm`
- `accel X Y Z`, `accel random`
- `volume N`
- `quit`

See [headless regression](docs/headless-regression.md) and
`./build/op1emu-headless --help`.

**Running the binaries directly:** the GUI reads `gui/ui.json` relative to its
working directory, so run it from the repository root:

```sh
OP1EMU_PROFILE=functional OP1EMU_OTP_FILE=PATH/TO/otp.bin \
    ./build/op1emu PATH/TO/nand.img --nand-snapshot
```

`OP1EMU_OTP_FILE` must name an existing, valid 8,192-byte image. Without it
the emulator uses (and creates if missing) `./otp.bin`.

### Profiles

| Profile | Use |
| --- | --- |
| `functional` | The usable mode. It enables a small, explicit set of timing assumptions, all logged at start-up: CLKIN 25 MHz (PROVISIONAL), an experimental PLL awake bypass, a guest clock for the core timer and SPORT at an assumed 44.1 kHz, and the core timer counting during IDLE. |
| `strict` | No timing assumptions. Stops at unresolved hardware behaviour. |
| unset | The pre-profile behaviour; same outcome as `strict` for v241 |

Details: [emulator profiles](docs/emulator-profiles.md).

### GUI controls

- **Mouse:** click the keys and buttons on the drawn OP-1. Encoders step
  when you click their clockwise or counter-clockwise half.
- **Window:** it can be resized; clicks are mapped back to the drawing.
- **Keyboard:**

  | Key | Control |
  | --- | --- |
  | R | Record (hold R and press Space to record) |
  | Space | Play |
  | Escape | Stop |
  | Shift | Shift |

  Key mappings live in `gui/ui.json` (`keycaps`).

## Limitations

The full register of hacks, assumptions and limitations is
[docs/hacks.md](docs/hacks.md). The most important:
- **Speed:** emulation runs well below real time, at roughly 0.1–0.16 guest
  seconds per host second on the development machine.
- **Audio:** the host audio device receives far fewer samples than it plays,
  so output is mostly underrun silence. The legacy profiles add an inherited
  ÷10 slowdown on top.
- **USB:** incomplete. No session or bus-reset state; USB/IP enumeration and
  the firmware updater are unsupported.
- **Timing assumptions:** the hardware clock assumptions are provisional
  (CLKIN, the audio frame rate, core timer behaviour in IDLE).
- **Host time:** the real-time clock, CYCLES and GP timers derive from host
  time, so ordinary runs are not bit-for-bit reproducible across hosts.
- **Partial device models:** several devices are fixed-value stand-ins or
  partial models (some I²C devices, GPIO-expander corner cases, NAND command
  `0x50`).

## Documentation

**Using the emulator:**
- [Emulator profiles](docs/emulator-profiles.md)
- [Headless regression runner](docs/headless-regression.md)
- [Temporary hacks, assumptions and limitations](docs/hacks.md)

**Device data and tools:**
- [OTP image and import tool](docs/otp-image.md)
- [YAFFS provisioning tool](docs/yaffs-provisioning.md)
- [NAND boot investigation](docs/nand-boot-investigation.md)
- [v241 input provenance](docs/v241-nand-provenance-and-traversal.md)

**Design and device models:**
- [Guest clock architecture](docs/guest-clock-architecture.md)
- [Multi-block execution](docs/multiblock-execution.md)
- [Device polling fast paths](docs/device-polling-fast-paths.md)
- [SPORT frame format](docs/sport-frame-format.md)
- [SIC interrupt delivery](docs/sic-interrupt-delivery.md)
- [MCP23017 expander model](docs/mcp23017-expander.md)

**Investigation records:**
- [BF524 execution accounting](docs/bf524-execution-accounting.md)
- [Shadow timing coverage](docs/shadow-timing-coverage.md)
- [PLL/IDLE foundation](docs/bf524-pll-idle-foundation.md)
- [PLL/IDLE checkpoint](docs/pll-idle-awake-bypass-checkpoint.md)
- [Clock readiness](docs/op1-clock-readiness.md)
- [Upstream ports](docs/upstream-correctness-ports.md)
- [Upstream guest-clock audit](docs/upstream-guest-clock-audit.md)

## Acknowledgements

- [op1emu](https://github.com/op1emu/op1emu): the upstream project this fork
  is based on.
- [bcore](https://github.com/op1emu/bcore): the Blackfin CPU core.
- [bfin_sim](https://github.com/op1emu/bfin_sim): a Blackfin simulator, from
  gdb/sim.
- [op1kenobi](https://github.com/alexmandelshtam/op1kenobi): OP-1 screenshot
  assets used for the GUI background.
