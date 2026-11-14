#!/bin/sh

PRC_FOLDER_NAME=/root/total_mp_process
ROOT_DIR=`pwd`

mkdir -p $ROOT_DIR/boot-images
mount /dev/mmcblk1p1 $ROOT_DIR/boot-images

echo "[step 1] Fusing tiboot3.bin / tispl.bin / u-toob.img"
echo 0 > /sys/block/mmcblk0boot0/force_ro
dd if=$ROOT_DIR/boot-images/tiboot3.bin of=/dev/mmcblk0boot0 seek=0
dd if=$ROOT_DIR/boot-images/tispl.bin of=/dev/mmcblk0boot0 seek=1024
dd if=$ROOT_DIR/boot-images/u-boot.img of=/dev/mmcblk0boot0 seek=4096

mmc bootpart enable 1 1 /dev/mmcblk0boot0
sleep 1

mmc bootbus set single_backward x1 x8  /dev/mmcblk0boot0
sleep 1

mmc hwreset enable /dev/mmcblk0
sleep 1

echo "[Step 2] Make File System"
$PRC_FOLDER_NAME/make_emmc_rfs.sh

echo "[Step 3] Umount NAND"
$PRC_FOLDER_NAME/unmount_eMMC.sh

echo "[step 4] Set date"
$PRC_FOLDER_NAME/check_timeserver.sh

umount $ROOT_DIR/boot-images

echo "Test Result OK"
