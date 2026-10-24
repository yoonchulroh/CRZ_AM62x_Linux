#!/bin/bash
#
# U-Boot-only variant of create-sdcard_ubuntu.sh
#
# Writes ONLY the FAT boot partition (tiboot3.bin / tispl.bin / u-boot.img).
# The rootfs partition is created and formatted when partitioning, but is
# never populated: no rootfs.tar.gz, no custom_datafs, no kernel/dtb.
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
NUM_OF_DRIVES=`df | grep -c $DEVICEDRIVENAME`

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

if [ "$NUM_OF_DRIVES" != "0" ]; then
        echo "Unmounting the $DEVICEDRIVENAME drives"
        for ((c=1; c<="$NUM_OF_DRIVES"; c++ ))
        do
                unmounted=`df | grep '\<'$DEVICEDRIVENAME$P$c'\>' | awk '{print $1}'`
                if [ -n "$unmounted" ]
                then
                     echo " unmounted ${DRIVE}$P$c"
                     umount -f ${DRIVE}$P$c
                fi

        done
fi

# check to see if the device is already partitioned the way we expect
for ((  c=1; c<5; c++ ))
do
	eval "SIZE$c=`cat /proc/partitions | grep -v $ROOTDRIVE | grep '\<'$DEVICEDRIVENAME$P$c'\>'  | awk '{print $3}'`"
done

PARTITION=0
if [ -n "$SIZE1" -a -n "$SIZE2" ] ; then
	if  [ "$SIZE1" -gt "72000" -a "$SIZE2" -gt "700000" ]
	then
		PARTITION=1
	fi
fi

if [ "$PARTITION" -eq "1" ]
then
cat << EOM

################################################################################

   Detected device is already partitioned for boot + rootfs

   U-Boot only needs partition 1, so re-partitioning is normally unnecessary.
   Keeping the partitions preserves whatever rootfs is already on the card.

################################################################################

EOM
	ENTERCORRECTLY=0
	while [ $ENTERCORRECTLY -ne 1 ]
	do
		read -p 'Would you like to re-partition the drive anyways [y/N] : ' CASEPARTITION
		echo " "
		ENTERCORRECTLY=1
		case $CASEPARTITION in
		"y")  echo "Now partitioning $DEVICEDRIVENAME ...";PARTITION=2;;
		"n"|"") echo "Skipping partitioning";PARTITION=1;;
		*)  echo "Please enter y or n";ENTERCORRECTLY=0;;
		esac
		echo ""
	done
fi

if [ "$PARTITION" -eq "0" ]
then
cat << EOM

################################################################################

	SD card is not partitioned as expected, it will be re-partitioned
	with the usual 2 partitions (boot + rootfs) from $SD_DUMP_FILE

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
		"y") PARTITION=2;;
		"n") exit 1;;
		*)  echo "Please enter y or n";ENTERCORRECTLY=0;;
		esac
	done
fi

if [ "$PARTITION" -eq "2" ]
then
cat << EOM

################################################################################

		Now making 2 partitions

################################################################################

EOM

SIZE=`fdisk -l $DRIVE | grep Disk | awk '{print $5}'`
echo DISK SIZE - $SIZE bytes

# Feed sfdisk the layout from sd.dump with the device names stripped, so the
# same layout applies to /dev/sdX and /dev/mmcblkX alike.
sed -e '/^device:/d' -e 's|^/dev/[a-z0-9]* *: *||' $SD_DUMP_FILE | sfdisk $DRIVE
if [ $? -ne 0 ] ; then
	echo "	**** Error *** partitioning $DRIVE failed"
	exit 1
fi

partprobe $DRIVE 2>/dev/null
sleep 2

cat << EOM

################################################################################

		Formatting Boot

################################################################################
EOM
	mkfs.vfat -F 32 -n "boot" ${DRIVE}${P}1
cat << EOM

################################################################################

		Formatting rootfs (left empty - u-boot only test)

################################################################################
EOM
	mkfs.ext3 -L "rootfs" ${DRIVE}${P}2
	sync
	sync
fi

echo " "
echo "Mount the boot partition "
mkdir -p $PATH_TO_SDBOOT
mount -t vfat ${DRIVE}${P}1 $PATH_TO_SDBOOT
if [ $? -ne 0 ] ; then
	echo "	**** Error *** could not mount ${DRIVE}${P}1"
	exit 1
fi

echo " "
echo "Emptying boot partition "
rm -rf $PATH_TO_SDBOOT/*

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

if [ "$MISSING" -ne "0" ] ; then
	echo " "
	echo "	**** Warning *** one or more boot images were missing, the card will not boot"
	echo " "
	exit 1
fi

cat << EOM

################################################################################

   Operation Finished - u-boot images only

   The rootfs partition was NOT written.  U-Boot will reach its prompt and
   then fail to find a kernel; press a key to stop autoboot and check:

       version          - confirms the build you just flashed
       mmc info
       fatls mmc 1:1    - lists the three files written here

################################################################################

EOM
