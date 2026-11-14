#!/bin/sh

# Usage: sudo ./make_auto_eMMC_fuse_SD.sh [/dev/sdX2]
# Writes a bootable SD card with create-sdcard_ubuntu.sh, then adds the eMMC
# fusing images and auto-start to it.
# Without an argument, the SD rootfs partition is found by its "rootfs" label
# (set by create-sdcard_ubuntu.sh), excluding the host's own root disk.

ROOTFS_DIR=./sd_rootfs

if [ "$(id -u)" != 0 ]; then
	echo "must run script with sudo"
	exit 1
fi

cd "$(dirname "$0")"

./create-sdcard_ubuntu.sh || exit 1
udevadm settle

ROOTFS_DEV=$1
if [ -z "$ROOTFS_DEV" ]; then
	HOST_DISK=`lsblk -no PKNAME $(findmnt -no SOURCE /)`
	ROOTFS_DEV=`lsblk -rno PATH,LABEL,PKNAME | awk -v host=$HOST_DISK '$2 == "rootfs" && $3 != host {print $1}'`
fi

if [ -z "$ROOTFS_DEV" ] || [ `echo "$ROOTFS_DEV" | wc -l` != 1 ]; then
	echo "SD rootfs partition not found (or more than one): '$ROOTFS_DEV'"
	echo "Pass it explicitly, e.g. sudo $0 /dev/sdc2"
	exit 1
fi

echo mount $ROOTFS_DEV $ROOTFS_DIR
mkdir -p $ROOTFS_DIR
mount $ROOTFS_DEV $ROOTFS_DIR || { rmdir $ROOTFS_DIR; exit 1; }

echo mkdir -p $ROOTFS_DIR/root/update_image/
mkdir -p $ROOTFS_DIR/root/update_image/

echo cp ./custom_datafs.tar.gz $ROOTFS_DIR/root/update_image/
cp ./custom_datafs.tar.gz $ROOTFS_DIR/root/update_image/

echo cp ./rootfs.tar.gz $ROOTFS_DIR/root/update_image/
cp ./rootfs.tar.gz $ROOTFS_DIR/root/update_image/

echo "Copy Fusing eMMC Upgrade script"
#echo "export TSLIB_TSDEVICE=/dev/input/event0" >> $ROOTFS_DIR/root/.bashrc
grep -qx "~/S99_Fusing_eMMC_FS.sh" $ROOTFS_DIR/root/.bashrc || \
	echo "~/S99_Fusing_eMMC_FS.sh" >> $ROOTFS_DIR/root/.bashrc

echo sync
sync
sync

umount $ROOTFS_DIR
rmdir $ROOTFS_DIR
