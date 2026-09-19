#!/bin/sh

ROOTFS_NAME=custom_datafs.tar
ROOTFS_DIR=custom_datafs
RTL_DIR=usr/lib/firmware/rtlwifi

mkdir -p $ROOTFS_DIR/$RTL_DIR
cp -a rtl8188eufw.bin $ROOTFS_DIR/$RTL_DIR

rm -rf $ROOTFS_NAME.gz

cd $ROOTFS_DIR
tar cvf $ROOTFS_NAME *
mv $ROOTFS_NAME ../
cd ..

gzip $ROOTFS_NAME
