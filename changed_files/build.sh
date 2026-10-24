#!/bin/sh

IMAGE_DIR=./image/custom_datafs/boot-images
MODULES_DIR=./image/custom_datafs/usr/lib/

START_TIME=`date +%s`

# audio-event-recognition: cross-compile the aer-* modules and install them,
# the YAMNet model, /etc/aer/aer.conf and the systemd units into the overlay
build_aer()
{
	make -C ./audio-event-recognition
	if [ $? != 0 ] ; then
		exit 1
	fi
	make -C ./audio-event-recognition install DESTDIR=`pwd`/image/custom_datafs
	if [ $? != 0 ] ; then
		exit 1
	fi
}

case "$1" in
clean)
	make linux_clean u-boot_clean
	make -C ./audio-event-recognition clean
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
	cp ./image_temp/boot/dtb/ti/k3-am625-sk-bsd101wx1-300.dtbo ./image/custom_datafs/boot/dtb/ti
	cd image
	./custom_datafs_make.sh
	cd ..
	rm -fr image_temp
	;;
rootfs)
	cd ubuntu-26.04
	./create_ubuntu26.04.sh
	cd ..
	;;
aer)
	build_aer
	cd image
	./custom_datafs_make.sh
	cd ..
	;;
sample)
	make -C ./sample/touch-demo
    if [ $? != 0 ] ; then
	    exit 1
    fi
	mkdir -p ./image/custom_datafs/usr/local/bin
	mkdir -p ./image/custom_datafs/etc/xdg/weston
	cp ./sample/touch-demo/touch-demo ./image/custom_datafs/usr/local/bin
	cp ./sample/touch-demo/weston.ini ./image/custom_datafs/etc/xdg/weston
	cd image
	./custom_datafs_make.sh
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
	cp ./image_temp/boot/dtb/ti/k3-am625-sk-bsd101wx1-300.dtbo ./image/custom_datafs/boot/dtb/ti
	build_aer
	cd image
	./custom_datafs_make.sh
	cd ..
	rm -fr image_temp
	cd ubuntu-26.04
	./create_ubuntu26.04.sh
	cd ..
	;;
esac

END_TIME=`date +%s`
echo "Total compile time is $((($END_TIME-$START_TIME)/60)) minutes $((($END_TIME-$START_TIME)%60)) seconds"

