# Audio Event Recognition Manual

The audio event recognition feature listens to a USB microphone, recognises sounds such as sirens, alarms and breaking glass with YAMNet, shows them on the LCD and sends an MQTT notification to user devices on the network.

You need a USB microphone (UAC) connected to the board and an Ethernet connection for the board. The first build needs internet access on the host.

## Build

In the converted SDK:

```
./build.sh aer
```

This builds the feature and adds it to the image. The first build also downloads and builds TensorFlow Lite and takes a few minutes. Steps for building the image and flashing it to SD card remain the same.

To take the feature out of the image again:

```
./build.sh aer-remove
```

## Start and stop

Audio event recognition is installed but not started by default on a fresh image. On the board:

| Command | Effect |
|---|---|
| `sudo aer enable` | start now and at every boot |
| `sudo aer disable` | stop now and not at boot |
| `sudo aer start` / `sudo aer stop` | start or stop now only |
| `sudo aer restart` | restart, e.g. after changing the configuration |
| `aer status` | show the state of every service for AER |
| `aer log` | follow the logs (`aer log -b` for this boot) |

## Configure

Edit `/opt/audio-event/config.ini`, then run `sudo aer restart`.

| Setting | Meaning |
|---|---|
| `[board] name` | board name shown on the screen and sent in notifications |
| `[audio_input] mic_name` | part of the microphone's name, as shown by `arecord -l` |
| `[detector] cooldown_sec` | seconds before the same sound is reported again |
| `[watched_classes]` | sounds to report: `<class index> = <threshold>, <name>` |
| `[display] alert_dismiss_sec` | seconds the alert screen stays up |
| `[mqtt] broker_port`, `topic` | where user devices receive notifications |

Class indices are listed in `/opt/audio-event/yamnet_class_map.csv`. Thresholds are between 0.0 and 1.0.

## Screen

The screen shows the sounds heard right now, the input level and the most recent events. When a watched sound is detected, a red alert screen appears; touch it to dismiss it, or it closes after `alert_dismiss_sec`. It works in both display modes (Weston and GPU).

## Notifications

The board runs an MQTT broker. To receive notifications, subscribe from a user device on the same network to port 1883 of the board, topic `audio-event`, with any MQTT 3.1.1 client.

Each detected event arrives as one JSON message:

```json
{"device":"CRZ AM62x (Linux)","event_id":17,"class":"Siren","score":0.873,"threshold":0.4,"time":1795064602}
```

`time` is in seconds since 1970-01-01 UTC.

## Optimize

```
sudo aer optimize
sudo aer default
```

`aer optimize` makes recognition faster and steadier:

- the quantized YAMNet model, run with XNNPACK
- inference memory locked in RAM and real-time scheduling for inference
- all AER services on CPUs 1-3; everything else and the interrupts on CPU 0

`aer default` undoes all of it. Both take effect immediately and are kept across reboots. `aer status` shows which one is active.

Notes:

- The quantized model scores sounds a little differently.
- In optimize mode, other programs on the board share CPU 0 only.

Every inference time is logged, in microseconds, to `/var/log/audio-event/inference_latency.log`.
