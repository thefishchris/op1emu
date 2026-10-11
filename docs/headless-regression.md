# Headless regression frontend

Build `op1emu-headless`, or use `op1emu --headless`. The standalone target
does not link a window/audio backend; the root configure still discovers the
project's existing dependencies. It opens NAND in **snapshot mode by default**.
Use `--nand-rw` only when persistent guest writes are desired.

```sh
cmake --build build --target op1emu-headless headless_tests execution_accounting_tests
./build/op1emu-headless --help
./build/op1emu-headless /absolute/source.nand.img \
  --ui-config /absolute/path/to/gui/ui.json --input-script commands.txt \
  --max-runtime 600000 --stop-on-unsupported-idle \
  --screenshot-on-stop /new/output/tape.ppm
```

`--profile strict|functional` selects an execution profile and overrides
`OP1EMU_PROFILE` (see `docs/emulator-profiles.md`). `run-op1.sh --headless`
wraps this with private input copies.

The CPU still boots the original OP-1 NAND path. No flags/interrupts are fabricated
to advance an unsupported boundary. The configured OTP input must also be
supplied: either `otp.bin` in the working directory, or an existing image
selected with `OP1EMU_OTP_FILE`, which is validated and never created
(`docs/otp-image.md`). Use a private OTP copy, or a read-only image, and pass
absolute NAND/UI/script paths; do not overwrite an existing OTP.

One script command per line; blank lines and lines beginning with `#` are ignored:

```text
accel 64 -64 512
volume 128
wait-frames 1 600000
screenshot /new/output/startup.ppm
wait-frames 20 600000
screenshot /new/output/later.ppm
quit
```

- `keys` lists names from the same UI JSON used by mouse input.
- `press KEY`, `release KEY`, `tap KEY [MS]` queue logical controls through the
  platform; tap defaults to 100 host milliseconds. A key such as `f3#` is valid.
- `wait MS` waits on host observation time; it does not advance guest time.
- `wait-frames N [MS]` waits for N new complete guest-drawn frames; default host
  timeout is 600000 ms. Receiving a frame is not proof that firmware startup is
  complete. Timeout is an error, never a synthetic frame or wake.
- `screenshot NEW.ppm` writes the latest complete RGB565 frame, expanded to RGB
  and rotated like the GUI. Paths in script commands cannot contain spaces.
  Existing paths are refused, protecting source/evidence files from overwrite.
- `accel X Y Z` pins int16 sensor values; `accel random` restores the existing
  random host feed. `volume N` accepts 0..255. Pinned values are pushed once
  per change rather than continuously generating host-input events.
- `quit`, SIGINT, or SIGTERM ends the host run. EOF alone is not quit.

`--stop-on-unsupported-idle` is an observation bound, not a failed-JIT verdict.
It stops before further execution through the boundary. `--screenshot-on-stop`
can preserve the already completed frame even while a script was waiting for
more frames. No framebuffer, timing, or hardware completion is invented.

Exit codes: 0 for normal quit/signal or an explicitly requested boundary stop;
1 for configuration/script/frame timeout/screenshot or genuine CPU execution
failure; 2 when `--max-runtime` expires. Genuine failure latches `Failed()` and
does not repeatedly retry the unchanged PC. Architectural IDLE and guest
exceptions are not by themselves JIT failures.

Host USB/IP and audio sinks are not started in headless mode. Guest devices,
PPI/DMA, existing clocks, and IRQ routing are unchanged. There is no automatic
power-manager flag clear. The experimental guest clock is opt-in through
`OP1EMU_EXPERIMENTAL_GUEST_CLOCK=1` (see `docs/guest-clock-architecture.md`).
In that mode `--stop-on-unsupported-idle` also stops at a classified
`held-domain-active` IDLE.
