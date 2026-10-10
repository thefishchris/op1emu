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

## SIC -> CEC raise de-duplication (`BlackFinCpu::QueueInterruptRaise`)

### Path (unchanged)

1. A device calls `SIC::SetInterruptLevel(pin, level)`.
2. `SIC::ForwardInterrupts()` re-forwards **every** pending, unmasked source
   on every call, including unchanged levels from unrelated devices.
3. Each forward queues one `cec_raise(ivg)` event.
4. `ProcessEvents()` runs the queued events once per `Run()`, or on an IDLE
   wake.

Evidence kept locally (`sic-forwarding-investigation`) found 99.97% of about
19.9M forwards per guest second in v241 to be such re-forwards. Most are
re-forwards of one GPIO line that stays asserted.

### What is de-duplicated

- **Key:** the core interrupt level (IVG), together with the event-pass epoch.
- **Epoch:** `ProcessEvents()` increments it on entry and on exit, so it is
  odd during a pass and even between passes.
- **Same pass:** raises queued in the same epoch by this one call site carry
  the same delay. Their remaining delays are decremented together, so they
  execute in the same pass.
- **Rule:** a raise of an IVG already queued in the current epoch is not
  queued again.

### Why it is exact

Within one pass, a repeated `cec_raise(ivg)` cannot change anything:
- it only ORs `ILAT[ivg]` and sets the pending flag, both already done by the
  first raise;
- no queued event clears ILAT or dispatches, because dispatch happens in
  `cec_check_pending` outside the pass;
- once a raise in user mode has taken the illegal-use exception, every later
  raise in the pass returns on `did_jump`.

The key is the IVG, not the SIC pin, because the raise event carries only
the IVG. Different IVGs, other event types, and raises in later passes are
untouched. The current re-forward, re-latch-while-in-service and
re-pend-at-RTI behaviour is preserved exactly. Making it match the documented
level-sensitive semantics is a separate, guest-visible change.

### Evidence

`tests/interrupt_raise_dedupe_tests.cpp` runs the ten investigation scenarios
with and without de-duplication and compares the full per-block trace: PC,
RETI, SEQSTAT, IMASK, IPEND, ILAT, SIC ISR/IMASK/IAR and ISR entry order.
All traces are identical. It also checks the pass scope directly.

v241 functional firmware A/B (same script, concurrent runs), per guest second:
- **Queued raise events:** 18.0–22.8M → 3.7–4.8M.
- **Cadence:** SPORT, DMA, IRQ28 and core timer cadence identical. ISR entry
  rates per IVG agree within run-to-run variation; host-time devices keep
  firmware runs nondeterministic.
- **Throughput:** unchanged within noise (−1% to +3% blocks per CPU-second).
  The remaining per-forward cost (`SIC::ForwardInterrupts`, the forward
  callback and the event-queue lock per request) dominates. The worthwhile
  reduction is forwarding only on real transitions, which is guest-visible
  and separate.
