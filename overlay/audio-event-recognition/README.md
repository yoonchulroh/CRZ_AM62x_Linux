# Audio event recognition (AER)

Recognises abnormal sounds (sirens, breaking glass, alarms, explosions,
screams) with YAMNet on the CRZ AM62x board, shows them on the LCD and
publishes them over MQTT.

## Pipeline

Each box is its own program and systemd service. They pass data over
AF_UNIX `SOCK_SEQPACKET` sockets at `/run/aer/<module>/<module>.sock`.

```
 COMS CM420 (USB)
      |
 [1] aer-capture     ALSA hw:CM420,0, S16_LE stereo 48 kHz, 50 ms chunks
      |
 [2] aer-preprocess  stereo -> mono, int16 -> float32 [-1, 1],
      |              polyphase resampling to 16 kHz
 [3] aer-framer      15600-sample (0.975 s) windows every 7680 samples (0.48 s)
      |
 [4] aer-infer       YAMNet (TensorFlow Lite 2.14 C API) -> 521 class scores
      |
 [5] aer-detect      event score = max over the event's classes; fire when it
      |              reaches the threshold (cooldown per event type)
      +--> [6-1] aer-display  full-screen dashboard on Weston (LCD)
      +--> [6-2] aer-notify   MQTT PUBLISH -> aer-broker (port 1883)
                                               ^
                              user devices subscribe here
```

* A producer accepts any number of consumers. `aer-detect` feeds both outputs.
* Consumers reconnect automatically, so modules can start, crash and restart in
  any order.
* Sends never block. A consumer that falls behind misses messages, but capture
  never stalls.

0.975 s is the fixed input of the YAMNet TFLite classification model: 96 STFT
hops of 10 ms plus one 25 ms window, which is 15600 samples at 16 kHz.

## MQTT

`aer-broker` is a small MQTT 3.1.1 broker on the board, listening on port
1883 of all interfaces. `aer-notify` publishes one message per detected event
to `aer/<device>/event` with QoS 1:

```json
{"time":"2026-10-17T13:20:01.123+09:00","device_name":"mango","event_type":"siren","score":0.852}
```

* `time`: local time of the analysed audio frame (ISO 8601).
* `device_name`: `[general] device_name`, or the hostname if that is empty.
* `event_type`: name of the `[event ...]` section that fired.
* `score`: YAMNet score (0..1).

On the user device (e.g. 192.168.0.162), subscribe to the board:

```sh
mosquitto_sub -h 192.168.0.227 -t 'aer/#' -v
```

Any MQTT 3.1/3.1.1 client works, including phone apps such as MQTT Dash or IoT
MQTT Panel. The broker supports:
* QoS 0/1 delivery (QoS 2 subscriptions are granted QoS 1)
* `+` / `#` wildcards
* retained messages
* last will
* keep alive

It does not support persistent sessions, MQTT 5, TLS or authentication: anyone
on the LAN can subscribe and publish.

## Building

`./build.sh aer` (also part of `./build.sh all`) does three things:
1. On the first run, `scripts/fetch-deps.sh` downloads into `third_party/`:
   * the YAMNet model, checked against its sha256;
   * the arm64 TensorFlow Lite headers and libraries from the Ubuntu 26.04
     archive. These are used only for compiling and linking.
2. It cross-compiles the eight programs with the SDK toolchain.
3. It installs into `image/custom_datafs`, which `create-sdcard_ubuntu.sh`
   unpacks onto the SD card rootfs:

| Path on the board | Contents |
|---|---|
| `/usr/local/bin/aer-*` | the programs |
| `/usr/local/share/aer/` | `yamnet.tflite`, `yamnet_labels.txt`, this file |
| `/etc/aer/aer.conf` | configuration |
| `/etc/systemd/system/aer*.{target,service}` | units, enabled via `aer.target` (multi-user) |

The board's runtime libraries come from apt in
`ubuntu-26.04/custom_datafs/data/create_ubuntu26.04_arm.sh`:
* `libtensorflow-lite2.14.1`
* `libasound2t64`
* `libwayland-client0`
* `libcairo2`
* `fonts-dejavu-core`

A rootfs built before this change does not have TensorFlow Lite, so run
`./build.sh rootfs` once.

Full flow: `./build.sh` (or `./build.sh rootfs` + `./build.sh aer`), then
`cd image && sudo ./create-sdcard_ubuntu.sh`.

## Services

```sh
systemctl status 'aer-*'                 # all modules
sudo systemctl restart aer.target        # restart the pipeline (e.g. after editing aer.conf)
sudo systemctl stop aer.target           # stop everything
journalctl -u aer-detect -f              # detected events
journalctl -u aer-capture -u aer-infer   # device / model problems
```

* All modules except the display run as transient `DynamicUser` accounts.
  `aer-capture` also gets the `audio` group.
* `aer-display` runs as `mango`, the Weston user. It finds Weston's socket in
  `/run/user/<uid>` itself (Weston 14 uses `wayland-1`) and waits until the
  compositor is up.

## Configuration

Edit `/etc/aer/aer.conf`, then restart `aer.target`. Event types are sections:

```ini
[event siren]
threshold = 0.3
classes = Siren | Civil defense siren | Police car (siren) | Ambulance (siren) | Fire engine, fire truck (siren)
```

* Class names come from `/usr/local/share/aer/yamnet_labels.txt`, separated by
  `|`; class numbers also work.
* The defaults are siren, breaking, alarm, explosion and scream, all at
  threshold 0.3 with a 5 s cooldown per type.
* "Alarm" is the parent class of "Siren" in AudioSet, so a siren usually
  reports both `siren` and `alarm`. Remove `Alarm` from `[event alarm]` if
  that is not wanted.

## Testing without the microphone

Every module takes:
* `-c <config>`
* `-v` for per-frame logging

The `AER_RUN_DIR` environment variable moves the sockets, so a second pipeline
can run next to the services.

`aer-capture` can replay a 16-bit PCM WAV file (any rate and channel count) in
real time instead of the microphone:

```sh
sudo systemctl stop aer-capture
sudo /usr/local/bin/aer-capture -w siren.wav     # add -l to loop
sudo systemctl start aer-capture
```

## Source layout

| Directory | Contents |
|---|---|
| `common/` | `aer.h` (message formats), `ipc.c` (sockets), `conf.c` (INI reader), `mqtt.c` (MQTT packets), `util.c` |
| `capture/` … `broker/` | one program per directory |
| `config/aer.conf`, `systemd/` | installed as described above |
| `scripts/fetch-deps.sh` | model and TensorFlow Lite download |
