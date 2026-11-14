# How to Test

This repo holds only the changes made on top of an unmodified TI Processor SDK.

## Layout

| Path | What it is |
|---|---|
| `apply_change.sh` | Converts a stock SDK into the am62x_20260918 tree: patches, deletions, directory renames, overlay. |
| `patches/` | Diffs for stock files that were modified (against stock directory names). |
| `deleted_files.txt` | Stock files/directories that am62x_20260918 does not have; deleted by the script. |
| `overlay/` | Files not in the stock SDK, copied in after the renames. |
| `changed_files/` | Full copies of every modified or added file, at their am62x_20260918-relative paths. |
| `original_files/` | Original versions of `changed_files/` from the stock SDK, or am62x_20230814 where the stock SDK lacks them. |
| `diffs/` | Per-file diffs between `original_files/` and `changed_files/`. |

## 1. Get the baseline SDK

ti-processor-sdk-linux-am62xx-evm-12.01.00.05.03

## 2. Apply

```
./apply_change.sh /path/to/ti-processor-sdk-linux-am62xx-evm-12.01.00.05.03
```

The script sets `TI_SDK_PATH` in the SDK's `Rules.make` to the directory given as its argument, replacing the install path the TI installer wrote there. Apply the changes in the directory you will build from; if the SDK is moved afterwards, update `TI_SDK_PATH` in `Rules.make` to the new location.

This modifies the SDK in place, deletes the parts the CRZ tree does not use (about 9 GB, including `filesystem/` and `board-support/linux-extras-*`), and renames `ti-u-boot-*`, `ti-linux-kernel-*` and `trusted-firmware-a-*` under `board-support/`.

Keep a copy of the SDK if you need the stock version.

## 3. Build everything

```
cd /path/to/ti-processor-sdk-linux-am62xx-evm-12.01.00.05.03
./build.sh all
```

`all` builds, in order:

1. U-Boot (`tiboot3.bin`, `tispl.bin`, `u-boot.img`)
2. the Linux kernel, device tree and the BSD101WX1-300 display overlay, plus modules
3. the Ubuntu 26.04 root filesystem (`image/rootfs.tar.gz`)

The build needs internet access: the rootfs step downloads Ubuntu Base and installs `qemu-user` on the host.

The rootfs step asks for your sudo password, then drops you into a shell inside the target root filesystem. Run this there, then `exit` to let the build finish:

```
./create_ubuntu26.04_arm.sh
exit
```

## 4. Flash to SD card

Insert the SD card, then run the script from the `image` directory:

```
cd image
sudo ./create-sdcard_ubuntu.sh
```

Pick the SD card's device number from the list. **Everything on the selected drive is erased**, so double-check that it is the SD card and not a host disk.

## 5. Boot

Put the SD card in the board and watch the UART console:

```
sudo picocom -b 115200 /dev/[UART FILE]
```

Log in as `mango` / `mango` (or `root` / `root`).

## 6. Optional: falcon mode

In falcon mode the R5 SPL boots Linux directly, skipping the A53 SPL and U-Boot proper.

```
./build.sh falcon
```

`./build.sh all` (or `./build.sh kernel`) has to be run at least once before building falcon.

Flash the SD card as in step 4.

`./build.sh uboot` then flashing the SD card switches back to normal boot.

## 7. Optional: install to eMMC

Write an SD card that flashes the eMMC on first root login:

```
cd image
sudo ./make_auto_eMMC_fuse_SD.sh
```

Boot it from SD and log in as `root` on the UART console. The login runs `S99_Fusing_eMMC_FS.sh` to install to eMMC.
Then power off, remove the SD card, set SW1 to OFF-ON-ON-OFF and power on to boot from eMMC.
