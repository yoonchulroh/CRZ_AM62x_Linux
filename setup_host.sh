#!/bin/bash
# setup_host.sh -- one-time install of the host packages the CRZ AM62x flow
# needs. The host is assumed to be Ubuntu 22.04 (x86_64); newer releases work too.
#
# Usage:  ./setup_host.sh
#
# Covers every step in README.md after the TI SDK installer:
#   apply_change.sh / rt_change.sh    patch, sed, coreutils
#   build.sh uboot / falcon           U-Boot (binman, mkimage, mkeficapsule), TF-A
#   build.sh kernel / kernel-rt       Linux kernel, fdtoverlay
#   build.sh aer                      TensorFlow Lite download (cmake/ninja come
#                                     from the SDK's linux-devkit)
#   build.sh rootfs                   Ubuntu base download, arm64 chroot via qemu
#   image/create-sdcard_ubuntu.sh     partitioning and filesystem tools
#   image/make_auto_eMMC_fuse_SD.sh   same, plus udevadm/lsblk/findmnt
#   UART console                      picocom
#
# The cross compilers are not installed here: the stock TI SDK 12.01 brings
# them in linux-devkit/ and k3r5-devkit/, and Rules.make points at those.
#
# Needs sudo and internet access. Safe to run again; installed packages are
# left as they are.

set -euo pipefail

die() { echo "error: $*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# Host check
# ---------------------------------------------------------------------------
[ -r /etc/os-release ] || die "cannot read /etc/os-release"
. /etc/os-release
[ "${ID:-}" = ubuntu ] || die "this script is for Ubuntu (found ${ID:-unknown})"
[ "$(uname -m)" = x86_64 ] ||
	die "the TI SDK host tools are x86_64 only (found $(uname -m))"

UBUNTU_MAJOR=${VERSION_ID%%.*}
[ "$UBUNTU_MAJOR" -ge 22 ] ||
	die "Ubuntu $VERSION_ID is too old; use 22.04 or newer"

if [ "$(id -u)" = 0 ]; then
	SUDO=""
else
	command -v sudo >/dev/null || die "sudo not found"
	SUDO=sudo
fi

echo "==> Ubuntu $VERSION_ID ($VERSION_CODENAME)"

# ---------------------------------------------------------------------------
# Packages
# ---------------------------------------------------------------------------
PKGS_COMMON="
	ca-certificates curl wget git
	tar gzip xz-utils file
	coreutils sed grep findutils
"

# apply_change.sh, rt_change.sh
PKGS_APPLY="
	patch
"

# U-Boot 2026.01 (incl. host tools: mkimage with FIT signing, mkeficapsule,
# binman with the TI board-config/signing etypes), TF-A, Linux 6.18.
# pylibfdt is built inside the U-Boot tree with swig (python3-libfdt is not in
# Ubuntu 22.04).
PKGS_BUILD="
	build-essential make bc bison flex
	libssl-dev openssl libgnutls28-dev uuid-dev libelf-dev
	libncurses-dev pkg-config
	device-tree-compiler
	python3 python3-dev python3-setuptools
	python3-pyelftools python3-yaml python3-jsonschema yamllint
	swig
	cpio kmod rsync
"

# build.sh rootfs: chroot into the arm64 Ubuntu base. The rootfs script copies
# /usr/bin/qemu-aarch64 into the chroot, so that binary has to be statically
# linked. Up to 24.04 only qemu-user-static is; from 25.04 qemu-user itself is
# static and qemu-user-static no longer exists.
if [ "$UBUNTU_MAJOR" -le 24 ]; then
	PKGS_ROOTFS="qemu-user-static binfmt-support"
else
	PKGS_ROOTFS="qemu-user qemu-user-binfmt"
fi
PKGS_ROOTFS="$PKGS_ROOTFS sudo"

# image/create-sdcard_ubuntu.sh, image/make_auto_eMMC_fuse_SD.sh, UART console
PKGS_FLASH="
	fdisk util-linux dosfstools e2fsprogs udev
	picocom
"

PKGS=$(echo $PKGS_COMMON $PKGS_APPLY $PKGS_BUILD $PKGS_ROOTFS $PKGS_FLASH)

echo "==> updating package lists"
$SUDO apt-get update

echo "==> installing packages"
$SUDO env DEBIAN_FRONTEND=noninteractive apt-get install -y $PKGS

# ---------------------------------------------------------------------------
# Register the arm64 binfmt handler now, so the rootfs chroot works without
# a reboot
# ---------------------------------------------------------------------------
echo "==> registering qemu-aarch64 binfmt handler"
if [ -d /run/systemd/system ]; then
	$SUDO systemctl restart systemd-binfmt || true
fi
if command -v update-binfmts >/dev/null; then
	$SUDO update-binfmts --enable qemu-aarch64 2>/dev/null || true
fi

# ---------------------------------------------------------------------------
# Verify
# ---------------------------------------------------------------------------
echo "==> checking"
FAIL=0
check() {
	if "$@" >/dev/null 2>&1; then
		echo "    ok      $DESC"
	else
		echo "    MISSING $DESC"
		FAIL=1
	fi
}

for c in patch make gcc bc bison flex swig fdtoverlay openssl cpio \
	 curl wget sfdisk fdisk mkfs.vfat mkfs.ext3 xz udevadm lsblk \
	 findmnt picocom; do
	DESC=$c check command -v $c
done

DESC="python3 modules (elftools, yaml, jsonschema, yamllint)" \
	check python3 -c 'import elftools, yaml, jsonschema, yamllint'

QEMU=/usr/bin/qemu-aarch64
[ -x /usr/bin/qemu-aarch64-static ] && QEMU=/usr/bin/qemu-aarch64-static
DESC="$QEMU is statically linked" \
	check sh -c "file -L $QEMU | grep -q 'static'"
DESC="qemu-aarch64 binfmt handler enabled" \
	check grep -qx enabled /proc/sys/fs/binfmt_misc/qemu-aarch64

if [ "$FAIL" != 0 ]; then
	echo
	die "some checks failed; see MISSING above"
fi

cat <<EOF

Host is ready. Next:
  1. Install the TI SDK (ti-processor-sdk-linux-am62xx-evm-12.01.00.05.03)
  2. ./apply_change.sh /path/to/ti-processor-sdk-linux-am62xx-evm-12.01.00.05.03
  3. cd /path/to/ti-processor-sdk-linux-am62xx-evm-12.01.00.05.03 && ./build.sh all
EOF
