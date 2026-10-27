# FlightWall

FlightWall is a self-contained live-aircraft display for a Raspberry Pi Pico
2 W and four HUB75 LED matrix panels. It retrieves nearby aircraft from the
adsb.fi open-data API and presents a rotating traffic list, a detailed nearest-
aircraft screen, and an animated radar display.

Current source release: **v1.6.5**

Download the USB installer: [FlightWall-v1.6.5.uf2](release/FlightWall-v1.6.5.uf2).
The USB image is built as a normal RP2350 image; automatic OTA images retain
Try-Before-You-Buy rollback protection. Devices on v1.6.1 or later can receive
v1.6.3 through the signed automatic-update feed.

## Hardware

- Raspberry Pi Pico 2 W
- SEENGREAT RGB Matrix Adapter Board V3.9
- Four chained 64x32, 1/16-scan HUB75 panels
- 2x2 physical arrangement producing a 128x64 display
- A suitable regulated power supply for each panel

The data chain begins at the bottom-right panel and proceeds clockwise:

1. Controller to bottom-right
2. Bottom-right to bottom-left
3. Bottom-left to top-left
4. Top-left to top-right

GPIO mapping:

| HUB75 signal | Pico GPIO |
| --- | ---: |
| R1, G1, B1, R2, G2, B2 | GP2-GP7 |
| A, B, C, D | GP8-GP11 |
| E | Unused |
| Clock | GP13 |
| Latch | GP14 |
| Output enable | GP15 |

## Features

- Browser-based Wi-Fi provisioning and saved-network management
- Configurable location, range, brightness, refresh timing, and screen timing
- Seven aircraft rows per page across the full four-panel display
- Nearest-aircraft details with compact airline marks
- Animated radar sweep and traffic history
- Watched-aircraft and transponder-emergency alerts
- Startup quadrant test and display load test
- Browser-uploaded A/B firmware updates with rollback-friendly boot behavior
- Signed GitHub updates with automatic or user-controlled installation

Configuration is stored in flash on the Pico; Wi-Fi credentials and personal
location data are not compiled into the source or included in this repository.
The factory location is the public location of Capital Region International
Airport (KLAN) and can be changed on the setup page.

## Build

Prerequisites:

- Raspberry Pi Pico SDK 2.3.0
- ARM GNU embedded toolchain with Cortex-M33/RP2350 support
- CMake and Python 3

Configure and build:

```sh
PICO_SDK_PATH=/path/to/pico-sdk \
PICO_TOOLCHAIN_PATH=/path/to/arm-toolchain \
cmake -S . -B build

cmake --build build --target hub75_demo -j2
```

The firmware image is written to `build/hub75_demo.uf2`.

Development builds are hashed but unsigned. For a signed release, keep the
private key outside the repository and pass it during configuration:

```sh
cmake -S . -B build \
  -DFLIGHTWALL_SIGNING_KEY=/secure/path/to/flightwall-private.pem
```

See [SECURITY.md](SECURITY.md) and [update/README.md](update/README.md) before
distributing signed or automatic updates.

## Documentation

- [v1.6.5 release notes](RELEASE-v1.6.5.md)
- [v1.6.4 release notes](RELEASE-v1.6.4.md)
- [v1.6.3 release notes](RELEASE-v1.6.3.md)
- [v1.6.2 release notes](RELEASE-v1.6.2.md)
- [v1.6.1 release notes](RELEASE-v1.6.1.md)
- [v1.6.0 release notes](RELEASE-v1.6.0.md)
- [FlightWall source and build notes](FLIGHTWALL-SOURCE-BACKUP.md)
- [Upstream HUB75 driver documentation](https://github.com/JuPfu/hub75)

## Credits and license

FlightWall is built on Jürgen Pfuetzenreuter's
[HUB75 DMA/PIO driver](https://github.com/JuPfu/hub75). The original driver and
this repository are distributed under the MIT License; see [LICENSE](LICENSE).
