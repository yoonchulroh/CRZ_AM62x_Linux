#!/bin/sh

UPDATE_FOLDER_NAME=/root/update_image/


ROOT_FS_NAME=$UPDATE_FOLDER_NAME/rootfs.tar.gz
DATA_FS_NAME=$UPDATE_FOLDER_NAME/custom_datafs.tar.gz

echo "Start eMMC filesystem ..."
mkdir -p /mnt/eMMC_rootfs

echo "[step 2-1] fdisk /dev/mmcblk0"
fdisk /dev/mmcblk0 <<EOF
p
d
n




w
EOF

echo "[step 2-2] mkfs.ext4 /dev/mmcblk0p1"
mkfs.ext4 /dev/mmcblk0p1 << EOF

EOF
mount -t ext4 /dev/mmcblk0p1 /mnt/eMMC_rootfs
echo "[Step 2-3] mount done ..."

tar zxvf $ROOT_FS_NAME -C /mnt/eMMC_rootfs
echo "[Step 2-4] rootfs filesystem copy done ..."

echo "Complete eMMC filesystem ..."

tar zxvf $DATA_FS_NAME -C /mnt/eMMC_rootfs
echo "Complete eMMC data filesystem ..."

echo sync
sync
sync

sleep 1

