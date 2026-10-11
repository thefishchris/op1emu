# Continuous integration

The workflow [`.github/workflows/ci.yml`](../.github/workflows/ci.yml)
builds the emulator and runs the public test suite. It needs no firmware,
NAND, OTP or boot ROM data, and must never be given any.

## When it runs

- pull requests that target `main`;
- pushes to `development/**` branches;
- manually (`workflow_dispatch`).

## What it does

One job on a GitHub-hosted runner:
1. **Check out** the repository with its Bcore submodule (`ext/bcore`),
   recursively, at the commit the repository pins.
2. **Verify** that the submodule is checked out at exactly the pinned
   commit, that the `.gitmodules` URL is the Bcore fork
   (`https://github.com/thefishchris/bcore.git`), and that it is the only
   submodule.
3. **Install** the toolchain and build dependencies from the Ubuntu
   archive.
4. **Record** the tool versions in the log, and fail unless LLVM is
   exactly 15.0.7.
5. **Configure and build** all default targets with the same commands as
   the README:
   ```sh
   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DLLVM_DIR=/usr/lib/llvm-15/lib/cmake/llvm
   cmake --build build
   ```
   The build includes the GUI (`op1emu`), the headless runner
   (`op1emu-headless`) and the tests. The step fails if either program is
   missing.
6. **Check the test count:** fail unless CTest registers exactly 28
   tests. Several tests are only registered when CMake finds Python, so a
   missing interpreter would otherwise shrink the suite silently. Update
   `EXPECTED_TEST_COUNT` in the workflow when tests are added or removed.
7. **Run the suite:** `ctest --test-dir build --output-on-failure`.

## Not covered

- **Firmware boot:** v241 boot and controls need the user-supplied inputs
  (`AGENTS.md`), so they are not tested in CI. Run the headless regression
  runner locally ([headless regression](headless-regression.md)).
- **GUI interaction:** the GUI is built, but not driven interactively.
- **Bcore assembler suite:** Bcore's own suite needs the Blackfin
  cross-toolchain and is not run here. Bcore's two shift tests are part of
  the 28.
- **Sanitizer and warning builds:** none are configured yet.

## Versions

| Item | Version | How it is fixed |
| --- | --- | --- |
| Runner image | `ubuntu-24.04` | Named explicitly (not `ubuntu-latest`) |
| `actions/checkout` | v7.0.1 | Pinned to commit `3d3c42e5aac5ba805825da76410c181273ba90b1` |
| LLVM | 15.0.7 (`llvm-15-dev`, Ubuntu `1:15.0.7-14build3` when this was written) | Ubuntu 24.04 archive; the version is checked in the workflow |
| Compiler | GCC 13 (`g++`, Ubuntu 24.04 default) | Ubuntu archive |
| CMake, Ninja, Python | Ubuntu 24.04 archive (CMake 3.28, Ninja 1.11, Python 3.12) | Ubuntu archive |
| GUI build libraries | `libgl-dev`, `libx11-dev`, `libxrandr-dev`, `libxinerama-dev`, `libxcursor-dev`, `libxi-dev` (for GLFW's X11 backend) | Ubuntu archive |
| Fetched dependencies | spdlog, GLFW, nlohmann/json, stb, cqueue, uvw/libuv, miniaudio | Pinned tags or commits in `CMakeLists.txt` |
| Bcore and its test fixtures | Submodule commit; `bfin_sim` fixtures | Pinned commits |

**Archive packages are not pinned** to exact Ubuntu package revisions, because
the archive drops superseded revisions. Each run logs the installed
versions, and the LLVM check fails the run if the LLVM release changes.

**Hosted runner differences:** hosted runners preinstall newer CMake and
compilers. The workflow installs and uses the archive packages listed
above. The versions actually used are in each run's "Record tool versions"
step.

**Updating `actions/checkout`:** replace both the commit SHA and the
version comment, taking the SHA from the release tag on
`github.com/actions/checkout`.

## Permissions and security

- **Token:** read-only `contents: read`.
- **Credentials:** checkout does not persist credentials
  (`persist-credentials: false`).
- **Secrets:** none are used. Pull requests from forks run with the same
  read-only token.

## Activating it on a fork

GitHub disables Actions on forked repositories by default.
1. Enable Actions for the repository: Settings → Actions → General. If the
   Actions tab asks, confirm the workflows.
2. Push a `development/**` branch, or open a pull request to `main`, and
   check that the run succeeds.
3. **Optionally, after a successful run:** add the "Build and test (Ubuntu
   24.04, LLVM 15)" check as a required status check in `main`'s branch
   protection.
