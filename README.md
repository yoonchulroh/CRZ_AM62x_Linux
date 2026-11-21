# Manual

This repo holds only the changes made on top of an unmodified TI Processor SDK.

## Layout

| Path | What it is |
|---|---|
| `setup_host.sh` | One-time install of the host packages needed to apply, build and flash. |
| `apply_change.sh` | Converts a stock SDK into the am62x_20260918 tree: patches, deletions, directory renames, overlay. |
| `patches/` | Diffs for stock files that were modified (against stock directory names). |
| `deleted_files.txt` | Stock files/directories that am62x_20260918 does not have; deleted by the script. |
| `overlay/` | Files not in the stock SDK, copied in after the renames. |
| `changed_files/` | Full copies of every modified or added file, at their am62x_20260918-relative paths. |
| `original_files/` | Original versions of `changed_files/` from the stock SDK, or am62x_20230814 where the stock SDK lacks them. |
| `diffs/` | Per-file diffs between `original_files/` and `changed_files/`. |

## 0. Set up the host

The host PC is assumed to be Ubuntu 22.04 (x86_64).

`setup_host.sh` is a one-time setup script that installs the dependencies.
It needs sudo and internet access:
```
./setup_host.sh
```

It only has to be run once per host PC.

## 1. Get the baseline SDK

Get the stock TI SDK from TI website (https://www.ti.com/tool/ko-kr/PROCESSOR-SDK-AM62X).

Download `ti-processor-sdk-linux-am62xx-evm-12.01.00.05.03-Linux-x86-Install.bin`. This works for both Linux and Linux-RT.

Download the file and execute the installer.

## 2. Apply

Convert the stock TI SDK for the target CRZ board, by running the following script:
```
./apply_change.sh /path/to/ti-processor-sdk-linux-am62xx-evm-12.01.00.05.03
```

This modifies the SDK in place, deleting the parts the CRZ tree does not use and renaming some directories.
Keep a copy of the SDK if you need the stock version.

The script sets `TI_SDK_PATH` in `Rules.make` to the path given. If the SDK is moved later, it needs to be updated manually.

## 3. Build Image

```
cd /path/to/ti-processor-sdk-linux-am62xx-evm-12.01.00.05.03
./build.sh all
```

`all` builds, in order:
1. U-Boot
2. Linux kernel
3. Ubuntu 26.04 root filesystem

If only a part of the entire build is needed, run `build.sh uboot`, `build.sh kernel`, or `build.sh rootfs`.

The build needs internet access for the rootfs step.

The rootfs step asks for your sudo password, then drops you into a shell inside the target root filesystem.
When it asks to run a script, run `./create_ubuntu26.04_arm.sh`.
It will then ask to select region.
When it asks to exit, run `exit` to let the build finish.

## 4. Flash to SD card

Insert the SD card to the host PC, then run the script from the `image` directory:

```
cd image
sudo ./create-sdcard_ubuntu.sh
```

Pick the SD card's device number from the list. Everything on the selected drive is erased, so double check that the device can be safely erased. 

## 5. Boot

Set SW1 on the board to ON-ON-ON-OFF. Put the SD card in the board and toggle the power switch.

You can watch the UART console with:
```
sudo picocom -b 115200 /dev/[UART FILE]
```

Log in as `root` / `root` or `mango` / `mango`.

## 6. Optional: falcon mode

In falcon mode the R5 SPL boots Linux directly, skipping the A53 SPL and U-Boot proper.
This can reduce boot time, but removes U-Boot console.

Make the image use falcon mode by running the following command:
```
./build.sh falcon
```
Flash the SD card as in step 4.

`./build.sh all` (or `./build.sh kernel`) has to be run at least once before building falcon.

`./build.sh uboot` then flashing the SD card switches back to normal boot.

## 7. Optional: install to eMMC

Write an SD card that flashes the eMMC:

```
cd image
sudo ./make_auto_eMMC_fuse_SD.sh
```
Same as step 4, pick the SD card's device number from the list.

Set SW1 to ON-ON-ON-OFF to boot from SD card.
Log in as `root` on the UART console. This automatically runs `S99_Fusing_eMMC_FS.sh` to install to eMMC.
Then power off, remove the SD card, set SW1 to OFF-ON-ON-OFF and power on to boot from eMMC.

## 8. Optional: RT kernel build

To build a PREEMPT_RT kernel, run `rt_change.sh` instead of (or after) step 2.

```
./rt_change.sh /path/to/ti-processor-sdk-linux-am62xx-evm-12.01.00.05.03
```

Build the kernel with `./build.sh kernel-rt`. `./build.sh kernel` will fail. `./build.sh all` builds the RT kernel too.

On the board, `uname -r` will show `6.18.38-rt`.

## 9. Optional: GPU display mode

The image boots in Weston mode: Weston draws the display on the CPU. In GPU mode, labwc draws it on the PowerVR GPU instead. Both are in the root filesystem built in step 3.

Switch to GPU display mode on the board:

```
sudo display-mode gpu
```

The switch takes effect immediately and is kept across reboots. `sudo display-mode weston` switches back, and `display-mode` without an argument prints the current mode.

What renders where:

| | Weston mode | GPU mode |
|---|---|---|
| Compositor | CPU | GPU |
| cairo app | CPU | CPU |
| GLES 2 app | CPU | GPU |
| Vulkan app | CPU | GPU |

To check that the GPU is in use, look for PowerVR in labwc's renderer line:
```
journalctl -b -t labwc | grep "GL renderer"
```

### Other users

Users other than `mango` can reach the display socket, in either mode, once they are in the `render` group (after logging in again):

```
sudo usermod -aG render <user>
```

## 10. Optional: audio event recognition

Audio event recognition listens to a USB microphone, recognizes sounds such as sirens, alarms and breaking glass, shows them on the LCD and sends MQTT notifications to subscribers. Build it into the image with:

```
./build.sh aer
```

Then flash the SD card as in step 4. It is installed but not started; see `aer-README.md` for starting it, configuration, notifications and `aer optimize`.

## 11. Optional: prioritize systemd services

Two scripts lower the average and worst-case latency of chosen systemd services, including custom services:

- `rt-sched` runs a service with real-time scheduling, so it preempts normal tasks.
- `cpu-partition` reserves CPUs for chosen services and moves every other process and the interrupts to the remaining CPUs.

The scripts are installed with audio event recognition, so build and flash the image with it as in step 10. On the board, audio event recognition must be disabled while you use the scripts for your own services:

```
sudo aer default
sudo aer disable
```

Then apply the scripts to your services:

```
sudo rt-sched apply my-app.service
sudo cpu-partition apply my-app.service
```

By default, `rt-sched` uses SCHED_FIFO priority 50, and `cpu-partition` gives the services CPUs 1-3 and puts everything else on CPU 0. Choose other values with the options:

```
sudo rt-sched --policy rr --priority 30 apply my-app.service
sudo cpu-partition --isolated 2-3 --housekeeping 0-1 apply my-app.service
```

Both restart the services they change, take effect immediately and are kept across reboots.

| Command | Effect |
|---|---|
| `rt-sched status`, `cpu-partition status` | show what is applied |
| `sudo rt-sched remove <service>`, `sudo cpu-partition remove <service>` | undo for one service |
| `sudo rt-sched reset`, `sudo cpu-partition reset` | undo for all services |
