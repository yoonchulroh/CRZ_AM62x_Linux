#!/bin/sh
#
# apply_change.sh -- turn a stock TI Processor SDK into the CRZ AM62x tree
# (am62x_20260918).
#
# Usage:  ./apply_change.sh /path/to/ti-processor-sdk-linux-am62xx-evm-12.01.00.05.03
#
# Steps, in order:
#   1. patches/*.patch     modify stock files
#   2. deleted_files.txt   delete stock files/dirs that am62x_20260918 lacks
#   3. rename versioned source dirs to the names am62x_20260918 uses
#   4. overlay/            copy in files that are not in the stock SDK
#   5. point TI_SDK_PATH in Rules.make at the given SDK path
#
# Everything is checked before anything is touched, so a failed check leaves
# the SDK as it was. Re-running on an already converted SDK does nothing.

set -e

SDK_VERSION="12.01.00.05.03"
HERE=$(cd "$(dirname "$0")" && pwd)

# stock name -> am62x_20260918 name, under board-support/
RENAMES="ti-u-boot-2026.01+git:ti-u-boot
ti-linux-kernel-6.18.38+git-ti:ti-linux-kernel
trusted-firmware-a-2.15+git:trusted-firmware"

SDK=""
for arg in "$@"; do
	case "$arg" in
	-h|--help)
		sed -n '2,/^$/p' "$0" | grep '^#' | sed 's/^# \{0,1\}//'
		exit 0
		;;
	*) SDK="$arg" ;;
	esac
done

die() { echo "error: $*" >&2; exit 1; }

# Entries of deleted_files.txt, without comments and blank lines.
deleted_paths() { grep -v -e '^#' -e '^[[:space:]]*$' "$HERE/deleted_files.txt"; }

[ -n "$SDK" ] || die "no SDK path given. Try: $0 --help"
[ -d "$SDK" ] || die "not a directory: $SDK"
SDK=$(cd "$SDK" && pwd)

###############################################################################
# Sanity-check the target
###############################################################################

[ -f "$SDK/Rules.make" ] || die "$SDK does not look like a TI SDK (no Rules.make)"

# Refuse to double-apply: the renamed u-boot dir only exists after conversion.
if [ -d "$SDK/board-support/ti-u-boot" ] &&
   [ ! -e "$SDK/board-support/ti-u-boot-2026.01+git" ]; then
	echo "This SDK already has the CRZ changes applied (board-support/ti-u-boot"
	echo "exists). Nothing to do."
	exit 0
fi

# The manifest carries no version string, but a stock Rules.make ships with
# TI_SDK_PATH defaulting to the versioned install directory. Fall back to the
# directory name if that has already been edited.
if ! grep -qs "$SDK_VERSION" "$SDK/Rules.make" &&
   ! echo "$SDK" | grep -qs "$SDK_VERSION"; then
	echo "warning: cannot confirm this is SDK $SDK_VERSION."
	echo "         These changes were generated against $SDK_VERSION and may"
	echo "         not apply cleanly to another release."
fi

###############################################################################
# Check everything first, so a failure leaves the SDK untouched
###############################################################################

echo "==> checking $SDK"

for p in "$HERE"/patches/*.patch; do
	patch --dry-run -s -d "$SDK" -p1 < "$p" ||
		die "$(basename "$p") does not apply cleanly. SDK left untouched."
done

for r in $RENAMES; do
	from=${r%%:*}; to=${r#*:}
	[ -d "$SDK/board-support/$from" ] ||
		die "board-support/$from not found. SDK left untouched."
	[ ! -e "$SDK/board-support/$to" ] ||
		die "board-support/$to already exists. SDK left untouched."
done

deleted_paths | while IFS= read -r p; do
	case "$p" in
	/*|*..*) die "refusing unsafe path in deleted_files.txt: $p" ;;
	esac
	[ -e "$SDK/$p" ] || [ -L "$SDK/$p" ] ||
		echo "    note: $p already absent"
done

###############################################################################
# Apply
###############################################################################

echo "==> applying patches"
for p in "$HERE"/patches/*.patch; do
	echo "    $(basename "$p")"
	patch -s -d "$SDK" -p1 < "$p"
done

echo "==> deleting files not in am62x_20260918"
deleted_paths | while IFS= read -r p; do
	echo "    $p"
	rm -rf -- "$SDK/$p"
done

echo "==> renaming source directories"
for r in $RENAMES; do
	from=${r%%:*}; to=${r#*:}
	echo "    board-support/$from -> $to"
	mv -- "$SDK/board-support/$from" "$SDK/board-support/$to"
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
echo "    ./build.sh all"
echo
echo "See README.md for flashing the SD card."
