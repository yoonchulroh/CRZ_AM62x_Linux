#!/bin/sh
#
# apply_change.sh -- apply the CRZ AM62x changes to a stock TI Processor SDK.
#
# Usage:  ./apply_change.sh /path/to/ti-processor-sdk-linux-am62xx-evm-12.01.00.05.03
#
# Idempotent: re-running on an already-patched SDK detects this and stops
# without touching anything. Use --force to re-apply anyway.

set -e

SDK_VERSION="12.01.00.05.03"
HERE=$(cd "$(dirname "$0")" && pwd)

FORCE=0
SDK=""
for arg in "$@"; do
	case "$arg" in
	--force) FORCE=1 ;;
	-h|--help)
		sed -n '2,/^$/p' "$0" | grep '^#' | sed 's/^# \{0,1\}//'
		exit 0
		;;
	*) SDK="$arg" ;;
	esac
done

die() { echo "error: $*" >&2; exit 1; }

[ -n "$SDK" ] || die "no SDK path given. Try: $0 --help"
[ -d "$SDK" ] || die "not a directory: $SDK"
SDK=$(cd "$SDK" && pwd)

###############################################################################
# Sanity-check the target
###############################################################################

[ -f "$SDK/Rules.make" ] || die "$SDK does not look like a TI SDK (no Rules.make)"

# The SDK Makefiles locate u-boot with a glob, so the exact suffix may vary
# between point releases; resolve it rather than hard-coding it.
UBOOT_DIR=$(ls -d "$SDK"/board-support/ti-u-boot* 2>/dev/null | head -1)
[ -n "$UBOOT_DIR" ] || die "no board-support/ti-u-boot* found under $SDK"

# The manifest carries no version string, but a stock Rules.make ships with
# TI_SDK_PATH defaulting to the versioned install directory. Fall back to the
# directory name if that has already been edited.
if ! grep -qs "$SDK_VERSION" "$SDK/Rules.make" &&
   ! echo "$SDK" | grep -qs "$SDK_VERSION"; then
	echo "warning: cannot confirm this is SDK $SDK_VERSION."
	echo "         These patches were generated against $SDK_VERSION and may"
	echo "         not apply cleanly to another release."
fi

###############################################################################
# Refuse to double-apply
###############################################################################

if [ "$FORCE" -eq 0 ] &&
   grep -qs 'CONFIG_NAND_BOOT=y' "$UBOOT_DIR/configs/am62x_evm_a53_defconfig"; then
	echo "This SDK already has the CRZ changes applied (NAND boot present in"
	echo "am62x_evm_a53_defconfig). Nothing to do. Use --force to re-apply."
	exit 0
fi

###############################################################################
# Dry run everything first, so a failure leaves the SDK untouched
###############################################################################

echo "==> checking patches against $SDK"
for p in "$HERE"/patches/*.patch; do
	patch --dry-run -s -d "$SDK" -p1 < "$p" ||
		die "$(basename "$p") does not apply cleanly. SDK left untouched."
done

###############################################################################
# Apply
###############################################################################

echo "==> applying patches"
for p in "$HERE"/patches/*.patch; do
	echo "    $(basename "$p")"
	patch -s -d "$SDK" -p1 < "$p"
done

echo "==> copying overlay files"
cp -a "$HERE"/overlay/. "$SDK"/

###############################################################################
# Localise the hard-coded paths
###############################################################################

echo "==> setting TI_SDK_PATH to $SDK"
sed -i "s|^export TI_SDK_PATH?=.*|export TI_SDK_PATH?=$SDK|" "$SDK/Rules.make"

echo
echo "Done. Next:"
echo "    cd $SDK"
echo "    ./build.sh uboot"
echo
echo "Note: build.sh also copies boot images to /nfs/am62x (IMAGE_DIR2)."
echo "Edit that line in $SDK/build.sh if you do not have that NFS export."
