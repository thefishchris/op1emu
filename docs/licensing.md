# Licensing

This page records how op1emu and the components it is built from are
licensed, and what anyone distributing it has to do. It is the project's
understanding, not legal advice. Points that are still open are listed
under [Open questions](#open-questions).

## Summary

| What | Licence |
| --- | --- |
| This repository's own files | MIT ([`LICENSE`](../LICENSE)) |
| The Bcore CPU core (`ext/bcore` submodule) | GPL-3.0-or-later, with MIT-licensed parts (see Bcore's `NOTICE`) |
| `op1emu` and `op1emu-headless` as built | A combined work that includes Bcore. The project distributes it under **GPL-3.0-or-later**. |
| Third-party libraries fetched at configure time | Their own licences (table below), including LGPL-3.0 for cqueue |

op1emu is **not** distributed as an MIT-only program. Its own files are MIT,
but the programs built from them include GPL-licensed code.

## This repository's own files

The files in this repository are offered under the MIT License: see
[`LICENSE`](../LICENSE), "Copyright (c) 2026 op1emu". This fork's
contributions are offered under the same terms.

The exceptions are the GUI artwork (`gui/ui.png`, `screenshot.png`). It is
based on third-party MIT-licensed designs, and the MIT grants do not cover
Teenage Engineering's design or symbols. See
[GUI artwork attribution](artwork-attribution.md).

No GNU-derived code has been identified in this repository's own files.
They have not had the file-by-file provenance review that Bcore had.

## The Bcore CPU core

`ext/bcore` is a fork of [op1emu/bcore](https://github.com/op1emu/bcore),
kept at [thefishchris/bcore](https://github.com/thefishchris/bcore).
Upstream published Bcore under the MIT License. However, its own comments
and documentation record that parts of it were developed by following, and
in places translating, the GNU Blackfin simulator and the binutils Blackfin
disassembler. Those are GPL-3.0-or-later, "Copyright Free Software
Foundation, Inc.", with the simulator files "Contributed by Analog Devices,
Inc.". A comparison confirmed a close structural correspondence for the
Core Event Controller.

The fork therefore:
- distributes Bcore under **GPL-3.0-or-later** (`COPYING`);
- preserves the upstream MIT notice (`LICENSE.MIT`), which still applies to
  the files classified as original upstream work and to the fork's
  additions;
- classifies every Bcore file as GNU-derived, uncertain (treated as GPL),
  original upstream (MIT) or fork addition (MIT), with the evidence, in
  `docs/provenance.md` and `REUSE.toml`;
- explains all of this in `NOTICE`.

Bcore is **not** a clean-room or independent reimplementation of the GNU
sources. Whether particular files are derivative works in law is not
settled. The fork takes the conservative position and treats the uncertain
files as GPL until their origin is established.

## The built programs

`op1emu` (GUI) and `op1emu-headless` link Bcore. A program that includes
GPL-3.0-or-later code is distributed under GPL-3.0-or-later as a whole. The
MIT terms of this repository's files allow that, and those files stay
available under MIT on their own.

**Consequence:** anyone distributing these programs, as source or binaries,
must follow GPL-3.0-or-later for the combined work. That includes providing
the Corresponding Source (below).

## Third-party components

These components are fetched at configure time, at the versions pinned in
[`CMakeLists.txt`](../CMakeLists.txt) and Bcore's build, and are not stored
in this repository. Each licence is the one in the fetched source tree.

| Component | Version | Licence | Used by |
| --- | --- | --- | --- |
| Bcore (`ext/bcore`, submodule) | pinned commit | GPL-3.0-or-later, with MIT parts | both programs |
| LLVM | 15 (supplied by you) | Apache-2.0 WITH LLVM-exception | both programs, through Bcore |
| spdlog | v1.11.0 | MIT (bundles {fmt}, MIT) | both programs |
| nlohmann/json | v3.12.0 | MIT | both programs |
| GLFW | 3.3.8 | zlib | GUI |
| stb (`stb_image`) | pinned commit | MIT or public domain (your choice) | GUI |
| cqueue (torrentg/cqueue) | 1.0.6 | **LGPL-3.0** | GUI |
| uvw | v3.4.0_libuv_v1.48 | MIT | GUI |
| libuv (fetched by uvw) | 1.48 | MIT (two notices: libuv contributors; Joyent and Node contributors, in `LICENSE-extra`) | GUI |
| miniaudio | 0.11.21 | Public domain (Unlicense) or MIT-0 (your choice) | GUI |

Bcore's own test suite also fetches the GNU simulator's assembler tests
(GPL-3.0-or-later) from [op1emu/bfin_sim](https://github.com/op1emu/bfin_sim).
They are only used to test Bcore and are not built into either program.

### cqueue (LGPL-3.0)

cqueue is a header-only C++ template library. The in-tree wrapper
(`ext/cqueue/cqueue.{h,cpp}`, MIT) instantiates it, so its code is compiled
into the GUI program `op1emu`. `op1emu-headless` does not use it.

LGPL-3.0 is GPL-3.0 plus additional permissions, so it can be part of a
GPL-3.0-or-later combined work. Its own requirements still apply when the
GUI is distributed as a binary:
- ship the LGPL-3.0 text and the GPL-3.0 text it builds on;
- keep the notices cqueue's source carries (its `LICENSE` is the LGPL-3.0
  text; no separate copyright line was found in the version fetched);
- give recipients a way to rebuild the program with a modified cqueue.

Providing the full Corresponding Source of the GPL combined work, with build
instructions, is expected to meet the last point. That has not been
confirmed by legal review.

## User-supplied inputs

The emulator needs device and vendor data that this project does not contain
or distribute:
- the boot ROM image ([docs/boot-rom.md](boot-rom.md));
- the OP-1 NAND image;
- the OTP image ([docs/otp-image.md](otp-image.md));
- the OP-1 firmware.

Their owners' rights apply to them. Do not add them to this repository or
include them in a distribution.

## Distributing op1emu

**As source** (a Git repository or archive): keep `LICENSE`, the Bcore
submodule with its `COPYING`, `LICENSE.MIT` and `NOTICE`, and the artwork
attribution. The source obligations of GPL-3.0 already apply here. Bcore must
not be presented as MIT-only.

**As binaries:** more is needed, because a binary contains code from every
component above but none of their licence files or source.
1. **Licence texts and notices:** ship them with the binary:
   - the GPL-3.0 text;
   - the LGPL-3.0 text (cqueue);
   - the MIT texts with their copyright lines (op1emu, Bcore upstream,
     spdlog and {fmt}, nlohmann/json, uvw, libuv, stb, the artwork);
   - the zlib licence (GLFW);
   - Apache-2.0 for LLVM;
   - Bcore's `NOTICE`.
2. **Corresponding Source:** provide the complete source for the exact
   build. GPL-3.0 section 6 sets out how: alongside the binary, from a
   network server, or by a written offer. That source is:
   - this repository at the built commit;
   - Bcore at the pinned submodule commit;
   - every fetched dependency at the version listed above;
   - the build scripts.

   Every fetched dependency is pinned to a tag or commit. Tags can move, so
   record the resolved commits for a release.
3. **LLVM:** Bcore links LLVM 15 either as the shared library
   `libLLVM-15.so` or as static component libraries, depending on how that
   LLVM was built.
   - If a release ships LLVM, in either form, it must carry LLVM's licence
     and notices.
   - If it relies on the user's installed LLVM, say so in the release
     notes.
4. **No private inputs:** a release must not contain any ROM, NAND, OTP or
   firmware data, nor any file derived from them.

## Open questions

These are not settled. None of them changes the GPL-3.0-or-later
distribution described above, which is the conservative choice.
- Whether the Bcore files classified as GNU-derived or uncertain are
  derivative works in law. The fork treats them as GPL either way.
- How the AI-assisted generation of upstream Bcore, with the GNU sources
  consulted, affects authorship and copyright.
- Whether shipping full Corresponding Source meets every cqueue (LGPL-3.0)
  requirement for binaries.
- How far the MIT grants for the GUI artwork extend to the edited render,
  and Teenage Engineering's position on depicting its design.

Answers from the upstream Bcore author or a legal review will be recorded in
Bcore's `docs/provenance.md` and on this page.
