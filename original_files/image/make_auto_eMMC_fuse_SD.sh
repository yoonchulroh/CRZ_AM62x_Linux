#!/bin/sh

BOOT_DIR=/media/hgchoe/boot
ROOTFS_DIR=/media/hgchoe/rootfs

echo mkdir -p $ROOTFS_DIR/root/update_image/
mkdir -p $ROOTFS_DIR/root/update_image/

echo cp ./custom_datafs.tar.gz $ROOTFS_DIR/root/update_image/
cp ./custom_datafs.tar.gz $ROOTFS_DIR/root/update_image/

echo cp ./rootfs.tar.gz $ROOTFS_DIR/root/update_image/
cp ./rootfs.tar.gz $ROOTFS_DIR/root/update_image/

echo "Copy Fusing eMMC Upgrade script"
#echo "export TSLIB_TSDEVICE=/dev/input/event0" >> $ROOTFS_DIR/root/.bashrc
echo "~/S99_Fusing_eMMC_FS.sh" >> $ROOTFS_DIR/root/.bashrc

echo sync
sync
sync

umount /media/hgchoe/*
