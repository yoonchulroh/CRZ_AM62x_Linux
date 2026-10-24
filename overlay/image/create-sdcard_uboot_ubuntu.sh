#!/bin/bash
#
# U-Boot-only variant of create-sdcard_ubuntu.sh
#
# Writes ONLY the FAT boot partition (tiboot3.bin / tispl.bin / u-boot.img).
# The card is always wiped and re-partitioned from sd.dump, so the result
# does not depend on what was flashed before.  The rootfs partition is
# formatted but left empty: no rootfs.tar.gz, no custom_datafs, no kernel/dtb.
#
# Keeping an old rootfs is NOT harmless: the default bootcmd loads
# /boot/Image and /boot/dtb/ from mmc 1:2, so a leftover rootfs gets booted
# by the new u-boot and hides whether the u-boot build itself is good.
#
# On AM62x SD boot the ROM reads tiboot3.bin from partition 1, R5 SPL then
# loads tispl.bin and u-boot.img from the same partition.  Nothing else is
# touched until U-Boot is already at its prompt, so this is enough to verify
# a u-boot build.  Autoboot will fail to find a kernel - that is expected;
# press a key to stop it and check 'version', 'mmc info', 'fatls mmc 1:1'.
#
# Usage:  sudo ./create-sdcard_uboot_ubuntu.sh
#         sudo BOOTFILEPATH=/some/other/dir ./create-sdcard_uboot_ubuntu.sh
#
# Force locale language to be set to English. This avoids issues when doing
# text and string processing.
export LANG=C

# Source of the boot images - where ../build.sh uboot installs them
BOOTFILEPATH=${BOOTFILEPATH:-custom_datafs/boot-images}
SD_DUMP_FILE=sd.dump
PATH_TO_SDBOOT=boot

clear

AMIROOT=`whoami | awk {'print $1'}`
if [ "$AMIROOT" != "root" ] ; then

	echo "	**** Error *** must run script with sudo"
	echo ""
	exit 1
fi

if [ ! -d "$BOOTFILEPATH" ] ; then
	echo "	**** Error *** boot image directory '$BOOTFILEPATH' not found"
	echo "	Run '../build.sh uboot' first, or set BOOTFILEPATH to the"
	echo "	directory holding tiboot3.bin, tispl.bin and u-boot.img"
	echo ""
	exit 1
fi

if [ ! -f "$SD_DUMP_FILE" ] ; then
	echo "	**** Error *** '$SD_DUMP_FILE' not found - run this script from the image/ directory"
	echo ""
	exit 1
fi

# Never leave a mounted partition behind, even on Ctrl-C
cleanup()
{
	if mountpoint -q "$PATH_TO_SDBOOT" 2>/dev/null ; then
		sync
		umount -f "$PATH_TO_SDBOOT"
	fi
	rmdir "$PATH_TO_SDBOOT" 2>/dev/null
}
trap cleanup EXIT INT TERM

check_for_sdcards()
{
        # find the avaible SD cards
        ROOTDRIVE=`mount | grep 'on / ' | awk {'print $1'} |  cut -c6-8`
        PARTITION_TEST=`cat /proc/partitions | grep -v $ROOTDRIVE | grep '\<sd.\>\|\<mmcblk.\>' | grep -n ''`
        if [ "$PARTITION_TEST" = "" ]; then
	        echo -e "Please insert a SD card to continue\n"
	        while [ "$PARTITION_TEST" = "" ]; do
		        read -p "Type 'y' to re-detect the SD card or 'n' to exit the script: " REPLY
		        if [ "$REPLY" = 'n' ]; then
		            exit 1
		        fi
		        ROOTDRIVE=`mount | grep 'on / ' | awk {'print $1'} |  cut -c6-8`
		        PARTITION_TEST=`cat /proc/partitions | grep -v $ROOTDRIVE | grep '\<sd.\>\|\<mmcblk.\>' | grep -n ''`
	        done
        fi
}

# find the avaible SD cards
ROOTDRIVE=`mount | grep 'on / ' | awk {'print $1'} |  cut -c6-9`
if [ "$ROOTDRIVE" = "root" ]; then
    ROOTDRIVE=`readlink /dev/root | cut -c1-3`
else
    ROOTDRIVE=`echo $ROOTDRIVE | cut -c1-3`
fi

# Check for available mounts
check_for_sdcards

echo -e "\nAvailable Drives to write images to: \n"
echo "#  major   minor    size   name "
cat /proc/partitions | grep -v $ROOTDRIVE | grep '\<sd.\>\|\<mmcblk.\>' | grep -n ''
echo " "

DEVICEDRIVENUMBER=
while true;
do
	read -p 'Enter Device Number or 'n' to exit: ' DEVICEDRIVENUMBER
	echo " "
        if [ "$DEVICEDRIVENUMBER" = 'n' ]; then
                exit 1
        fi

        if [ "$DEVICEDRIVENUMBER" = "" ]; then
                # Check to see if there are any changes
                check_for_sdcards
                echo -e "These are the Drives available to write images to:"
                echo "#  major   minor    size   name "
                cat /proc/partitions | grep -v $ROOTDRIVE | grep '\<sd.\>\|\<mmcblk.\>' | grep -n ''
                echo " "
               continue
        fi

	DEVICEDRIVENAME=`cat /proc/partitions | grep -v $ROOTDRIVE | grep '\<sd.\>\|\<mmcblk.\>' | grep -n '' | grep "${DEVICEDRIVENUMBER}:" | awk '{print $5}'`
	if [ -n "$DEVICEDRIVENAME" ]
	then
	        DRIVE=/dev/$DEVICEDRIVENAME
	        DEVICESIZE=`cat /proc/partitions | grep -v $ROOTDRIVE | grep '\<sd.\>\|\<mmcblk.\>' | grep -n '' | grep "${DEVICEDRIVENUMBER}:" | awk '{print $4}'`
		break
	else
		echo -e "Invalid selection!"
                # Check to see if there are any changes
                check_for_sdcards
                echo -e "These are the only Drives available to write images to: \n"
                echo "#  major   minor    size   name "
                cat /proc/partitions | grep -v $ROOTDRIVE | grep '\<sd.\>\|\<mmcblk.\>' | grep -n ''
                echo " "
	fi
done

DRIVE=/dev/$DEVICEDRIVENAME

# This if statement will determine if we have a mounted sdX or mmcblkX device.
# If it is mmcblkX, then we need to set an extra char in the partition names, 'p',
# to account for /dev/mmcblkXpY labled partitions.
if [[ ${DEVICEDRIVENAME} =~ ^sd. ]]; then
	echo "$DRIVE is an sdx device"
	P=''
else
	echo "$DRIVE is an mmcblkx device"
	P='p'
fi

# Unmount every partition of the card, whatever its number and however often
# it is mounted (desktop auto-mount, a previous run, ...)
unmount_all()
{
	for part in `lsblk -lnpo NAME $DRIVE` ; do
		while findmnt -rn -S $part > /dev/null ; do
			echo " unmounting $part"
			if ! umount $part ; then
				echo "	**** Error *** could not unmount $part"
				exit 1
			fi
		done
	done
}

cat << EOM

################################################################################

	$DRIVE will be wiped completely and re-partitioned from $SD_DUMP_FILE:

	    partition 1  FAT32 "boot"    tiboot3.bin, tispl.bin, u-boot.img
	    partition 2  ext3  "rootfs"  empty

	Nothing from a previous flash is kept (old partition table, boot
	files, rootfs, kernel), so the card always comes out the same.

	****WARNING**** continuing will erase all data on $DEVICEDRIVENAME

################################################################################

EOM
ENTERCORRECTLY=0
while [ $ENTERCORRECTLY -ne 1 ]
do
	read -p 'Continue? [y/n] : ' EXITQ
	echo " "
	ENTERCORRECTLY=1
	case $EXITQ in
	"y") ;;
	"n") exit 1;;
	*)  echo "Please enter y or n";ENTERCORRECTLY=0;;
	esac
done

unmount_all

cat << EOM

################################################################################

		Wiping $DRIVE

################################################################################

EOM

# Partitions first - once the partition table is gone their filesystem
# signatures can no longer be found.  Then the whole disk, which removes the
# MBR/GPT including GPT's backup header at the end of the card.
for part in `lsblk -lnpo NAME $DRIVE | tail -n +2` ; do
	if ! wipefs -a $part ; then
		echo "	**** Error *** wiping $part failed"
		exit 1
	fi
done
if ! wipefs -a $DRIVE ; then
	echo "	**** Error *** wiping $DRIVE failed"
	exit 1
fi

# Zero the first 16 MiB (MBR, the gap in front of partition 1, the start of
# partition 1) and the last 1 MiB, so nothing an earlier layout wrote outside
# a filesystem - raw bootloader images, stale partition tables - survives.
SECTORS=`blockdev --getsz $DRIVE`
dd if=/dev/zero of=$DRIVE bs=1M count=16 conv=fsync status=none && \
dd if=/dev/zero of=$DRIVE bs=512 seek=$((SECTORS - 2048)) count=2048 conv=fsync status=none
if [ $? -ne 0 ] ; then
	echo "	**** Error *** zeroing $DRIVE failed"
	exit 1
fi

cat << EOM

################################################################################

		Now making 2 partitions

################################################################################

EOM

SIZE=`fdisk -l $DRIVE | grep Disk | awk '{print $5}'`
echo DISK SIZE - $SIZE bytes

# Feed sfdisk the layout from sd.dump with the device names stripped, so the
# same layout applies to /dev/sdX and /dev/mmcblkX alike.
sed -e '/^device:/d' -e 's|^/dev/[a-z0-9]* *: *||' $SD_DUMP_FILE | \
	sfdisk --wipe always --wipe-partitions always $DRIVE
if [ $? -ne 0 ] ; then
	echo "	**** Error *** partitioning $DRIVE failed"
	exit 1
fi

partprobe $DRIVE 2>/dev/null
udevadm settle
for n in 1 2 ; do
	if [ ! -b ${DRIVE}${P}$n ] ; then
		echo "	**** Error *** ${DRIVE}${P}$n did not appear after partitioning"
		exit 1
	fi
done
unmount_all

cat << EOM

################################################################################

		Formatting Boot

################################################################################
EOM
if ! mkfs.vfat -F 32 -n "boot" ${DRIVE}${P}1 ; then
	echo "	**** Error *** formatting ${DRIVE}${P}1 failed"
	exit 1
fi
cat << EOM

################################################################################

		Formatting rootfs (left empty - u-boot only test)

################################################################################
EOM
# -F: never stop at a "Proceed anyway? (y,N)" prompt - answering it with
# Enter would leave the old rootfs in place
if ! mkfs.ext3 -F -L "rootfs" ${DRIVE}${P}2 ; then
	echo "	**** Error *** formatting ${DRIVE}${P}2 failed"
	exit 1
fi
sync
unmount_all

echo " "
echo "Mount the boot partition "
mkdir -p $PATH_TO_SDBOOT
mount -t vfat ${DRIVE}${P}1 $PATH_TO_SDBOOT
if [ $? -ne 0 ] ; then
	echo "	**** Error *** could not mount ${DRIVE}${P}1"
	exit 1
fi

TIBOOT3=`ls $BOOTFILEPATH | grep tiboot3 | awk {'print $1'}`
TISPL=`ls $BOOTFILEPATH | grep tispl | awk {'print $1'}`
BOOTIMG=`ls $BOOTFILEPATH | grep u-boot | grep .img | awk {'print $1'}`
BOOTBIN=`ls $BOOTFILEPATH | grep u-boot | grep .bin | awk {'print $1'}`
BOOTUENV=`ls $BOOTFILEPATH | grep uEnv.txt | awk {'print $1'}`

cat << EOM
################################################################################

	Copying boot partition from $BOOTFILEPATH

################################################################################

EOM

MISSING=0

if [ "$TIBOOT3" != "" ] ; then
	cp $BOOTFILEPATH/$TIBOOT3 $PATH_TO_SDBOOT/tiboot3.bin
	echo "tiboot3.bin copied"
else
	echo "tiboot3.bin file not found"
	MISSING=1
fi

if [ "$TISPL" != "" ] ; then
	cp $BOOTFILEPATH/$TISPL $PATH_TO_SDBOOT/tispl.bin
	echo "tispl.bin copied"
else
	echo "tispl.bin file not found"
	MISSING=1
fi

if [ "$BOOTIMG" != "" ] ; then
	cp $BOOTFILEPATH/$BOOTIMG $PATH_TO_SDBOOT/u-boot.img
	echo "u-boot.img copied"
elif [ "$BOOTBIN" != "" ] ; then
	cp $BOOTFILEPATH/$BOOTBIN $PATH_TO_SDBOOT/u-boot.bin
	echo "u-boot.bin copied"
else
	echo "No U-Boot file found"
	MISSING=1
fi

if [ "$BOOTUENV" != "" ] ; then
	cp $BOOTFILEPATH/$BOOTUENV $PATH_TO_SDBOOT/uEnv.txt
	echo "uEnv.txt copied"
fi

echo ""
echo "Syncing...."
sync
sync
sync

echo ""
echo "Boot partition now contains:"
ls -l $PATH_TO_SDBOOT
echo ""
md5sum $PATH_TO_SDBOOT/* 2>/dev/null

echo " "
echo "Un-mount the boot partition "
umount -f $PATH_TO_SDBOOT
rmdir $PATH_TO_SDBOOT 2>/dev/null
unmount_all
sync

if [ "$MISSING" -ne "0" ] ; then
	echo " "
	echo "	**** Warning *** one or more boot images were missing, the card will not boot"
	echo " "
	exit 1
fi

cat << EOM

################################################################################

   Operation Finished - u-boot images only

   The rootfs partition is empty, so there is no kernel on this card.
   U-Boot will reach its prompt and then fail to find a kernel on mmc1;
   press a key to stop autoboot and check:

       version          - confirms the build you just flashed
       mmc info
       fatls mmc 1:1    - lists the three files written here

   If an OS still boots, autoboot found it on another device - the
   'bootflow scan' fallback also tries mmc0 (eMMC), usb, pxe and dhcp.
   The boot log names the device it booted from.

################################################################################

EOM
