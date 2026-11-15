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

## 8. Optional: RT kernel build

To build a PREEMPT_RT kernel, run `rt_change.sh` instead of (or after) step 2.

```
./rt_change.sh /path/to/ti-processor-sdk-linux-am62xx-evm-12.01.00.05.03
```

This adds TI's RT config fragment to `Rules.make` and renames the `kernel` build target to `kernel-rt` (`./build.sh kernel` then fails with a message).

Build the kernel with `./build.sh kernel-rt`; `./build.sh all` builds the RT kernel too. On the board, `uname -r` shows `6.18.38-rt`.

## 9. Optional: GPU display mode

The image boots in Weston mode: Weston draws the display on the CPU. In GPU mode, labwc draws it on the PowerVR GPU instead. Both are in the root filesystem built in step 3; switch on the board:

```
sudo display-mode gpu
```

The switch takes effect immediately and is kept across reboots. `sudo display-mode weston` switches back, and `display-mode` without an argument prints the current mode.

What renders where:

| | Weston mode | GPU mode |
|---|---|---|
| Compositor (draws the display from all windows) | CPU | GPU |
| cairo app | CPU | CPU |
| GLES 2 app | CPU | GPU |
| Vulkan app | CPU | GPU |

In Weston mode a Vulkan app can still render on the GPU if it is started with `PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1`; a GLES app cannot.

To check that the GPU is in use, look for PowerVR in labwc's renderer line:

```
journalctl -b -t labwc | grep "GL renderer"
```

In GPU mode:

- labwc starts with an empty desktop. Put the commands that start your apps in `~/.config/labwc/autostart`.
- Apps render on the GPU too: GLES through Mesa's Zink driver (GLES 2.0 only) and Vulkan directly. An app that needs more than GLES 2.0 has to be started with `MESA_LOADER_DRIVER_OVERRIDE` unset; it then renders on the CPU, as in Weston mode.
- This holds however an app is started. `display-mode gpu` sets the two variables that select the GPU (`MESA_LOADER_DRIVER_OVERRIDE=zink` and `PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1`) for apps started by labwc, for login shells from the next login (`/etc/profile.d/gpu-mode.sh`) and for systemd services from their next start (`/etc/systemd/system.conf.d/gpu-mode.conf`). `display-mode weston` removes them.
- Weston's own protocols (its on-screen keyboard and text input, `weston-screenshooter`) and `weston.ini` are not available.
- Mesa marks its PowerVR Vulkan driver, which this mode relies on, as not conformant.
