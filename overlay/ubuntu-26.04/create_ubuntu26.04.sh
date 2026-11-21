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

# The arm64 chroot runs under a statically linked qemu-aarch64: on 22.04/24.04
# that is qemu-user-static's qemu-aarch64-static, from 25.04 qemu-user's own
# qemu-aarch64 (qemu-user-static no longer exists there).
find_static_qemu()
{
	for q in /usr/bin/qemu-aarch64-static /usr/bin/qemu-aarch64; do
		if [ -x "$q" ] && file -L "$q" | grep -q static; then
			echo "$q"
			return 0
		fi
	done
	return 1
}

if ! QEMU=$(find_static_qemu); then
	sudo apt-get update -y
	if apt-cache policy qemu-user-static | grep -q 'Candidate: [0-9]'; then
		sudo apt-get install -y qemu-user-static binfmt-support
	else
		sudo apt-get install -y qemu-user qemu-user-binfmt
	fi
	QEMU=$(find_static_qemu) || { echo "no static qemu-aarch64 found"; exit 1; }
fi
sudo systemctl restart systemd-binfmt 2>/dev/null || true
if command -v update-binfmts >/dev/null; then
	sudo update-binfmts --enable qemu-aarch64 2>/dev/null || true
fi

# Put the binary in the chroot at the path the kernel's binfmt handler names
QEMU_IN_CHROOT=$(sed -n 's/^interpreter //p' /proc/sys/fs/binfmt_misc/qemu-aarch64 2>/dev/null)
[ -n "$QEMU_IN_CHROOT" ] || { echo "qemu-aarch64 binfmt handler not registered"; exit 1; }
sudo cp -v "$QEMU" "$TARGET_DIR$QEMU_IN_CHROOT"

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
sudo rm -f ".$QEMU_IN_CHROOT"

sudo tar cvzf ../rootfs.tar.gz .
cd ..

mkdir -p ../../image
sudo mv rootfs.tar.gz ../../image
sudo rm -fr rootfs
