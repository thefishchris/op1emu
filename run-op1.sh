#!/bin/sh
# Launch the OP-1 emulator with an explicit execution profile
# (docs/emulator-profiles.md).
#
#   ./run-op1.sh --profile strict|functional --nand NAND.img --otp OTP.bin \
#       --boot-rom ROM.bin [--headless] [--build-dir DIR] [-- extra emulator arguments]
#
# NAND and OTP may also come from OP1EMU_NAND and OP1EMU_OTP_SOURCE, and the
# 32768-byte boot ROM image (docs/boot-rom.md) from OP1EMU_BOOTROM_FILE. The ROM
# is only read, never copied.
# Source images are never modified: NAND is opened with --nand-snapshot and
# the OTP is copied into a fresh private run directory, which is printed and
# kept for inspection. Other OP1EMU_* variables pass through and follow the
# profile precedence rules.
set -eu

die() { echo "run-op1.sh: $*" >&2; exit 2; }

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build_dir=$repo_root/build
profile=
nand=${OP1EMU_NAND:-}
otp=${OP1EMU_OTP_SOURCE:-}
boot_rom=${OP1EMU_BOOTROM_FILE:-}
headless=0

while [ "$#" -gt 0 ]; do
    case $1 in
        --profile) [ "$#" -ge 2 ] || die "--profile needs a value"; profile=$2; shift 2 ;;
        --nand) [ "$#" -ge 2 ] || die "--nand needs a path"; nand=$2; shift 2 ;;
        --otp) [ "$#" -ge 2 ] || die "--otp needs a path"; otp=$2; shift 2 ;;
        --boot-rom) [ "$#" -ge 2 ] || die "--boot-rom needs a path"; boot_rom=$2; shift 2 ;;
        --build-dir) [ "$#" -ge 2 ] || die "--build-dir needs a path"; build_dir=$2; shift 2 ;;
        --headless) headless=1; shift ;;
        --help|-h) sed -n '2,14p' "$0"; exit 0 ;;
        --) shift; break ;;
        *) die "unknown argument: $1 (emulator arguments go after --)" ;;
    esac
done

case $profile in
    strict|functional) ;;
    '') die "--profile strict|functional is required" ;;
    *) die "--profile must be strict or functional" ;;
esac
[ -n "$nand" ] && [ -f "$nand" ] || die "NAND image not found (use --nand or OP1EMU_NAND)"
[ -n "$otp" ] && [ -f "$otp" ] || die "OTP image not found (use --otp or OP1EMU_OTP_SOURCE)"
[ -n "$boot_rom" ] && [ -f "$boot_rom" ] || die "boot ROM image not found (use --boot-rom or OP1EMU_BOOTROM_FILE; see docs/boot-rom.md)"

nand=$(CDPATH= cd -- "$(dirname -- "$nand")" && pwd)/$(basename -- "$nand")
boot_rom=$(CDPATH= cd -- "$(dirname -- "$boot_rom")" && pwd)/$(basename -- "$boot_rom")
run_dir=$(mktemp -d "${TMPDIR:-/tmp}/op1emu-run.XXXXXX")
cp -- "$otp" "$run_dir/otp.bin"
# The emulator reads gui/ui.json and otp.bin relative to its working directory.
ln -s "$repo_root/gui" "$run_dir/gui"
echo "run-op1.sh: profile=$profile run-dir=$run_dir (private OTP copy, NAND snapshot)" >&2

export OP1EMU_PROFILE="$profile"
export OP1EMU_OTP_FILE="$run_dir/otp.bin"
export OP1EMU_BOOTROM_FILE="$boot_rom"
cd "$run_dir"
if [ "$headless" -eq 1 ]; then
    exec "$build_dir/op1emu-headless" "$nand" --profile "$profile" --nand-snapshot \
        --ui-config "$repo_root/gui/ui.json" "$@"
fi
exec "$build_dir/op1emu" "$nand" --nand-snapshot "$@"
