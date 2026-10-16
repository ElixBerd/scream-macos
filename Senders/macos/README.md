# Scream for macOS

Adds a **Scream Audio** output device to the Mac. Pick it in the Sound menu
(or in any app) and everything played through it goes onto the LAN as a
standard Scream stream: 5 byte header + up to 1152 bytes of LSB PCM,
multicast `239.255.77.77:4010` by default. Any Scream receiver works
(Unix/PulseAudio/ALSA/JACK, Windows ScreamReader, ESP32, ...).

Works on Apple Silicon and Intel, macOS 12 or newer.

## Install

```sh
cd Senders/macos
make
sudo make install
```

`make install` copies `ScreamAudio.driver` to `/Library/Audio/Plug-Ins/HAL`,
`screamctl` to `/usr/local/bin`, and restarts Core Audio (sound drops for a
second). Needs the Xcode Command Line Tools (`xcode-select --install`).

Then choose **Scream Audio** in System Settings > Sound > Output, or in the
menu bar sound icon. Volume keys and mute work on the device.

Uninstall with `sudo make uninstall`.

## Choosing the receiver

The device streams multicast out of the box. To send to one receiver:

```sh
screamctl -u 192.168.1.50     # unicast to one receiver
screamctl -g                  # back to multicast 239.255.77.77
screamctl                     # show current settings
screamctl -d                  # make Scream Audio the default output
```

| Option | Meaning |
|---|---|
| `-u ADDR` | unicast destination |
| `-g [ADDR]` | multicast group (default 239.255.77.77) |
| `-p PORT` | UDP port (default 4010) |
| `-b BITS` | 16, 24 or 32 (default 16) |
| `-s BYTES`| PCM bytes per packet, 6..1152 (default 1152) |
| `-t TTL`  | multicast TTL (default 1) |
| `-d` | make Scream Audio the default output device |

Settings are stored by the driver and survive reboots. The sample rate
(44.1/48/88.2/96 kHz) is set in Audio MIDI Setup like for any device.

## How it works

`ScreamAudio.driver` is a user-space Core Audio driver (AudioServerPlugIn),
loaded by `coreaudiod`. No kernel extension, no SIP changes.

- The device clock runs on `mach_absolute_time()`.
- On each IO cycle Core Audio hands the driver the final mix of all apps. The
  driver converts it to 16/24/32 bit PCM (TPDF dither for 16 bit) and sends
  it right away with a non-blocking `sendto()`. Nothing is queued besides the
  packet being filled, so the Mac adds roughly one IO buffer of delay.
- Packets are marked DSCP EF / `NET_SERVICE_TYPE_VO` so Wi-Fi sends them in
  the voice queue.

## Latency tuning

End-to-end delay is mostly the receiver's buffer, then the packet size.

- Smaller packets cut packetization delay: `screamctl -s 288` sends 72 stereo
  frames (1.5 ms at 48 kHz) instead of 288 (6 ms). The Unix receiver accepts
  short packets; other receivers may assume 1152 byte packets, so test first.
- On the Unix receiver, lower its target latency: `scream -o pulse -t 10` or
  `scream -o alsa -t 10`.
- Prefer wired Ethernet, or unicast on Wi-Fi: many access points send
  multicast at the lowest basic rate, which adds jitter and loss.

## Process-tap sender (alternative)

`build/scream-sender` captures the whole system mix with a Core Audio process
tap (macOS 14.2+) without switching outputs, so the Mac's own speakers keep
playing. Run `build/scream-sender -h` for options. It asks for System Audio
Recording permission on first run; without it the tap delivers silence.
