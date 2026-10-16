# Scream for macOS

Adds a **Scream Audio** output device to the Mac. Pick it in the Sound menu
(or in any app) and everything played through it goes onto the LAN as a
standard Scream stream: 5 byte header + up to 1152 bytes of LSB PCM,
multicast `239.255.77.77:4010` by default. Any Scream receiver works
(Unix/PulseAudio/ALSA/JACK, Windows ScreamReader, ESP32, ...).

Works on Apple Silicon and Intel.

## Install

1. Download `Scream-<version>.pkg` from the
   [Releases page](https://github.com/ElixBerd/scream-macos/releases) and
   double-click it.
2. The package is not notarized by Apple yet, so macOS may refuse to open it.
   Open System Settings > Privacy & Security, scroll down and click
   **Open Anyway** next to the Scream message, then run the installer again.
3. Sound stops for a second while Core Audio reloads. Scream then appears in
   the menu bar, switches your output to **Scream Audio** and opens at every
   login.

Requires macOS 13 or newer (the driver alone works on macOS 12).

## The menu bar app

Click the radio-waves icon in the menu bar:

- **Stream to Network** (⌘S) switches the Mac's output to Scream Audio, or
  back to the speakers/headphones you used before. The icon is dimmed while
  not streaming.
- **Receiver**: *All Receivers* (multicast, the default) or a specific IP.
  Recent IPs are remembered. For a specific IP, start the receiver with
  `scream -u`.
- **Latency**: packet size. *Lowest* (1.5 ms at 48 kHz) gives the least delay,
  *Standard* (6 ms) works with every receiver.
- **Quality**: 16/24/32-bit and 44.1 to 96 kHz.
- **Open at Login**, **Uninstall Scream…**

Volume keys and mute work on the Scream Audio device. All settings are kept
by the driver and survive reboots, even if the app is not running.

## Build from source

```sh
cd Senders/macos
make            # driver, screamctl, Scream.app, tap sender
make pkg        # build/Scream-<version>.pkg
sudo make install
```

Needs the Xcode Command Line Tools (`xcode-select --install`). Pushing a tag
`v*` builds the pkg in GitHub Actions and attaches it to a release.

Uninstall with **Uninstall Scream…** in the menu or `sudo scream-uninstall`
(also works after the app was dragged to the Trash).

## Command line

`screamctl` changes the same settings from Terminal:

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
- The stream's physical format is 16, 24 or 32 bit integer PCM, which is
  exactly what Scream sends, so it shows up in Audio MIDI Setup's Format menu
  and changing it there changes the network format. Core Audio converts its
  float mix to that format; the driver applies volume, mute and 16 bit TPDF
  dither just before.
- On each IO cycle the driver copies the converted mix into packets and sends
  them right away with a non-blocking `sendto()`. Nothing is queued besides
  the packet being filled, so the Mac adds roughly one IO buffer of delay.
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
