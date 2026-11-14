#!/bin/sh

UPDATE_FOLDER_NAME=/root/update_image/


ROOT_FS_NAME=$UPDATE_FOLDER_NAME/tisdk-tiny-image-am62xx-evm.tar.xz
DATA_FS_NAME=$UPDATE_FOLDER_NAME/custom_datafs.tar.gz

echo "Start nand boot ubifs filesystem ..."
mkdir -p /mnt/nand_rootfs

ubiformat /dev/mtd6
ubiattach /dev/ubi_ctrl -m 6
echo "[Step 1-1] mtd6 ubiattach done ..."
ubimkvol /dev/ubi0 -N rootfs -m -n 0
echo "[Step 1-2] mtd6 ubimkvol done ..."

mount -t ubifs ubi0:rootfs /mnt/nand_rootfs
echo "[Step 1-3] mount done ..."

tar Jxvf $ROOT_FS_NAME -C /mnt/nand_rootfs
echo "[Step 1-4] rootfs filesystem copy done ..."

echo "Complete nand boot ubifs filesystem ..."

tar zxvf $DATA_FS_NAME -C /mnt/nand_rootfs

echo "Complete nand data ubifs filesystem ..."

echo sync
sync
sync

sleep 1

