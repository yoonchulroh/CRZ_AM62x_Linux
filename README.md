# How to Test

This repo holds only the changes made on top of an unmodified TI Processor SDK.

## Layout

| Path | What it is |
|---|---|
| `apply_change.sh` | Applies changes to a stock SDK. |
| `changed_files/` | Full copies of every modified file, at their SDK-relative paths. |

## 1. Get the baseline SDK

ti-processor-sdk-linux-am62xx-evm-12.01.00.05.03

## 2. Apply

```
# apply changes to stock TI SDK
./apply_change.sh /path/to/ti-processor-sdk-linux-am62xx-evm-12.01.00.05.03

# build uboot image
cd /path/to/ti-processor-sdk-linux-am62xx-evm-12.01.00.05.03
./build.sh uboot

# flash to SD card
cd image
sudo ./create_sdcard_uboot_ubuntu.sh

# observe UART output (Press some key before autoboot, or interrupt autoboot)
sudo picocom -b 115200 /dev/[UART FILE]
```
