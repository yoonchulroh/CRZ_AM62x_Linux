#!/bin/sh

ROOTFS_NAME=custom_datafs.tar
ROOTFS_DIR=custom_datafs
RTL_DIR=usr/lib/firmware/rtlwifi

mkdir -p $ROOTFS_DIR/$RTL_DIR
cp -a rtl8188eufw.bin $ROOTFS_DIR/$RTL_DIR

rm -rf $ROOTFS_NAME.gz

cd $ROOTFS_DIR
# Everything in the overlay belongs to root on the target. Without this the
# build user's uid (1000 = mango on the board) ends up owning /etc, /usr, ...
tar --owner=0 --group=0 --numeric-owner --mode=go-w -cvf $ROOTFS_NAME *
mv $ROOTFS_NAME ../
cd ..

gzip $ROOTFS_NAME
