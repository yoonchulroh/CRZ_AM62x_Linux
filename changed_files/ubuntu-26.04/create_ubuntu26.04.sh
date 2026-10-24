#!/bin/bash

TARGET_DIR=./custom_datafs/rootfs
BASE_FILE=./custom_datafs/data/ubuntu-base-26.04-base-arm64.tar.gz

if [ -f "$BASE_FILE" ]; then
    echo "$BASE_FILE exists."
else 
    echo "$BASE_FILE does not exist."
	wget https://cdimage.ubuntu.com/ubuntu-base/releases/26.04/release/ubuntu-base-26.04-base-arm64.tar.gz -P ./custom_datafs/data
fi
mkdir -p $TARGET_DIR
sudo tar xvzf $BASE_FILE -C $TARGET_DIR
sudo cp ./custom_datafs/data/create_ubuntu26.04_arm.sh $TARGET_DIR
sudo apt-get update -y
sudo apt-get install -y qemu-user qemu-user-binfmt
sudo systemctl restart systemd-binfmt
sudo cp -av /usr/bin/qemu-aarch64 $TARGET_DIR/usr/bin

echo "Run ./create_ubuntu26.04_arm.sh"

sudo chroot $TARGET_DIR
sudo umount $TARGET_DIR/proc 2>/dev/null || true

cd $TARGET_DIR

# The "./" entry of the archive becomes / on the SD card: it must be root's,
# not the build user's (mkdir above runs unprivileged).
sudo chown root:root .
sudo chmod 755 .

sudo rm -fr var/cache/apt
sudo rm -f create_ubuntu26.04_arm.sh
sudo rm -f usr/bin/qemu-aarch64

sudo tar cvzf ../rootfs.tar.gz .
cd ..

mkdir -p ../../image
sudo mv rootfs.tar.gz ../../image
sudo rm -fr rootfs
