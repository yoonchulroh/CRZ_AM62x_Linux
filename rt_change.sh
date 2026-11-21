#!/bin/sh
#
# rt_change.sh -- turn a TI SDK into the RT variant of the CRZ AM62x tree
# (am62x_rt), whose kernel is built with TI's PREEMPT_RT config fragment.
#
# Usage:  ./rt_change.sh /path/to/ti-processor-sdk-linux-am62xx-evm-12.01.00.05.03
#
# The path can be the stock (non-RT) TI SDK or a tree apply_change.sh has
# already converted (am62x_20260918). This script first runs apply_change.sh,
# which does nothing on an already converted tree, then:
#   1. Rules.make  adds RT_FRAGMENT=ti_rt.config crz_rt.config, which the
#                  kernel makerules pass to "make defconfig"
#   2. build.sh    renames the "kernel" target to "kernel-rt"; "kernel" now
#                  fails with a message
#   3. adds board-support/ti-linux-kernel/kernel/configs/crz_rt.config, which
#      sets CONFIG_LOCALVERSION="-rt" so "uname -r" shows 6.18.38-rt
#   4. points TI_SDK_PATH in Rules.make at the given path
#
# The RT changes are checked before any of them is made. Re-running on an RT
# tree does nothing. If the tree already has a kernel build, the next
# "./build.sh kernel-rt" rebuilds it from scratch (the kernel config changes).

set -e

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

HERE=$(cd "$(dirname "$0")" && pwd)
RT_CONFIG=board-support/ti-linux-kernel/kernel/configs/crz_rt.config

[ -n "$SDK" ] || die "no SDK path given. Try: $0 --help"
[ -d "$SDK" ] || die "not a directory: $SDK"
SDK=$(cd "$SDK" && pwd)

###############################################################################
# Sanity-check the target
###############################################################################

case "$SDK/" in
"$HERE"/*) die "$SDK is this kit's directory (or inside it), not the SDK" ;;
esac
case "$HERE/" in
"$SDK"/*) die "$SDK contains this kit; give the SDK directory itself" ;;
esac

# Convert a stock SDK first; on an already converted tree (including an RT
# one) apply_change.sh does nothing. It stops the script if it fails.
echo "==> running apply_change.sh"
"$HERE/apply_change.sh" "$SDK" || die "apply_change.sh failed on $SDK"
[ -d "$SDK/board-support/ti-u-boot" ] ||
	die "apply_change.sh did not convert $SDK"
echo

for f in Rules.make Makefile build.sh; do
	[ -f "$SDK/$f" ] || die "$SDK does not look like a CRZ AM62x tree (no $f)"
done
grep -q '^export TI_SDK_PATH?=' "$SDK/Rules.make" ||
	die "$SDK/Rules.make has no 'export TI_SDK_PATH?=' line"
grep -Eq '^PLATFORM\??=am62xx-evm' "$SDK/Rules.make" ||
	die "$SDK/Rules.make is not for am62xx-evm"
[ -w "$SDK" ] && [ -w "$SDK/Rules.make" ] && [ -w "$SDK/build.sh" ] ||
	die "no write permission on $SDK (run as the user that owns the SDK)"

if grep -q '^RT_FRAGMENT=' "$SDK/Rules.make"; then
	echo "$SDK is already an RT tree (RT_FRAGMENT in Rules.make). Nothing to do."
	exit 0
fi

[ -d "$SDK/board-support/ti-linux-kernel/kernel/configs" ] ||
	die "$SDK has no board-support/ti-linux-kernel; not a converted CRZ tree"
[ -f "$SDK/board-support/ti-linux-kernel/kernel/configs/ti_rt.config" ] ||
	die "$SDK has no ti_rt.config in the kernel; not a 12.01 kernel tree?"
grep -q '^kernel)' "$SDK/build.sh" ||
	die "$SDK/build.sh has no 'kernel' target; not the CRZ build.sh"
[ ! -e "$SDK/$RT_CONFIG" ] ||
	die "$SDK/$RT_CONFIG already exists; refusing to overwrite it"

###############################################################################
# Check the patch first, so a failure leaves the tree untouched
###############################################################################

PATCH=$(mktemp)
trap 'rm -f "$PATCH"' EXIT
cat > "$PATCH" <<'EOF_RT_PATCH'
--- a/Rules.make
+++ b/Rules.make
@@ -36,2 +36,6 @@
 ### PLATFORM CONFIG ###
+
+### rt fragment config ###
+RT_FRAGMENT=ti_rt.config crz_rt.config
+
 #platform
--- a/build.sh
+++ b/build.sh
@@ -9,7 +9,7 @@
 
 START_TIME=`date +%s`
 
-# falcon mode: pack the kernel Image and DTB staged by "kernel" into the
+# falcon mode: pack the kernel Image and DTB staged by "kernel-rt" into the
 # fitImage the R5 SPL loads. U-Boot proper is skipped, so the display overlay
 # it would apply (name_overlays) is merged into the DTB here instead. The DTB
 # carries no bootargs: the R5 SPL generates them with root=PARTUUID of the
@@ -21,7 +21,7 @@
 	for f in $BOOT_DIR/Image $BOOT_DIR/dtb/ti/k3-am625-sk.dtb \
 		 $BOOT_DIR/dtb/ti/k3-am625-sk-bsd101wx1-300.dtbo ; do
 		if [ ! -f $f ] ; then
-			echo "$f not found, run ./build.sh kernel first"
+			echo "$f not found, run ./build.sh kernel-rt first"
 			exit 1
 		fi
 	done
@@ -119,7 +119,7 @@
 	cd ..
 	rm -fr ./image_temp
 	;;
-kernel)
+kernel-rt)
 	mkdir -p $MODULES_DIR
 	mkdir image_temp
 	make linux
@@ -145,6 +145,11 @@
 	cd ..
 	rm -fr image_temp
 	;;
+kernel)
+	# this tree builds only the RT kernel (RT_FRAGMENT in Rules.make)
+	echo "This is the RT tree: use ./build.sh kernel-rt"
+	exit 1
+	;;
 rootfs)
 	cd ubuntu-26.04
 	./create_ubuntu26.04.sh
EOF_RT_PATCH

echo "==> checking $SDK"
patch --dry-run -s -d "$SDK" -p1 < "$PATCH" ||
	die "the RT changes do not apply cleanly to Rules.make/build.sh. Tree left untouched."

###############################################################################
# Apply
###############################################################################

echo "==> patching Rules.make and build.sh"
patch -s --no-backup-if-mismatch -d "$SDK" -p1 < "$PATCH"

echo "==> adding $RT_CONFIG"
cat > "$SDK/$RT_CONFIG" <<'EOF_RT_CONFIG'
# CRZ RT build: tag the kernel release so "uname -r" shows 6.18.38-rt
# (modules go to /lib/modules/6.18.38-rt). Applied after ti_rt.config via
# RT_FRAGMENT in Rules.make.
CONFIG_LOCALVERSION="-rt"
EOF_RT_CONFIG

echo "==> setting TI_SDK_PATH to $SDK"
sed -i "s|^export TI_SDK_PATH?=.*|export TI_SDK_PATH?=$SDK|" "$SDK/Rules.make"

echo
echo "Done. Build the RT kernel with:"
echo "    cd $SDK"
echo "    ./build.sh kernel-rt"
