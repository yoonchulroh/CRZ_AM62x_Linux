#!/bin/sh

IMAGE_DIR=./image/custom_datafs/boot-images
IMAGE_DIR2=/nfs/am62x
MODULES_DIR=./image/custom_datafs/usr/lib/

START_TIME=`date +%s`

mkdir -p $IMAGE_DIR2

case "$1" in
clean)
	make linux_clean u-boot_clean
	;;
uboot)
	mkdir -p $IMAGE_DIR
	mkdir image_temp
	make u-boot
    if [ $? != 0 ] ; then
	    exit 1
    fi
	make u-boot_install
    if [ $? != 0 ] ; then
	    exit 1
    fi
	cp ./image_temp/tiboot3.bin $IMAGE_DIR
	cp ./image_temp/tispl.bin $IMAGE_DIR
	cp ./image_temp/u-boot.img $IMAGE_DIR
	cp ./image_temp/tiboot3.bin $IMAGE_DIR2
	cp ./image_temp/tispl.bin $IMAGE_DIR2
	cp ./image_temp/u-boot.img $IMAGE_DIR2
	cd ./image
	./custom_datafs_make.sh
	cd ..
	rm -fr ./image_temp
	;;
kernel)
	mkdir -p $MODULES_DIR
	mkdir image_temp
	make linux
    if [ $? != 0 ] ; then
	    exit 1
    fi
	make linux_install
    if [ $? != 0 ] ; then
	    exit 1
    fi
	rm -fr $MODULES_DIR/*
	cp -a ./image_temp/lib/* $MODULES_DIR
	mkdir -p ./image/custom_datafs/boot/dtb/ti
	cp ./image_temp/boot/Image ./image/custom_datafs/boot
	cp ./image_temp/boot/dtb/ti/k3-am625-sk.dtb ./image/custom_datafs/boot/dtb/ti
	cp ./image_temp/boot/Image $IMAGE_DIR2
	cp ./image_temp/boot/dtb/ti/k3-am625-sk.dtb $IMAGE_DIR2
	cd image
	./custom_datafs_make.sh
	cd ..
	rm -fr image_temp
	;;
rootfs)
	cd ubuntu-20.04
	./create_ubuntu20.04.sh
	cd ..
	;;
all|*)
	mkdir -p $IMAGE_DIR
	mkdir image_temp
	make u-boot
    if [ $? != 0 ] ; then
	    exit 1
    fi
	make u-boot_install
    if [ $? != 0 ] ; then
	    exit 1
    fi
	cp ./image_temp/tiboot3.bin $IMAGE_DIR
	cp ./image_temp/tispl.bin $IMAGE_DIR
	cp ./image_temp/u-boot.img $IMAGE_DIR
	cp ./image_temp/tiboot3.bin $IMAGE_DIR2
	cp ./image_temp/tispl.bin $IMAGE_DIR2
	cp ./image_temp/u-boot.img $IMAGE_DIR2

	mkdir -p $MODULES_DIR
	make linux
    if [ $? != 0 ] ; then
	    exit 1
    fi
	make linux_install
    if [ $? != 0 ] ; then
	    exit 1
    fi
	rm -fr $MODULES_DIR/*
	cp -a ./image_temp/lib/* $MODULES_DIR
	mkdir -p ./image/custom_datafs/boot/dtb/ti
	cp ./image_temp/boot/Image ./image/custom_datafs/boot
	cp ./image_temp/boot/dtb/ti/k3-am625-sk.dtb ./image/custom_datafs/boot/dtb/ti
	cp ./image_temp/boot/Image $IMAGE_DIR2
	cp ./image_temp/boot/dtb/ti/k3-am625-sk.dtb $IMAGE_DIR2
	cd image
	./custom_datafs_make.sh
	cd ..
	rm -fr image_temp
	cd ubuntu-20.04
	./create_ubuntu20.04.sh
	cd ..
	;;
esac

END_TIME=`date +%s`
echo "Total compile time is $((($END_TIME-$START_TIME)/60)) minutes $((($END_TIME-$START_TIME)%60)) seconds"

