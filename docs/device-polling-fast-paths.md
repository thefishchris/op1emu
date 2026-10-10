# Per-block device polling fast paths

`BlackFinCpu::Run()` executes one guest basic block (about 10 packets in the
v241 SPORT-active workload, 24–31M blocks per guest second). After each block
it calls `ProcessWithInterrupt()` on every device. Profiling (2026-10-09,
evidence kept locally) found most host time in that loop, not in guest code.
A fast path here skips only work that provably changes nothing. Guest-visible
behaviour, including interrupt-line updates, is preserved exactly.

## SPORT DMA

See `docs/sport-frame-format.md`, "DMA polling fast path".

## USB (`USB::ProcessTransfer`)

### Behaviour (unchanged)

- **Global enable:** with `GLOBAL_ENA` clear, a poll returns at once.
- **Full pass:** otherwise, under `USB::mutex`, the pass:
  1. walks the 8 endpoints, moving FIFO/buffer data and calling host
     callbacks;
  2. completes EP0 `DATAEND`;
  3. calls `UpdateInterrupts()`. That function does six register reads,
     then drives the INT0, INT1 and INT2 lines. All three handler calls are
     made every time, even with unchanged levels. Each reaches
     `SIC::SetInterruptLevel`, whose re-forwarding of pending SIC sources is
     guest-observable and is deliberately left as is.
- **Host side:** the host (usbipd, its own thread) changes state only through
  `HandleSetupPacket`, `HandleDataWrite`, `HandleDataRead` (and, in local
  work, `HandleBusReset`). Each runs under `USB::mutex`. The guest changes
  state through register writes on the CPU thread.

### Inactive predicate (`USB::NoPendingWork`)

A full pass changes nothing when all of the following hold:
- no setup callback is pending, and EP0 `DATAEND` is clear;
- for every endpoint, there is no TX or RX host callback and no buffered host
  OUT data;
- every enabled TX endpoint has no packet ready. (This committed version moves
  a ready packet without waiting for the host.)
- every enabled RX endpoint that is not already flagged ready has an empty
  FIFO.

These conditions mirror every branch of the walk. The walk's remaining
statement, a `std::vector::reserve`, is not observable.

### Fast path

`workPending` (a `std::atomic<bool>`) is set:
- by every guest register write (`USB::Write`/`Write32`), since any write may
  create work or change an interrupt input;
- by every host request, under the mutex.

Each full pass recomputes it at the end as `!NoPendingWork()`.

A poll with `workPending` clear and valid cached levels takes the mutex, then
drives the three lines with the levels from the last `UpdateInterrupts()`
computation, in the same order, and returns.

The cached levels stay exact because every input to `UpdateInterrupts()` (the
interrupt flags, enables, routing and reset/SOF bits) changes only through:
- a guest write;
- a host request, which also recomputes the levels itself;
- a full pass.

Register reads have no side effects on those inputs. FIFO reads change only
FIFO contents, which can only shrink, so they can never create work.

### Concurrency

- **Reads:** `workPending` is read only on the CPU thread, under the mutex.
  Guest writes are on the same thread, so program order applies. Host writes
  happen under the mutex, so they are visible to the next poll.
- **No lost requests:** a host request lands either before the poll's lock
  (and is seen) or after its unlock (and is seen by the next poll).
- **Atomics:** relaxed ordering suffices. The atomic only removes the formal
  race between a CPU-thread store and a host-thread store.
- **Mutex:** the fast path keeps the mutex; correctness first.

### Evidence

- **Tests:** `tests/usb_fastpath_tests.cpp` covers:
  - a scripted A/B against the always-walk path: every line update, register
    value and callback is identical;
  - 20,000 host IN requests posted from a second thread while the guest only
    polls in the fast path. Each completes, and walks happen only after the
    guest's packet write and after the host request.
  - host OUT data posted from a second thread and seen by guest polls.

  A ThreadSanitizer build of the test reported no races.
- **v241 functional, same script:**
  - after USB enable, 3 full passes in 343M polls (firmware register writes
    at enable and at about 8 host seconds later);
  - blocks per host second +18–19% in the SPORT-active phases;
  - SPORT, DMA, IRQ28 and core timer cadences unchanged.

## SIC -> CEC forwarding

The per-pass raise de-duplication added earlier is superseded by the
level-sensitive model in `docs/sic-interrupt-delivery.md`. Unchanged levels
are no longer forwarded at all, and an edge becomes one latch request in a
mask rather than a queued event.

## DMA active channels (`DMA::ProcessWithInterrupt`)

The per-block DMA pass used to call `DMAChannel::ProcessTransfer()` on all 16
channels. A channel that is not both enabled (`DMAEN`) and running returns 0
there at once, without side effects. Those two flags change only:
- through the channel's registers, i.e. `DMA::Write32`, including the
  `CONFIG` write callback that sets `running = enabled` and fetches the
  first descriptor; or
- inside its own `ProcessTransfer()`: completion, or the next descriptor,
  whose fetch writes the channel's registers, `CONFIG` included.

Peripheral readiness (SPORT frames due, NFC data) never changes them; it only
changes how much a transfer moves. `DMA::activeChannels` (bit n = channel n)
mirrors `enabled && running`. It is refreshed:
- for a channel after every `DMA::Write32` to it;
- after each of the channel's per-block `ProcessTransfer()`;
- for all channels after the idle/peripheral service helpers that call
  `ProcessTransfer()` directly.

The pass visits the same channels in the same ascending order and skips only
channels outside the set. A stale set bit only costs a no-op call. A missed
activation could come only from a register write, and that path is refreshed.

### Evidence

`tests/dma_active_channel_tests.cpp` runs the same deterministic workload with
and without tracking, and compares a hash of every channel register, SIC/CEC
state, SPORT word count and PC after each of 400,000 blocks, plus the
destination buffers. The workload covers:
- SPORT TX on a two-descriptor large-model list (firmware `CONFIG=0x7489`),
  disabled and re-enabled mid-run;
- SPORT RX autobuffer with interrupts;
- MDMA0 1D, started and later re-run at 32-bit;
- MDMA1 2D.

All states are identical. Removing the refresh on register writes makes the
test fail.

v241 functional A/B (same binary, tracking switched off/on), per guest second:
- **`ProcessTransfer` calls:** 1.09 billion → 60,000 in boot; 292–386M →
  40–59M in the SPORT-active phases.
- **Throughput:** blocks per host second +6.8–9.8%.
- **Behaviour:** SPORT, DMA, IRQ28 and core timer cadences unchanged; the UI
  works.

NAND/NFC DMA is covered by the firmware boot itself, which reads its
filesystem through it.
