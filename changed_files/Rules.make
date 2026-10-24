
### COMMON CONFIG ###
CFLAGS?=

#Strip modules when installing to conserve disk space
INSTALL_MOD_STRIP?=1

# Set EXEC_DIR to install example binaries.
# This will be configured with the setup.sh script.
EXEC_DIR?=__EXEC_DIR__

# Root of the boot partition to install boot binaries
BOOTFS?=__BOOTFS__

MAKE_JOBS?=1

### TI SDK CONFIG ###
#Points to the root of the TI SDK
export TI_SDK_PATH?=/home/yoonchulroh/CRZ/am62x_20260918

#root of the target file system for installing applications
DESTDIR?=${TI_SDK_PATH}/image_temp

export LINUX_DEVKIT_PATH=$(TI_SDK_PATH)/linux-devkit
export K3_R5_LINUX_DEVKIT_PATH=$(TI_SDK_PATH)/k3r5-devkit
SDK_PATH_TARGET=$(LINUX_DEVKIT_PATH)/sysroots/aarch64-oe-linux
SDK_PATH_HOST=$(LINUX_DEVKIT_PATH)/sysroots/x86_64-arago-linux

# The source directories for each component
CRYPTODEV_SRC_DIR:=$(shell ls -d $(TI_SDK_PATH)/board-support/extra-drivers/cryptodev*)
IMG_ROGUE_SRC_DIR:=$(shell ls -d $(TI_SDK_PATH)/board-support/extra-drivers/ti-img-rogue-driver*)
UBOOT_SRC_DIR:=$(shell ls -d $(TI_SDK_PATH)/board-support/ti-u-boot*)
LINUXKERNEL_INSTALL_DIR:=$(shell ls -d $(TI_SDK_PATH)/board-support/ti-linux-kernel*)

export TI_SECURE_DEV_PKG=$(TI_SDK_PATH)/board-support/core-secdev-k3
### PLATFORM CONFIG ###
#platform
SOC=am62

#add platform for scripts
PLATFORM?=am62xx-evm
YOCTO_MACHINE?=am62xx-evm

#Architecture
ARCH=arm64

#ARM Toolchains
export CROSS_COMPILE=$(LINUX_DEVKIT_PATH)/sysroots/x86_64-arago-linux/usr/bin/aarch64-oe-linux/aarch64-oe-linux-
export CROSS_COMPILE_ARMV7=$(K3_R5_LINUX_DEVKIT_PATH)/sysroots/x86_64-arago-linux/usr/bin/arm-oe-eabi/arm-oe-eabi-

# Host tools built by u-boot (mkimage is used to build the kernel fitImage)
export PATH:=$(TI_SDK_PATH)/board-support/u-boot-build/a53/tools:$(PATH)

#Default CC value to be used when cross compiling.  This is so that the
#GNU Make default of "cc" is not used to point to the host compiler
export CC=$(CROSS_COMPILE)gcc --sysroot=$(SDK_PATH_TARGET)

# u-boot machine configs for A53 and R5
UBOOT_MACHINE=am62x_evm_a53_defconfig
UBOOT_MACHINE_R5=am62x_evm_r5_defconfig
MKIMAGE_DTB_FILE=a53/dts/upstream/src/arm64/ti/k3-am625-sk.dtb

# Update platform, yocto machine and defconfigs if PLATFORM=am62xx-lp-evm
ifeq ($(PLATFORM),am62xx-lp-evm)
    YOCTO_MACHINE=am62xx-lp-evm
    UBOOT_MACHINE=am62x_lpsk_a53_defconfig
    UBOOT_MACHINE_R5=am62x_lpsk_r5_defconfig
    MKIMAGE_DTB_FILE=a53/dts/upstream/src/arm64/ti/k3-am62-lp-sk.dtb
endif

KERNEL_DEVICETREE_PREFIX=ti/k3-am625|ti/k3-am62-|ti/k3-am62x|ti/k3-am62.dtsi

TI_LINUX_FIRMWARE=$(TI_SDK_PATH)/board-support/prebuilt-images/$(PLATFORM)
UBOOT_ATF=$(TI_SDK_PATH)/board-support/prebuilt-images/$(PLATFORM)/bl31.bin
UBOOT_TEE=$(TI_SDK_PATH)/board-support/prebuilt-images/$(PLATFORM)/bl32.bin
TI_DM=$(TI_SDK_PATH)/board-support/prebuilt-images/$(PLATFORM)/ti-dm/am62xx/ipc_echo_testb_mcu1_0_release_strip.xer5f
LINUXEXTRASKERNEL_INSTALL_DIR:=$(shell ls -d $(TI_SDK_PATH)/board-support/linux-extras-*)
UBOOTEXTRAS_SRC_DIR:=$(shell ls -d $(TI_SDK_PATH)/board-support/u-boot-extras-jailhouse-*)

# Add configs for ti-img-rogue-driver
PVR_BUILD_DIR=am62_linux
WINDOW_SYSTEM=lws-generic
PVR_BUILD=release

MAKE_ALL_TARGETS?= u-boot linux linux-dtbs

MAKE_JOBS?=$(shell nproc)
